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
winfsp-pipe-bridge.exe \\.\pipe\<name> K:
```

Mounts drive `K:` (or `*` to pick a free letter) containing one file,
`property-file.env`. Opening it for reading connects to the pipe, reads until
EOF and serves those bytes. If the pipe's server answers with its standard
"Access denied" text the open fails with `STATUS_ACCESS_DENIED`. For every open
the bridge logs the caller's PID and executable path, which WinFsp reports at
open time.

## Build

Requires Visual Studio C++ build tools and WinFsp installed **with the
Developer feature** (SDK headers and import library). From a Developer
PowerShell:

```powershell
.\build.ps1
```

Run with `winfsp-x64.dll` on `PATH` (`C:\Program Files (x86)\WinFsp\bin`).

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
