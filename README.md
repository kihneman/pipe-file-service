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

## FspPipeFileService

A Windows service for apps that cannot start a program of their own, or that need a
file *link*. It is the privileged counterpart of `winfsp-pipe-bridge.exe`: one
long-running service, on one WinFsp volume, that any local user's app can ask to

1. show a named pipe as a read-only file (the **data server** is whatever serves the pipe), and
2. turn an empty placeholder file the app made into a symlink to that file. Only a
   privileged process may create file symlinks without Developer Mode.

```
FspPipeFileService.exe install [--mount <folder that does not exist, or a drive letter>] [--auto-start]   (elevated)
FspPipeFileService.exe uninstall                                                                          (elevated)
FspPipeFileService.exe run [--mount <...>] [--winfsp-dir <folder>]    run in this console (elevated), for development
```

The default mount is `C:\ProgramData\FspPipeFileService\mount` (a WinFsp mount
folder must not exist; the service creates the parent and removes an empty
leftover). The service is `FspPipeFileService`, display name "WinFsp Pipe File
Service", runs as LocalSystem with a minimal privilege list
(`SeCreateSymbolicLinkPrivilege`, `SeImpersonatePrivilege`, `SeChangeNotifyPrivilege`)
and is started on demand, or at every boot with `install --auto-start`. It writes
`C:\ProgramData\FspPipeFileService\service.log`.

### The volume

Flat and read-only: each registration is one file in the volume root, named

```
<pipe name without \\.\pipe\>-<requested file name>
```

Characters that are invalid in file names are replaced by `_` in the pipe part. The
whole requested name survives (`.env`, `.tar.gz`), but **the name on the volume is
not the exact requested name**, so a tool that follows the link and matches on the
exact file name will not match. A registration is refused when:

