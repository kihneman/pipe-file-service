# Smoke test for PipeFileService. The service must already be running
# (tests\service-up.ps1, elevated). Run this from an ordinary, unelevated Windows
# PowerShell 5.1: that is the realistic caller and the one that exercises the
# owner and impersonation checks. -LeaveLinked leaves one linked registration open
# in a background process so service-down.ps1 can check cleanup on service stop.
param([switch]$LeaveLinked)
$ErrorActionPreference = 'Stop'
$base = "fsp-svc-smoke-$PID"
$work = Join-Path $env:TEMP "fsp-service-smoke-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$content = "# smoke`nKEY=value`n"
$script:failures = 0
$mySid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}

# ---- control pipe client
function New-Client {
    $c = New-Object IO.Pipes.NamedPipeClientStream('.', 'PipeFileService', 'InOut')
    $c.Connect(5000); $c.ReadMode = 'Message'
    $c
}
function Read-Msg($c) {
    $ms = New-Object IO.MemoryStream; $buf = New-Object byte[] 8192
    do { $n = $c.Read($buf, 0, $buf.Length); if ($n -le 0) { return $null }; $ms.Write($buf, 0, $n) } until ($c.IsMessageComplete)
    [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
}
$script:events = @()
function Req($c, $h) {
    $b = [Text.Encoding]::UTF8.GetBytes(($h | ConvertTo-Json -Compress))
    $c.Write($b, 0, $b.Length); $c.Flush()
    while ($true) {
        $m = Read-Msg $c
        if ($null -eq $m) { return $null }
        if ($m.type -eq 'event') { $script:events += $m; continue }
        return $m
    }
}
function Reg($c, $pipe, $file, $extra) {
    $h = @{ type = 'register'; pipe = "\\.\pipe\$pipe"; file_name = $file }
    if ($extra) { foreach ($k in $extra.Keys) { $h[$k] = $extra[$k] } }
    Req $c $h
}

# ---- data servers (jobs). Each connection appends a line to the log.
$directLog = Join-Path $work 'direct.log'
$riLog = Join-Path $work 'ri.log'
$direct = Start-Job -ArgumentList "$base-d", $content, $directLog -ScriptBlock {
    param($name, $text, $log)
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    while ($true) {
        $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
        try { $s.WaitForConnection(); Add-Content $log 'conn'; $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() }
        catch { } finally { $s.Dispose() }
    }
}
# Reader-info server on <pipe>.ri: logs the request body, answers ok, or denied for the second connection onward.
$ri = Start-Job -ArgumentList "$base-r.ri", $content, $riLog -ScriptBlock {
    param($name, $text, $log)
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $n = 0
    while ($true) {
        $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'InOut', 4, 'Byte', 'None')
        try {
            $s.WaitForConnection()
            $hdr = New-Object byte[] 5; $got = 0
            while ($got -lt 5) { $r = $s.Read($hdr, $got, 5 - $got); if ($r -le 0) { break }; $got += $r }
            if ($got -eq 5) {
                $len = ($hdr[2] -shl 8) + $hdr[3]; $body = New-Object byte[] $len; $got = 0
                while ($got -lt $len) { $r = $s.Read($body, $got, $len - $got); if ($r -le 0) { break }; $got += $r }
                Add-Content $log ([Text.Encoding]::UTF8.GetString($body))
                $n++
                $status = if ($n -eq 1) { '{"status":"ok"}' } else { '{"status":"denied"}' }
                $sb = [Text.Encoding]::UTF8.GetBytes($status)
                $fr = [byte[]](0, 0, 0, $sb.Length, 73) + $sb
                $s.Write($fr, 0, $fr.Length)
                if ($n -eq 1) { $s.Write($bytes, 0, $bytes.Length) }
                $s.Flush(); $s.WaitForPipeDrain()
            }
        } catch { } finally { $s.Dispose() }
    }
}
Start-Sleep -Seconds 1

