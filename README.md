# pipe-file-service

> Formerly `winfsp-pipe-bridge` (programs `winfsp-pipe-bridge.exe` and `FspPipeFileService.exe`, service
> `FspPipeFileService`). They are now `pipe-file-mount-<driver>.exe` and `PipeFileService-<driver>.exe` (service
> `PipeFileService`); `PipeFileService-<driver>.exe install` removes a service still registered under the old name.

Shows a Windows named pipe as a read-only file, so tools that only know how to open a file path can read a
secret that another program serves over the pipe. The file sits on a small virtual volume provided by a
user-mode file system driver, [WinFsp](https://github.com/winfsp/winfsp) or
[Dokan](https://github.com/dokan-dev/dokany). The content is fetched from the pipe on the first read of each
open handle and is never written to disk (the ProjFS service build is the exception, see [ProjFS](#projfs-the-service-only)).

There are two programs, each built once per driver (`-winfsp` and `-dokan`; see
[Backends](#backends-winfsp-dokan-or-projfs)):

- `pipe-file-mount-<driver>.exe` mounts one pipe as one file, in the process that runs it
  ([How it works](#how-it-works));
- `PipeFileService-<driver>.exe` is a Windows service that does it for any local user's app and can link
  placeholder files to the pipes ([PipeFileService](#pipefileservice)).

It is an independent personal project, version 0.x and experimental. It knows nothing about any particular
server: it reads the pipe as an ordinary byte stream, or, with `--reader-info-suffix`, speaks the small
protocol described below. The drivers are not included; install the one you use.

## How it works

```
pipe-file-mount.exe \\.\pipe\<name> <mountpoint> [path on the volume]
```

(Built per driver as `pipe-file-mount-winfsp.exe` or `pipe-file-mount-dokan.exe`; the examples use the short name. See "Backends" below.)

Mounts drive `K:` (or `*` to pick a free letter) containing one file. The file
is at `\property-file.env` unless you give a path, which may have any number of
directories before the file name:

```
pipe-file-mount.exe \\.\pipe\secrets K: \secrets\app\prod.env
```

serves `K:\secrets\app\prod.env`. The directories (`secrets`, `app`) exist only
on the volume, are read-only, and contain nothing else. Separators may be `\`
or `/`, the leading separator is optional, and names are matched
case-insensitively. Empty components, `.` / `..`, a trailing separator,
characters that are invalid in Windows names (`< > : " | ? *`), and paths deeper
than 32 components are rejected with exit code 2.

The first read on a handle opened for reading connects to the pipe, reads until
EOF and serves those bytes; later reads on the same handle reuse them. Opening
the file and closing it without reading never connects, so programs that only
show or stat the file do not reach the server. If the pipe's server answers with
its standard "Access denied" text the first read fails with
`STATUS_ACCESS_DENIED`, and later reads on that handle repeat the failure without
asking again. Two reads racing on a fresh handle share one fetch. For every open
the mount tool logs the caller's PID and executable path, taken at open time.

### Telling the server who opened the file (`--reader-info-suffix`)

By default the pipe's server sees this program as the reader, because it is the
pipe's client. A server that wants to know (and ask the user about) the program
that actually opened the file can serve a second pipe, and you point the mount tool
at it with a suffix:

```
pipe-file-mount.exe \\.\pipe\secrets K: \secrets\app\prod.env --reader-info-suffix -reader
```

The mount tool then opens `\\.\pipe\secrets-reader` (the pipe name plus the suffix),
and on the first read of every handle opened for data it sends the caller's PID
and process creation time. The caller is taken at `Open` (WinFsp reports it only there,
Dokan on every request), so the mount tool records them there (and keeps a handle on
the process so the PID cannot be reused) and sends them at the first read. The server answers for that caller.
This is a request, not a hint: if the second pipe is missing or the server
refuses, the first read fails (`STATUS_ACCESS_DENIED` when refused,
`STATUS_IO_DEVICE_ERROR` when the server cannot be used). The mount tool never falls back to the data pipe,
because that would make the mount tool the reader.

Protocol, version 1. Every message is a frame: a 4-byte big-endian body length,
one tag byte, then a JSON body.

| Direction | Tag | Body |
|---|---|---|
| mount tool to server | 72 | `{"protocol":1,"reader_pid":<u32>,"reader_start_time":<u64>}` (start time is the process creation FILETIME) |
| server to mount tool | 73 | `{"status":"ok"}` or `{"status":"denied"}` |

After an `ok` response the server writes the content as raw bytes, unframed, and
closes the pipe. After `denied`, or if the server closes the pipe without a
response, the open fails with access denied. The server should verify the
caller itself (the PID is still alive and was created at the stated time) and
decide whether to trust the mount tool; the mount tool only reports what the file system driver told it.

A server should only believe the caller information from a mount tool it
recognises, for example by looking up the pipe client's process and checking the
SHA-256 of its executable (`Get-FileHash pipe-file-mount.exe`) against a list
it trusts. The mount tool is not signed, so such a list can only pin this exact
file: a rebuilt or updated mount tool has a new hash and has to be trusted again.

## PipeFileService

A Windows service for apps that cannot start a program of their own, or that need a
file *link*. It is the privileged counterpart of the mount tool: one
long-running service, on one volume of the file system driver, that any local user's app can ask to

1. show a named pipe as a read-only file (the **data server** is whatever serves the pipe), and
2. turn an empty placeholder file the app made into a symlink to that file. Only a
   privileged process may create file symlinks without Developer Mode.

In this README `PipeFileService.exe` stands for `PipeFileService-winfsp.exe` or `PipeFileService-dokan.exe`, and
`pipe-file-mount.exe` for `pipe-file-mount-winfsp.exe` or `pipe-file-mount-dokan.exe`.

```
PipeFileService.exe install [--mount <folder that does not exist, or a drive letter>] [--auto-start] [limits]   (elevated)
PipeFileService.exe uninstall                                                                                  (elevated)
PipeFileService.exe run [--mount <...>] [--winfsp-dir <folder> | --dokan-dir <folder>] [limits]    run in this console (elevated), for development
```

The default mount is `C:\ProgramData\PipeFileService\mount`. `--mount` names a folder that does not
exist, or a drive letter; the service prepares the folder itself (WinFsp needs a mount folder that does
not exist and Dokan one that exists and is empty, see [Backends](#backends-winfsp-dokan-or-projfs)). The service is `PipeFileService`, display name "Pipe File
Service", runs as LocalSystem with a minimal privilege list
(`SeCreateSymbolicLinkPrivilege`, `SeImpersonatePrivilege`, `SeChangeNotifyPrivilege`)
and is started on demand, or at every boot with `install --auto-start`. It writes
`C:\ProgramData\PipeFileService\service.log`.

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

Reading: on the first read of a handle opened for data the service opens the data
pipe **as the registering user** (it keeps an impersonation token from the
registering connection, because such a pipe is normally owner-only) and serves what
it reads. With a reader-info suffix it speaks the protocol above on
`<pipe><suffix>` and passes the real caller's PID and start time, which it
recorded in `Open`. Attribute-only opens never fetch, and neither do data opens
that close without a read (a program that opens the file to look at it and closes
it); a failed or refused fetch makes that read, and later reads on the handle, fail
without asking the server again.

### Control pipe

`\\.\pipe\PipeFileService`: message mode, local clients only, writable by
interactive users. One JSON object per message, flat (no nesting). Identity is
always taken from the connection (`ImpersonateNamedPipeClient`), never from a field.
A connection sees and changes only its own registrations. **When the connection
closes, or the service stops, everything it registered is removed, including links.**

Requests (all may carry a numeric `id`, which is echoed in the reply):

| `type` | fields |
| --- | --- |
| `register` | `pipe` (full pipe name), `file_name`, optional `reader_info_suffix`, optional `link_path`, optional `size` |
| `unregister` | `pipe` |

Replies have `"type":"reply"` and `"status":"ok"` or `"error"`. On success
`size` declares the length of the content in bytes (a whole number from 0 to 1073741824). With it the file
reports that size from the start, so a program that trusts the reported size works, and stat and listing still
never fetch. The content must then be exactly that long: if the server sends a different number of bytes the read
fails with an I/O error (nothing partial is returned) and an `error` event with `message` `size_mismatch` is
pushed. Without `size` the size is 0 until a handle has read, as before. A bad value is refused with
`bad_request`. The size applies to the registration; to change it, unregister and register again.

`register` returns `path` (the file inside the mount) and `linked`. Errors carry
`code` (`bad_request`, `bad_name`, `pipe_unreachable`, `name_in_use`, `link_refused`,
`not_found`, `driver_not_installed`, `too_many_registrations`) and a `message`. The service pushes events, `"type":"event"`, with
`event` of `opened`, `denied` or `error`, plus `file` and `reader_pid`. The event is
pushed when the content is fetched (the first read), not at open, so browsing a
folder that contains the file produces none.

**Limits** (the pipe is open to every interactive user): a message is at most 64 KB
(a larger one drops the connection); by default at most 64 registrations per
connection and 256 per user, beyond which `register` answers
`too_many_registrations`; at most 128 connections in all (extra ones are hung up on
before any thread is started) and 16 per user. A connection counts for its user from
its first message, so a connection that never sends one is only bounded by the total.
A connection over its user's limit is dropped when it sends its first message.
`unregister`, or closing a connection, frees slots at once. There is no idle timeout.

The four numbers can be changed when the service is installed (`install` keeps them
in the service's `Parameters` key, as DWORD values) or for a development `run`:

| option | default | allowed | registry value |
| --- | --- | --- | --- |
| `--max-reg-per-conn <n>` | 64 | 1 to 10000 | `MaxRegPerConn` |
| `--max-reg-per-user <n>` | 256 | 1 to 100000 | `MaxRegPerUser` |
| `--max-conn <n>` | 128 | 1 to 10000 | `MaxConn` |
| `--max-conn-per-user <n>` | 16 | 1 to 10000 | `MaxConnPerUser` |

The service reads them when it starts, so changing a value later means restarting
the service. A missing value keeps the default; an out-of-range value is logged and
ignored. `run` uses only its own options, never the registry. The effective limits
are logged at start.

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

The service needs its file system driver (WinFsp or Dokan, matching the exe) installed, but it is safe to
install the service on a machine that does not have the driver (for example from an app installer that
fetches the driver later, only if the user agrees):

- **At start, driver missing:** the service logs one line to `service.log` ("WinFsp is not installed" or
  "Dokan is not installed"), opens **no control pipe**, and stops with exit code 0. The Service Control
  Manager logs no failure and runs no recovery action. The start type is left alone, so with
  `install --auto-start` it tries again at every boot. Nothing polls and nothing listens while the driver
  is missing: **the control pipe does not exist until the driver is installed and the service is started.**
- **After installing the driver:** start the service again with an elevated
  `sc start PipeFileService` (or reboot). There is no wake-up message.
- **Driver removed while the service runs:** `register` answers
  `driver_not_installed` and registers nothing. Registrations that already exist
  are left alone; they stop working by themselves (reads fail) and are cleaned up
  when their connection closes or the service stops.
- "The driver is installed" means, for WinFsp: the registry `InstallDir` under
  `HKLM\SOFTWARE\WOW6432Node\WinFsp` has `bin\winfsp-x64.dll`, and a `WinFsp` file system
  driver service is registered (WinFsp 2.x names it `WinFsp+<timestamp>`). For Dokan: `dokan2.dll` is in the
  system folder and the `Dokan2` file system driver service exists. The check loads no driver code.
- For tests, `run --winfsp-dir <folder>` (WinFsp build) makes the check look for
  `<folder>\bin\winfsp-x64.dll` instead, and `run --dokan-dir <folder>` (Dokan build) for
  `<folder>\dokan2.dll`. They exist only in `run` mode.

### Installing a driver silently (for an installer or app)

#### WinFsp

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
`WinFsp.Launcher` service is running. Then `sc start PipeFileService`.

Uninstall (the product code changes with every WinFsp version; find it from the
upgrade code `{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}`):

```
msiexec /x {product-code} /qn /norestart /l*v C:\path\winfsp-uninstall.log
```

`tests\install-chain.ps1` runs this whole sequence on a clean VM.

The WinFsp installer also installs and starts
`WinFsp.Launcher`, a separate service this project does not use.

#### Dokan

Use the unmodified installer from the Dokan project (verify its SHA-256 and its Authenticode signature
before running it; the 2.3.1.1000 installer is signed by `LEOSAC`). From an elevated process:

```
msiexec /i Dokan_x64.msi /qn /norestart /l*v C:\path\dokan-install.log
```

Exit codes: `0` success (no restart was needed), `3010` success and a restart is requested. The installer puts
the `Dokan2` driver service (started, automatic) in place, `dokan2.dll` in the system folder and under
`Program Files\Dokan`, and sets the machine variable `DokanLibrary2`. Developer files are off by default.
Afterwards check that `dokan2.dll` is in the system folder and that `sc query Dokan2` shows it running.
Then `sc start PipeFileService`.

Uninstall: find the product code as the key under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall` whose `DisplayName` starts with
"Dokan Library", then:

```
msiexec /x {product-code} /qn /norestart /l*v C:\path\dokan-uninstall.log
```

It exits `3010`, and Windows keeps the driver service until the next restart. **The Dokan installer refuses to
install again until that restart**, so an installer that removes and reinstalls Dokan needs a restart in
between.

`tests\install-chain.ps1 -Backend dokan` runs the same sequence for Dokan.

#### Both

The service does not use a restricted service SID: that makes the token write-restricted and it then
cannot create its folder or reopen a user's placeholder.

Tests (`tests\service-up.ps1` and `tests\service-down.ps1` run elevated and take `-Backend winfsp|dokan`;
`tests\service-smoke.ps1` runs as an ordinary user, the realistic caller):

```powershell
.\tests\service-up.ps1        # elevated: install + start
.\tests\service-smoke.ps1     # unelevated
.\tests\service-down.ps1      # elevated: stop + uninstall + check nothing is left
```

`tests\limits.ps1` (unelevated, service running) checks the control-pipe limits. `tests\slow-read.ps1`
(unelevated, service running) checks that a read waiting 20 seconds for the data server succeeds and the
volume stays mounted.
`tests\exit-codes.ps1` checks that the test scripts end with an explicit `exit`, so a passing run
reports exit code 0 even when started with `&` or from a CI step.
`tests\driver-missing.ps1 -Backend winfsp|dokan` checks the driver-missing behaviour with `--winfsp-dir` or
`--dokan-dir` (the first part runs unelevated; the rest needs an elevated shell and no running
service). `tests\install-chain.ps1 -Backend winfsp|dokan -DriverInstaller <msi>` is for a clean VM (elevated):
service installed before the driver, driver installed silently, driver removed again; `-RemoveExisting`
uninstalls a driver that is already installed first (Dokan then needs a restart before it can be installed
again, and the script says so).

## Build

Requires the Visual Studio C++ x64 build tools (`build.ps1` finds them itself)
and the SDK of each driver you build for:

```powershell
.\build.ps1                              # WinFsp programs, using an installed WinFsp (Developer feature)
.\scripts\get-winfsp-sdk.ps1             # or fetch just the WinFsp SDK, no driver install
.\build.ps1 -WinFsp .\.winfsp-sdk\DYNAMIC
.\scripts\get-dokan-sdk.ps1              # fetch the Dokan headers and import library, no driver install
.\build.ps1 -Backend dokan               # Dokan programs (uses .\.dokan-sdk)
.\build.ps1 -Backend projfs             # ProjFS service (no driver SDK: uses the Windows SDK)
.\build.ps1 -Backend all -WinFsp .\.winfsp-sdk\DYNAMIC   # every backend
```

`get-winfsp-sdk.ps1` downloads a pinned WinFsp release MSI, checks its SHA-256 and
extracts it without installing anything. The programs link the C runtime
statically (no Visual C++ redistributable) and load their driver's DLL
(`winfsp-x64.dll` or `dokan2.dll`) at run time, printing "WinFsp is not installed" or
"Dokan is not installed" if it is missing. `--version` works without the driver.

Each service exe is built from two parts: `src\pfs_core.c` (control pipe, registrations, limits,
placeholder links, fetch on first read, service install) and one file system driver backend
(`src\pfs_backend_winfsp.c` or `src\pfs_backend_dokan.c`), joined by the interface in
`src\pfs_backend.h`. The mount tool has the same shape (`src\pfm_core.c`, `src\pfm_backend_*.c`,
`src\pfm_backend.h`). A build links exactly one backend, so each exe references exactly one driver.

### Backends: WinFsp, Dokan or ProjFS

`build.ps1 -Backend winfsp|dokan|all` builds `PipeFileService-winfsp.exe` and/or `PipeFileService-dokan.exe`
(and `pipe-file-mount-winfsp.exe` / `pipe-file-mount-dokan.exe`). The two services behave the same: same service name
(`PipeFileService`), same control pipe, same protocol, same file names. Only the driver differs, and each exe
references exactly one: the WinFsp build does not mention Dokan and the other way round, and both delay-load
their driver's DLL. Install whichever driver you ship; `install` and `uninstall` work the same.

The Dokan backend (Dokany 2.x) differs where the driver does:

- The driver is the file system driver service `Dokan2` plus `dokan2.dll` in the system folder; the service
  logs one line, opens no control pipe and exits 0 when either is missing (like WinFsp).
- A Dokan mount folder must exist and be empty, and a clean unmount leaves a stale junction behind. The service
  creates the folder at start and removes the junction at stop, so `--mount` still names a folder that does not
  exist.
- Dokan does not enforce per-file permissions itself. The backend checks the opener's token against the file's
  security descriptor, so the file is readable only by the registering user and SYSTEM, as with WinFsp.
- The driver unmounts a volume when one request stays pending longer than its timeout (15 s by default). A
  first read can wait for the data server (for example for the user to approve the reader), so the backend sets
  the 5-minute maximum and extends it on every read. `tests\slow-read.ps1` covers it.
- `run --dokan-dir <folder>` (development only) replaces `run --winfsp-dir`.

The Dokan `pipe-file-mount` takes the same arguments. A drive letter works as with WinFsp; `*` picks the first free drive letter from Z down and logs it. The volume is visible only to the logon session of the user who started it. A folder as mount point must exist and be empty (it is created if missing).

`scripts\get-dokan-sdk.ps1` fetches the Dokan headers and import library (every download checked against a
pinned SHA-256) without installing the driver.

#### ProjFS (the service only)

`build.ps1 -Backend projfs` builds `PipeFileService-projfs.exe`. It uses the Windows Projected File System, an
optional Windows feature (`Client-ProjFS`) with Microsoft's own library and filter driver: there is no third-party
driver to install or license. Enable it with `Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS
-NoRestart` (elevated; no restart was needed on Windows 11). It is a different kind of file system from the other
two, and **it does not keep their guarantee that the secret is never written to disk**:

- ProjFS projects names into a real folder. When a file is read, the content is written into that folder as an
  ordinary NTFS file. The backend deletes the file again when the handle that read it closes, so the next read
  asks the data server again, but a reader that opens the file while it is still there gets the data from the
  disk without a request to the data server (in a test with two readers, about one read in five), and if the
  service crashes, the file stays on disk until the next start (the service empties the folder at every start).
- The files take the access control of the folder: SYSTEM full control and authenticated users read. The
  registering user's permission is checked, against the process that made the request, only when ProjFS asks the
  service for the placeholder or the data; a file that is already on disk is served by NTFS to anyone the folder's
  access control allows. On a machine with several users this is weaker than the other builds.
- Writes, deletes, renames and new files are refused (the folder allows no writes, and the service vetoes
  delete, rename and hard links).
- The size of the file must be known before the first read, so `register` **requires `size`** in this build and
  answers `size_required` without it.
- `--mount` must name a folder (the default is fine), not a drive letter. The service deletes whatever is in the
  folder at start and removes the folder at stop. There is no ProjFS `pipe-file-mount`.
- The driver check is that `projectedfslib.dll` is in the system folder and the `PrjFlt` service exists.
  `tests\projfs.ps1` covers what is specific to this build.

## Releases

Releases are built by GitHub Actions when a tag is pushed. The tag must match
`MOUNT_VERSION` in `src\pfm_core.c`: `v0.1.0` for a release,
`v0.1.0-rc.1` (any `-suffix`) for a pre-release.

```powershell
git tag v0.1.0-rc.1 && git push origin v0.1.0-rc.1
```

The workflow attaches one zip per driver
(`pipe-file-service-<version>-winfsp-windows-x64.zip` and `...-dokan-windows-x64.zip`, each with its two
exes, `LICENSE` and `README.md`) and one `SHA256SUMS` for both. The release notes list the SHA-256 of every
exe. `.\scripts\release.ps1 -Tag v0.1.0-rc.1` builds the same files into `dist\`
locally (it needs both SDKs). The binaries are not Authenticode-signed (Windows SmartScreen may warn),
and neither driver is bundled.

### Verifying a release

Each release's zips, `SHA256SUMS` and executables carry a signed
[GitHub build attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations),
which proves they were built by this repository's release workflow from the
tagged commit (releases before v0.1.2 have none):

```
gh attestation verify pipe-file-service-<version>-<winfsp|dokan>-windows-x64.zip -R kihneman/pipe-file-service
gh attestation verify PipeFileService-<winfsp|dokan>.exe -R kihneman/pipe-file-service   # an exe taken out of the zip
sha256sum -c SHA256SUMS
```

## Test

`tests\smoke.ps1` serves a pipe from the script itself, mounts it, and checks
reads, nested and default paths, case-insensitive lookup, rejected paths, refused
writes and unmount. It needs the driver of `-Backend` (`winfsp`, the default, or `dokan`), a built `pipe-file-mount-<backend>.exe` and a free
drive letter (default `K`):

```powershell
.\tests\smoke.ps1
```

## Findings so far

- Works with `Get-Content`, `cmd /c type`, Python and Node.
- WinFsp reports the caller's PID and image path in `Open` only; `Read` sees
  PID 0. Dokan reports it on every request (cleanup and close can show pid 4, System, so only the
  open is used). Both programs therefore record the caller in `Open` and use it at the
  first `Read`.
- Programs open a file several times with attribute-only access (0x80) before the
  data open, and some open with read access (for example 0x00120089, a plain
  read open) and close without reading. Fetching at the first read keeps those
  away from the server. Both drivers still deliver reads when the reported size is 0,
  so real readers get their data.
- Measured with a counting server (Windows 11, WinFsp mount tool on a drive letter; the Dokan mount tool
  passes the same fetch-on-first-read test): a
  directory listing (`Get-ChildItem`, `dir`), `Get-Item`, `Test-Path`,
  `FileInfo.Length`, `File.Exists`, `icacls`, `attrib`, a read-open that closes
  without reading, and `Copy-Item` (CopyFile) cause no fetch. Reading programs
  fetch: `Get-Content`, `cmd /c type`, Git for Windows `ls -la` (it opens with
  0x00100001 and reads the first bytes of each file it lists), and also
  `Get-Item | Select-Object *`, because the file's version-info property reads it.
  A File Explorer window on the drive and a Windows Forms open dialog caused no
  open of the file at all in that run (Explorer's details pane and an Electron
  Save dialog were not measured; they were seen to open the file with read access
  before this change).
- Unless the registration declared a `size`, the size is 0 until a handle has fetched (it is then the real length on that
  handle). Readers that read to end of file are fine, but a program that trusts
  the reported size and never reads sees an empty file; `Copy-Item` (CopyFile) is
  one, so copy by reading (`Get-Content`, a stream) instead.
- WinFsp refuses to dispatch creates unless the interface table has `Create`
  and `Overwrite`, even for a read-only volume (here they refuse writes).
- WinFsp's `FileSystemName` holds 16 wide characters including the terminator; a longer
  name makes the CRT fast-fail (`0xC0000409`) with no message.
- Dokan: the directory fill function always returns 0 whether or not the entry was taken, so its
  return value must not be read as "buffer full". An open request that stays pending longer than the
  volume's timeout unmounts the whole volume. Dokan does not enforce per-file permissions
  (see [Backends](#backends-winfsp-dokan-or-projfs)).
- The volume's security descriptor allows only the current user and SYSTEM, so
  other users cannot trigger reads through the mount tool.

## Not done

Defender/indexer behaviour; an installer (the programs are not packaged as an installer or signed, and neither driver is chained into one); a run of `tests\install-chain.ps1` on a clean VM; testing with a second interactive user; and a share of the volume across users beyond what is described above.

## Licensing

This project's own code is MIT licensed (see `LICENSE`). The two file system drivers have their own licences, and
each applies only to the programs built for that driver.

**WinFsp builds** (`PipeFileService-winfsp.exe`, `pipe-file-mount-winfsp.exe`) link to the WinFsp DLL under
WinFsp's [FLOSS exception](https://github.com/winfsp/winfsp/blob/master/License.txt): they must stay under an
open-source licence and must not be linked or distributed together with proprietary software. Distribute
WinFsp itself only as the unmodified installer released by the WinFsp project.

> WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
> https://github.com/winfsp/winfsp

**ProjFS build** (`PipeFileService-projfs.exe`) uses only the Windows Projected File System API, a component of
Windows (the feature `Client-ProjFS`); no third-party driver or library is involved.

**Dokan builds** (`PipeFileService-dokan.exe`, `pipe-file-mount-dokan.exe`) do not use WinFsp. They use
[Dokan](https://github.com/dokan-dev/dokany) (Dokany) 2.x. Its user-mode library (`dokan2.dll`) and driver
are LGPL licensed; these programs load the library dynamically and do not contain it. Distribute Dokan itself
only as the unmodified installer released by the Dokan project.
