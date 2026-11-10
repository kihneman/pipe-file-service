# winfsp-pipe-bridge

**Experimental spike.** Exposes a Windows named pipe as a read-only file on a
[WinFsp](https://github.com/winfsp/winfsp) volume, so tools that only know how
to open a file path can read a secret that a broker serves over a pipe. The
content is fetched from the pipe each time the file is opened for reading and
is never written to disk.

It was written to try the idea with a pipe served by Keeper's property-file
mounts on Windows. It is an independent personal project, not a Keeper product,
and contains no Keeper code: it only reads the pipe as an ordinary byte stream.

> WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
> https://github.com/winfsp/winfsp

## How it works

```
winfsp-pipe-bridge.exe \\.\pipe\<name> <mountpoint> [path on the volume]
```

Mounts drive `K:` (or `*` to pick a free letter) containing one file. The file
is at `\property-file.env` unless you give a path, which may have any number of
directories before the file name:

```
winfsp-pipe-bridge.exe \\.\pipe\secrets K: \secrets\app\prod.env
```

serves `K:\secrets\app\prod.env`. The directories (`secrets`, `app`) exist only
on the volume, are read-only, and contain nothing else. Separators may be `\`
or `/`, the leading separator is optional, and names are matched
case-insensitively. Empty components, `.` / `..`, a trailing separator,
characters that are invalid in Windows names (`< > : " | ? *`), and paths deeper
than 32 components are rejected with exit code 2.

Opening the file for reading connects to the pipe, reads until EOF and serves
those bytes. If the pipe's server answers with its standard "Access denied" text
the open fails with `STATUS_ACCESS_DENIED`. For every open the bridge logs the
caller's PID and executable path, which WinFsp reports at open time.

## Build

Requires the Visual Studio C++ x64 build tools (`build.ps1` finds them itself)
and the WinFsp SDK (headers and import library):

```powershell
.\build.ps1                              # uses an installed WinFsp (Developer feature)
.\scripts\get-winfsp-sdk.ps1             # or fetch just the SDK, no driver install
.\build.ps1 -WinFsp .\.winfsp-sdk\DYNAMIC
```

`get-winfsp-sdk.ps1` downloads a pinned WinFsp release MSI, checks its SHA-256 and
extracts it without installing anything. The result links the C runtime
statically (no Visual C++ redistributable) and loads `winfsp-x64.dll` at run
time from the installed WinFsp, printing "WinFsp is not installed" if it is
missing. `winfsp-pipe-bridge.exe --version` works without WinFsp.

## Releases

Releases are built by GitHub Actions when a tag is pushed. The tag must match
`BRIDGE_VERSION` in `src\winfsp-pipe-bridge.c`: `v0.1.0` for a release,
`v0.1.0-rc.1` (any `-suffix`) for a pre-release.

```powershell
git tag v0.1.0-rc.1 && git push origin v0.1.0-rc.1
```

The workflow attaches a zip (exe, `LICENSE`, `README.md`) and `SHA256SUMS`.
`.\scripts\release.ps1 -Tag v0.1.0-rc.1` builds the same files into `dist\`
locally. The binary is not code-signed, and WinFsp itself is not bundled.

## Test

`tests\smoke.ps1` serves a pipe from the script itself, mounts it, and checks
reads, nested and default paths, case-insensitive lookup, rejected paths, refused
writes and unmount. It needs WinFsp, a built `winfsp-pipe-bridge.exe` and a free
drive letter (default `K`):

```powershell
.\tests\smoke.ps1
```

## Findings so far

- Works with `Get-Content`, `cmd /c type`, Python and Node.
- The caller's PID and image path are available in `Open` only; `Read` sees
  PID 0. Approval-style checks therefore belong at open time.
- Programs open a file several times with attribute-only access before the data
  open. The bridge only reads the pipe for opens that request read access, so a
  single command causes one pipe read.
- `stat` reports size 0 (the size is unknown until a data open). Readers that
  read to end of file are fine; tools that trust the stat size may read nothing.
- WinFsp refuses to dispatch creates unless the interface table has `Create`
  and `Overwrite`, even for a read-only volume (here they refuse writes).
- `FileSystemName` holds 16 wide characters including the terminator; a longer
  name makes the CRT fast-fail (`0xC0000409`) with no message.
- The volume's security descriptor allows only the current user and SYSTEM, so
  other users cannot trigger reads through the bridge.

## Not done

Real caller identity forwarding to the pipe's server, Defender/indexer
behaviour, multiple files, a service wrapper, an installer, tests.

## Licensing

MIT (see `LICENSE`). This program links to the WinFsp DLL under WinFsp's
[FLOSS exception](https://github.com/winfsp/winfsp/blob/master/License.txt):
it must stay under an open-source licence and must not be linked or
distributed together with proprietary software. Distribute WinFsp itself only
as the unmodified installer released by the WinFsp project.
