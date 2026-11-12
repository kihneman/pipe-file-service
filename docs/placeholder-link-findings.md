# Placeholder-file links: findings

The service turns an empty file the client created into a symlink to a registered file, so it can
run as a privileged account without ever acting on a path it was handed. This records what an
experiment (an elevated server, an unelevated client talking over a named pipe, Windows 11) showed.

## What works

1. Impersonate the control-pipe client (`ImpersonateNamedPipeClient`) and open the placeholder
   without following links, with **attribute and read-control access only** and share mode
   read+write (no delete), then `RevertToSelf`. A file the caller cannot write fails here.
2. On that handle check: not a directory, not a reparse point, size 0, link count 1, and the owner
   is the caller's user SID (or Administrators when the caller's token is elevated).
3. Reopen the same file **by file ID as the service** (`OpenFileById`, using the first handle as
   the volume hint), confirm the volume serial and file ID match, and set the symlink reparse data
   (`FSCTL_SET_REPARSE_POINT`) on that second handle.

The link then reads through to the target, and the placeholder shows `ReparsePoint`.

## What does not work (the design had to change)

Setting the reparse point on the handle that was opened under impersonation fails with
"A required privilege is not held by the client", even though the service holds
`SeCreateSymbolicLinkPrivilege`. The privilege is checked against the identity that opened the
handle, not the thread that issues the control code. Hence step 3 above.

## Refusals observed

| Case | Result |
| --- | --- |
| Non-empty file | refused (size) |
| Directory | refused |
| Hard link (link count 2) | refused |
| File owned by Administrators, caller is a standard user | refused (owner) |
| File in a folder the caller cannot write (`C:\Windows\win.ini`) | refused, access denied at the impersonated open |
| Missing file | refused, not found |
| Already a symlink | refused |

## Findings from running the real service

- A restricted service SID (`SERVICE_SID_TYPE_RESTRICTED`) makes the service token write-restricted.
  The service then could not create its `C:\ProgramData` folder, so the mount point failed and the
  service exited at startup; with an unrestricted service SID it ran. (A write-restricted token would
  also be expected to fail on a user's placeholder, whose ACL does not name the service SID; that was
  not tested separately.) The service is installed with an unrestricted service SID and a short
  required-privileges list instead.
- `ImpersonateNamedPipeClient` fails until the server has read a message from the client, so the
  client's identity is taken after the first request.
- Unlinking works: `FSCTL_DELETE_REPARSE_POINT` on the kept handle leaves the empty file, on
  unregister and on disconnect.
- Windows itself refuses to set a reparse point on a file with data (error 4392), so the emptiness
  check is a second line of defence with a clear message.
- Each of the empty, link-count, owner and open-as-the-caller checks was weakened in a test build and
  the smoke test failed every time, so each check is covered.

## Caveats

- Because the first open allows write sharing (the service must reopen it for writing), a client
  that holds the placeholder open for write does not block the link.