try {
    $c = New-Client
    "client: $([Security.Principal.WindowsIdentity]::GetCurrent().Name), connected"

    # ---- direct mode: register, read, attribute-only opens, unregister
    $r = Reg $c "$base-d" '.env'
    Check 'register (direct)' ($r.status -eq 'ok' -and $r.path) $r.message
    $file = $r.path
    Check 'file name is pipe-name + "-" + requested name' ($file -like "*\$base-d-.env")
    Start-Sleep -Seconds 3   # registering made the service probe the pipe; the server logs that connection a moment late
    $before = if (Test-Path $directLog) { (Get-Content $directLog).Count } else { 0 }
    $null = Test-Path $file; $null = (Get-Item $file).Length; $null = Get-ChildItem (Split-Path $file)
    $after = if (Test-Path $directLog) { (Get-Content $directLog).Count } else { 0 }
    Check 'attribute-only opens never fetch' ($before -eq $after) "connections $before -> $after"
    # the fetch is made by the first Read, so opening for reading and closing reaches neither the server nor the event stream
    $h = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite'); $h.Close()
    $after = if (Test-Path $directLog) { (Get-Content $directLog).Count } else { 0 }
    Check 'a read-open that closes without reading never fetches' ($before -eq $after) "connections $before -> $after"
    $null = Req $c @{ type = 'ping' }
    Check 'no "opened" event before a real read' (@($script:events | Where-Object { $_.event -eq 'opened' }).Count -eq 0)
    Check 'read returns the pipe content' (([IO.File]::ReadAllText($file)) -eq $content)
    $after = if (Test-Path $directLog) { (Get-Content $directLog).Count } else { 0 }
    Check 'one read handle causes exactly one connection' ($after -eq $before + 1) "connections $before -> $after"
    $null = Req $c @{ type = 'ping' }   # events are queued ahead of the next reply
    Check 'an "opened" event was pushed' (@($script:events | Where-Object { $_.event -eq 'opened' }).Count -ge 1)
    $acl = Get-Acl $file
    $users = @($acl.Access | ForEach-Object { $_.IdentityReference.Value })
    Check 'file ACL: only the registering user and SYSTEM' ($users.Count -eq 2 -and ($users -contains 'NT AUTHORITY\SYSTEM') -and ($users -contains [Security.Principal.WindowsIdentity]::GetCurrent().Name)) ($users -join ',')
    $wrote = $true; try { Set-Content $file 'x' -ErrorAction Stop } catch { $wrote = $false }
    Check 'writes are refused' (-not $wrote)

    # ---- a caller that is not the registering user is refused. There is only one account to test with, so the
    # caller is this user with its own SID switched to deny-only (a restricted token): the owner ACE no longer
    # applies to it, which is what a different user looks like to the file's ACL.
    Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class DenyOnly {
    [StructLayout(LayoutKind.Sequential)] struct SidAndAttr { public IntPtr Sid; public uint Attr; }
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint access, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr tok, int cls, IntPtr buf, int len, out int ret);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool CreateRestrictedToken(IntPtr tok, uint flags, int nDisable, SidAndAttr[] disable, int nPriv, IntPtr priv, int nRestrict, IntPtr restrict, out IntPtr newTok);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool DuplicateToken(IntPtr tok, int level, out IntPtr dup);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SetThreadToken(IntPtr thread, IntPtr tok);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    // Makes the calling thread run as this user with the user's own SID denied-only. Returns false on failure.
    public static bool Begin() {
        IntPtr tok, restricted, imp;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0002 | 0x0008, out tok)) return false;   // DUPLICATE | QUERY
        int len; GetTokenInformation(tok, 1, IntPtr.Zero, 0, out len);                          // TokenUser
        IntPtr buf = Marshal.AllocHGlobal(len);
        if (!GetTokenInformation(tok, 1, buf, len, out len)) return false;
        SidAndAttr[] d = new SidAndAttr[] { new SidAndAttr { Sid = Marshal.ReadIntPtr(buf), Attr = 0 } };
        if (!CreateRestrictedToken(tok, 0, 1, d, 0, IntPtr.Zero, 0, IntPtr.Zero, out restricted)) return false;
        if (!DuplicateToken(restricted, 2, out imp)) return false;                               // SecurityImpersonation
        return SetThreadToken(IntPtr.Zero, imp);
    }
    public static void End() { SetThreadToken(IntPtr.Zero, IntPtr.Zero); }
}
'@
    $denied = $null; $began = [DenyOnly]::Begin()
    try { try { $null = [IO.File]::ReadAllText($file); $denied = $false } catch [UnauthorizedAccessException] { $denied = $true } catch { $denied = "other: $($_.Exception.Message)" } }
    finally { [DenyOnly]::End() }
    Check 'a caller that is not the registering user cannot read the file' ($began -and $denied -eq $true) "began=$began denied=$denied"
    Check 'the registering user still can' (([IO.File]::ReadAllText($file)) -eq $content)

    # ---- registration rules
    $r = Reg $c "$base-d" 'other.env'
    Check 'same pipe twice is refused' ($r.status -eq 'error' -and $r.code -eq 'name_in_use') ($r | ConvertTo-Json -Compress)
    $r = Reg $c "$base-x" '.env'
    Check 'unreachable (missing) pipe is refused' ($r.status -eq 'error' -and $r.code -eq 'pipe_unreachable') ($r | ConvertTo-Json -Compress)
    foreach ($bad in 'a\b.env', 'a/b.env', '..', 'NUL', 'nul.txt', 'x.', 'x ', 'a:b', 'a*b', '', ('x' * 300)) {
        $r = Reg $c "$base-d" $bad
        Check "bad file name '$($bad.Substring(0, [Math]::Min(12, $bad.Length)))' is refused" ($r.status -eq 'error' -and $r.code -eq 'bad_name') ($r | ConvertTo-Json -Compress)
    }
    $r = Req $c @{ type = 'register'; pipe = 'not-a-pipe-path'; file_name = 'x' }
    Check 'pipe outside \\.\pipe\ is refused' ($r.status -eq 'error' -and $r.code -eq 'bad_name')
    $r = Req $c @{ type = 'register'; pipe = "\\.\pipe\$base-d\sub"; file_name = 'x' }
    Check 'pipe name with a sub-path is refused' ($r.status -eq 'error' -and $r.code -eq 'bad_name')
    $r = Req $c @{ type = 'nonsense' }
    Check 'unknown message type is refused' ($r.status -eq 'error' -and $r.code -eq 'bad_request')

    # Combined-name collision: pipe "A-B" + file "C"  and pipe "A" + file "B-C" give the same name.
    $collide = Start-Job -ArgumentList "$base-q", "$base-q-w" -ScriptBlock { param($a, $b)
        $ss = @(); foreach ($n in $a, $b) { $ss += New-Object IO.Pipes.NamedPipeServerStream($n, 'Out', 4, 'Byte', 'None') }
        Start-Sleep 120 }
    Start-Sleep -Seconds 1
    $r1 = Reg $c "$base-q" 'w-f.txt'
    $r2 = Reg $c "$base-q-w" 'f.txt'
    Check 'first of a colliding pair registers' ($r1.status -eq 'ok') ($r1 | ConvertTo-Json -Compress)
    Check 'second pair with the same combined name is refused' ($r2.status -eq 'error' -and $r2.code -eq 'name_in_use') ($r2 | ConvertTo-Json -Compress)
    Stop-Job $collide; Remove-Job $collide -Force

    # ---- reader-info mode
    $r = Reg $c "$base-r" 'ri.env' @{ reader_info_suffix = '.ri' }
    Check 'register (reader-info suffix)' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
    $riFile = $r.path
    $listed = @(Get-ChildItem (Split-Path $riFile) | ForEach-Object Name)
    Check 'the directory lists every registered file' (($listed -contains (Split-Path $file -Leaf)) -and ($listed -contains (Split-Path $riFile -Leaf))) ($listed -join ',')
    $text = [IO.File]::ReadAllText($riFile)
    Check 'first read is served by the reader-info server' ($text -eq $content)
    $req = (Get-Content $riLog | Select-Object -First 1) | ConvertFrom-Json
    Check 'reader-info request names this process as the reader' ($req.reader_pid -eq $PID -and $req.protocol -eq 1) ((Get-Content $riLog) -join ' ')
    $denied = $false; try { [IO.File]::ReadAllText($riFile) | Out-Null } catch { $denied = $true }
    Check 'a denied read fails the open' $denied
    $null = Req $c @{ type = 'ping' }
    Check 'a "denied" event was pushed' (@($script:events | Where-Object { $_.event -eq 'denied' }).Count -ge 1)

    # ---- unregister
    $r = Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-d" }
    Check 'unregister' ($r.status -eq 'ok')
    Check 'file is gone after unregister' (-not (Test-Path $file))
    $r = Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-d" }
    Check 'unregister again says not_found' ($r.status -eq 'error' -and $r.code -eq 'not_found')

    # ---- placeholder-file links
    function New-Placeholder($n) { $p = Join-Path $work $n; New-Item $p -ItemType File | Out-Null; $p }
    $dd = Start-Job -ArgumentList "$base-l", $content -ScriptBlock { param($name, $text)
        $bytes = [Text.Encoding]::UTF8.GetBytes($text)
        while ($true) { $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
            try { $s.WaitForConnection(); $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() } catch { } finally { $s.Dispose() } } }
    Start-Sleep -Seconds 1
    function Try-Link($placeholder, $name) { Reg $c "$base-l" $name @{ link_path = $placeholder } }

    $p = New-Placeholder 'good.env'
    $r = Try-Link $p 'good.env'
    Check 'link: empty own file becomes a link' ($r.status -eq 'ok' -and $r.linked -eq $true) ($r | ConvertTo-Json -Compress)
    Check 'link: reads through to the pipe content' (([IO.File]::ReadAllText($p)) -eq $content)
    Check 'link: placeholder is a reparse point' ((Get-Item $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)
    $r = Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-l" }
    $it = Get-Item $p -Force
    Check 'unregister removes the link, leaves the empty file' ($r.status -eq 'ok' -and -not ($it.Attributes -band [IO.FileAttributes]::ReparsePoint) -and $it.Length -eq 0) "attrs=$($it.Attributes) len=$($it.Length)"

    $p = Join-Path $work 'nonempty'; Set-Content $p 'data' -NoNewline
    $r = Try-Link $p 'a'; Check 'link refused: non-empty' ($r.code -eq 'link_refused' -and $r.message -like '*not empty*') ($r | ConvertTo-Json -Compress)
    $p = Join-Path $work 'adir'; New-Item $p -ItemType Directory | Out-Null
    $r = Try-Link $p 'a'; Check 'link refused: directory' ($r.code -eq 'link_refused' -and $r.message -like '*directory*') ($r | ConvertTo-Json -Compress)
    $p = New-Placeholder 'h1'; cmd /c mklink /H (Join-Path $work 'h2') $p | Out-Null
    $r = Try-Link $p 'a'; Check 'link refused: hard link count 2' ($r.code -eq 'link_refused' -and $r.message -like '*hard link*') ($r | ConvertTo-Json -Compress)
    $r = Try-Link "$env:windir\win.ini" 'a'; Check 'link refused: file the caller cannot write' ($r.code -eq 'link_refused' -and $r.message -like '*cannot open*') ($r | ConvertTo-Json -Compress)
    # Empty, owned by the caller, one link, but the caller has no write permission: only the open
    # under the caller's identity can refuse this one (the service itself could write it).
    # The user may only read it; Administrators may do anything. An unelevated token has Administrators
    # as deny-only, so only a service that skips the impersonation (running elevated) could write it.
    $p = New-Placeholder 'nowrite'
    icacls $p /inheritance:r /grant "*$mySid`:(R)" /grant '*S-1-5-32-544:(F)' /grant '*S-1-5-18:(F)' | Out-Null
    $r = Try-Link $p 'a'; Check 'link refused: empty own file without write permission' ($r.code -eq 'link_refused' -and $r.message -like '*cannot open*') ($r | ConvertTo-Json -Compress)
    icacls $p /grant "*$mySid`:(F)" | Out-Null
    $adminOwned = Join-Path $env:TEMP 'fsp-service-smoke-adminowned'   # made by service-up.ps1, elevated
    if (Test-Path $adminOwned) {
        $r = Try-Link $adminOwned 'a'
        Check 'link refused: owned by Administrators, caller is not elevated' ($r.status -eq 'error' -and $r.message -like '*not owned*') ($r | ConvertTo-Json -Compress)
    } else { 'SKIP  owner check (no admin-owned file; run service-up.ps1)' }
    $r = Try-Link (Join-Path $work 'missing') 'a'; Check 'link refused: missing file' ($r.status -eq 'error' -and $r.code -eq 'link_refused') ($r | ConvertTo-Json -Compress)
    $r = Try-Link 'relative\path' 'a'; Check 'link refused: relative path' ($r.status -eq 'error' -and $r.code -eq 'link_refused') ($r | ConvertTo-Json -Compress)
    $p = New-Placeholder 'twice'
    $r = Try-Link $p 'twice.env'
    $r2 = Reg $c "$base-d" 'again.env' @{ link_path = $p }   # second registration, same placeholder
    Check 'link refused: already a link' ($r.status -eq 'ok' -and $r2.status -eq 'error') ($r2 | ConvertTo-Json -Compress)
    Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-l" } | Out-Null
    # A refused link leaves nothing registered.
    $r = Reg $c "$base-l" 'plain.env'
    Check 'a refused link did not leave the pipe registered' ($r.status -eq 'ok')
    Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-l" } | Out-Null

    # ---- closing the connection removes everything, including the link
    $c2 = New-Client
    $p = New-Placeholder 'onclose.env'
    $r = Reg $c2 "$base-l" 'onclose.env' @{ link_path = $p }
    $isLink = (Get-Item $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint
    $c2.Dispose(); Start-Sleep -Seconds 1
    $it = Get-Item $p -Force
    Check 'disconnect removes the link' ($r.status -eq 'ok' -and $isLink -and -not ($it.Attributes -band [IO.FileAttributes]::ReparsePoint) -and $it.Length -eq 0) "attrs=$($it.Attributes)"
    $c3 = New-Client
    $r = Reg $c3 "$base-l" 'again.env'
    Check 'disconnect removes the registration' ($r.status -eq 'ok')
    $c3.Dispose()

    if ($LeaveLinked) {
        $leftDir = Join-Path $env:TEMP 'fsp-service-smoke-left'
        New-Item -ItemType Directory -Force $leftDir | Out-Null
        $lp = Join-Path $leftDir 'left.env'; New-Item $lp -ItemType File -Force | Out-Null
        $script = Join-Path $leftDir 'hold.ps1'
        @"
`$c = New-Object IO.Pipes.NamedPipeClientStream('.', 'PipeFileService', 'InOut'); `$c.Connect(5000); `$c.ReadMode = 'Message'
`$b = [Text.Encoding]::UTF8.GetBytes((@{ type = 'register'; pipe = '\\.\pipe\$base-l'; file_name = 'left.env'; link_path = '$lp' } | ConvertTo-Json -Compress))
`$c.Write(`$b, 0, `$b.Length); `$buf = New-Object byte[] 4096; `$null = `$c.Read(`$buf, 0, 4096)
Start-Sleep -Seconds 600
"@ | Set-Content $script
        Start-Process powershell -ArgumentList '-NoProfile', '-File', $script -WindowStyle Hidden | Out-Null
        Start-Sleep -Seconds 3
        Set-Content (Join-Path $leftDir 'info.txt') $lp
        "left a linked registration open: $lp ($((Get-Item $lp -Force).Attributes))"
    }
}
finally {
    # Stop-Job can hang on a job blocked inside a pipe call: end the job processes directly.
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID AND Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -notmatch 'hold\.ps1' } |   # keep the -LeaveLinked process alive
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
    if ($c) { $c.Dispose() }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