- the pipe name has a sub-path (a backslash after `\\.\pipe\`);
- the requested name is not one path component, contains characters invalid in
  Windows names, ends in a dot or a space, is `.`/`..`, or is a reserved device name;
- the combined name exceeds 255 characters;
- the pipe, **or the final combined name**, is already registered (`foo-bar` + `baz`
  and `foo` + `bar-baz` produce the same name).

Each file carries a security descriptor that lets only the user who registered it
(and SYSTEM) open it. The names of all registrations are visible to every
authenticated user in the root listing; the content is not.

Reading: on an open that requests data the service opens the data pipe **as the
registering user** (it keeps an impersonation token from the registering
connection, because such a pipe is normally owner-only) and serves what it reads.
With a reader-info suffix it speaks the protocol above on `<pipe><suffix>` and
passes the real caller's PID and start time itself. Attribute-only opens never fetch.

### Control pipe

`\\.\pipe\FspPipeFileService`: message mode, local clients only, writable by
interactive users. One JSON object per message, flat (no nesting). Identity is
always taken from the connection (`ImpersonateNamedPipeClient`), never from a field.
A connection sees and changes only its own registrations. **When the connection
closes, or the service stops, everything it registered is removed, including links.**

Requests (all may carry a numeric `id`, which is echoed in the reply):

| `type` | fields |
| --- | --- |
| `register` | `pipe` (full pipe name), `file_name`, optional `reader_info_suffix`, optional `link_path` |
| `unregister` | `pipe` |

Replies have `"type":"reply"` and `"status":"ok"` or `"error"`. On success
`register` returns `path` (the file inside the mount) and `linked`. Errors carry
`code` (`bad_request`, `bad_name`, `pipe_unreachable`, `name_in_use`, `link_refused`,
`not_found`, `driver_not_installed`) and a `message`. The service pushes events, `"type":"event"`, with
`event` of `opened`, `denied` or `error`, plus `file` and `reader_pid`.

Before accepting a registration the service opens the pipe it would read **as the
caller** and refuses if that fails, so a caller cannot register a pipe it cannot
reach, and a refusal reveals nothing about other users' pipes. This opening is a
real, brief connection to the data server (or to the reader-info pipe when a suffix
is given); a server should expect, and quickly drop, a client that connects and
sends nothing.

### Links (the placeholder-file protocol)

The app creates an **empty file** at the destination as itself and passes it as
`link_path` (an absolute path such as `C:\dir\file`). The service never acts on a
path later:

1. As the caller, it opens the placeholder without following links, with only
   attribute access. A file the caller cannot write fails here.
2. On that handle it checks: regular file (not a directory or a link), size 0, link
   count 1, and the owner is the caller's user (or Administrators when the caller's
   token is elevated). Anything else is refused with the reason, and nothing is touched.
3. It reopens the same file **by file ID** as the service, confirms the ID, and sets
   the symlink reparse data there. (Windows checks the symlink privilege against the
   identity that opened the handle, so the handle opened as the caller cannot be used;
   see `docs/placeholder-link-findings.md`.)
4. When the registration ends, it removes the link if it is still a symlink to the
   target it set, and leaves the empty file.

### Install

The service needs WinFsp installed, but it is safe to install it on a machine
that does not have WinFsp (for example from an app installer that fetches WinFsp
later, only if the user agrees):

- **At start, WinFsp missing:** the service logs one line to `service.log`, opens
  **no control pipe**, and stops with exit code 0. The Service Control Manager
  logs no failure and runs no recovery action. The start type is left alone, so
  with `install --auto-start` it tries again at every boot. Nothing polls and
  nothing listens while WinFsp is missing: **the control pipe does not exist until
  WinFsp is installed and the service is started.**
- **After installing WinFsp:** start the service again with an elevated
  `sc start FspPipeFileService` (or reboot). There is no wake-up message.
- **WinFsp removed while the service runs:** `register` answers
  `driver_not_installed` and registers nothing. Registrations that already exist
  are left alone; they stop working by themselves (reads fail) and are cleaned up
  when their connection closes or the service stops.
- "WinFsp is installed" means: the registry `InstallDir` under
  `HKLM\SOFTWARE\WOW6432Node\WinFsp` has `bin\winfsp-x64.dll`, and a `WinFsp` file system
  driver service is registered (WinFsp 2.x names it `WinFsp+<timestamp>`). The check
  loads no WinFsp code.
- For tests, `run --winfsp-dir <folder>` makes the check look for
  `<folder>\bin\winfsp-x64.dll` instead. It exists only in `run` mode.

### Installing WinFsp silently (for an installer or app)

Use the unmodified WinFsp installer from the WinFsp project (verify its SHA-256
and its Authenticode signature, signer `NAVIMATICS LLC`, before running it). From
an elevated process:

```
msiexec /i winfsp-2.1.25156.msi /qn /norestart ADDLOCAL=F.Main,F.User /l*v C:\path\winfsp-install.log
```

`F.User` is the driver, `winfsp-x64.dll`, the `WinFsp.Launcher` service (it
cannot be left out; this project does not use it) and the command-line tools. The
other features (`F.Developer`, `F.KernelDeveloper`, `F.Cygfuse`) are headers,
samples and Cygwin support and are off by default.

Exit codes: `0` success, `3010` success and a restart is requested, `1618`
another installation is running (wait and retry), `1603` fatal error. From the
installer's own tables: a newer WinFsp already installed makes it refuse with
`1603` ("A newer version of WinFsp is already installed"; read the log to tell it
apart), and an older one is upgraded in place.

Afterwards, check: `HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir` exists with
`bin\winfsp-x64.dll` in it, a `WinFsp+...` file system driver service is registered (`sc query type= driver state= all`), and the
`WinFsp.Launcher` service is running. Then `sc start FspPipeFileService`.

Uninstall (the product code changes with every WinFsp version; find it from the
upgrade code `{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}`):

```
msiexec /x {product-code} /qn /norestart /l*v C:\path\winfsp-uninstall.log
```

`tests\install-chain.ps1` runs this whole sequence on a clean VM.

The WinFsp installer also installs and starts
`WinFsp.Launcher`, a separate service this project does not use. The service does not
use a restricted service SID: that makes the token write-restricted and it then
cannot create its folder or reopen a user's placeholder.

Tests (`tests\service-up.ps1` and `tests\service-down.ps1` run elevated;
`tests\service-smoke.ps1` runs as an ordinary user, the realistic caller):

```powershell
.\tests\service-up.ps1        # elevated: install + start
.\tests\service-smoke.ps1     # unelevated
.\tests\service-down.ps1      # elevated: stop + uninstall + check nothing is left
```

`tests\driver-missing.ps1` checks the WinFsp-missing behaviour with `--winfsp-dir`
(the first part runs unelevated; the rest needs an elevated shell and no running
service). `tests\install-chain.ps1` is for a clean VM (elevated): service installed
before WinFsp, WinFsp installed silently, WinFsp removed again.

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
missing. `winfsp-pipe-bridge.exe --version` works without WinFsp. The same script builds
`FspPipeFileService.exe`.

## Releases

Releases are built by GitHub Actions when a tag is pushed. The tag must match
`BRIDGE_VERSION` in `src\winfsp-pipe-bridge.c`: `v0.1.0` for a release,
`v0.1.0-rc.1` (any `-suffix`) for a pre-release.

```powershell
git tag v0.1.0-rc.1 && git push origin v0.1.0-rc.1
```

The workflow attaches a zip (both exes, `LICENSE`, `README.md`) and `SHA256SUMS`.
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

Defender/indexer behaviour, an installer (the service is not packaged, signed or chained with WinFsp), and a share of the volume across users beyond what is described above.

## Licensing

MIT (see `LICENSE`). This program links to the WinFsp DLL under WinFsp's
[FLOSS exception](https://github.com/winfsp/winfsp/blob/master/License.txt):
it must stay under an open-source licence and must not be linked or
distributed together with proprietary software. Distribute WinFsp itself only
as the unmodified installer released by the WinFsp project.
