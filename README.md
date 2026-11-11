# winfsp-pipe-bridge

**Experimental spike.** Exposes a Windows named pipe as a read-only file on a
[WinFsp](https://github.com/winfsp/winfsp) volume, so tools that only know how
to open a file path can read a secret that a broker serves over a pipe. The
content is fetched from the pipe each time the file is opened for reading and
is never written to disk.

It is an independent personal project. It knows nothing about any particular
server: it reads the pipe as an ordinary byte stream, or, with
`--reader-info-suffix`, speaks the small protocol described below.

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

### Telling the server who opened the file (`--reader-info-suffix`)

By default the pipe's server sees this program as the reader, because it is the
pipe's client. A server that wants to know (and ask the user about) the program
that actually opened the file can serve a second pipe, and you point the bridge
at it with a suffix:

```
winfsp-pipe-bridge.exe \\.\pipe\secrets K: \secrets\app\prod.env --reader-info-suffix -reader
```

The bridge then opens `\\.\pipe\secrets-reader` (the pipe name plus the suffix),
and on every open that requests data it sends the caller's PID and process
creation time, taken in `Open` where WinFsp reports them. The server answers for
that caller. This is a request, not a hint: if the second pipe is missing or the
server refuses, the open fails. The bridge never falls back to the data pipe,
because that would make the bridge the reader.

Protocol, version 1. Every message is a frame: a 4-byte big-endian body length,
one tag byte, then a JSON body.

| Direction | Tag | Body |
|---|---|---|
| bridge to server | 72 | `{"protocol":1,"reader_pid":<u32>,"reader_start_time":<u64>}` (start time is the process creation FILETIME) |
| server to bridge | 73 | `{"status":"ok"}` or `{"status":"denied"}` |

After an `ok` response the server writes the content as raw bytes, unframed, and
closes the pipe. After `denied`, or if the server closes the pipe without a
response, the open fails with access denied. The server should verify the
caller itself (the PID is still alive and was created at the stated time) and
decide whether to trust the bridge; the bridge only reports what WinFsp told it.

A server should only believe the caller information from a bridge it
recognises, for example by looking up the pipe client's process and checking the
SHA-256 of its executable (`Get-FileHash winfsp-pipe-bridge.exe`) against a list
it trusts. The bridge is not signed, so such a list can only pin this exact
file: a rebuilt or updated bridge has a new hash and has to be trusted again.

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
locally. The binary is not Authenticode-signed (Windows SmartScreen may warn),
and WinFsp itself is not bundled.

### Verifying a release

Each release's zip and `SHA256SUMS` carry a signed
[GitHub build attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations),
which proves they were built by this repository's release workflow from the
tagged commit (releases before v0.1.2 have none):

```
gh attestation verify winfsp-pipe-bridge-<version>-windows-x64.zip -R kihneman/winfsp-pipe-bridge
sha256sum -c SHA256SUMS
```

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

Defender/indexer behaviour, multiple files, a service wrapper, an installer.

## Licensing

MIT (see `LICENSE`). This program links to the WinFsp DLL under WinFsp's
[FLOSS exception](https://github.com/winfsp/winfsp/blob/master/License.txt):
it must stay under an open-source licence and must not be linked or
distributed together with proprietary software. Distribute WinFsp itself only
as the unmodified installer released by the WinFsp project.
