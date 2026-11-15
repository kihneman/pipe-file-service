# Test for the optional `size` of a register request. The service must already be running
# (tests\service-up.ps1, elevated). Run from an ordinary, unelevated Windows PowerShell 5.1.
# Backend-neutral: the same checks hold for every build.
$ErrorActionPreference = 'Stop'
$base = "pfs-size-$PID"
$work = Join-Path $env:TEMP "pfs-declared-size-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$content = "KEY=value`n"                                   # 10 bytes
$len = [Text.Encoding]::UTF8.GetByteCount($content)
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}

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
function Conns($log) { if (Test-Path $log) { @(Get-Content $log).Count } else { 0 } }

# One data server per case, each logging a line per connection.
function Start-Server($name, $text, $log) {
    Start-Job -ArgumentList $name, $text, $log -ScriptBlock {
        param($name, $text, $log)
        $bytes = [Text.Encoding]::UTF8.GetBytes($text)
        while ($true) {
            $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
            try { $s.WaitForConnection(); Add-Content $log 'conn'; $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() }
            catch { } finally { $s.Dispose() }
        }
    } | Out-Null
}
$logs = @{}
foreach ($k in 'ok', 'big', 'small', 'none', 'bad') { $logs[$k] = Join-Path $work "$k.log"; Start-Server "$base-$k" $content $logs[$k] }
Start-Sleep -Seconds 1

try {
    $c = New-Client

    # ---- the right size: known before any read, stat never fetches, content is exact
    $r = Reg $c "$base-ok" 'a.env' @{ size = $len }
    Check 'register with a size' ($r.status -eq 'ok' -and $r.path) $r.message
    $file = $r.path
    Start-Sleep -Seconds 3   # the registration probe reaches the server a moment late
    $before = Conns $logs.ok
    $size = (Get-Item $file).Length
    Check 'size is the declared size before any read' ($size -eq $len) "size=$size"
    $null = Get-ChildItem (Split-Path $file)
    $h = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite'); $h.Close()
    Check 'stat, listing and a read-open without reading never fetch' ((Conns $logs.ok) -eq $before) "connections $before -> $(Conns $logs.ok)"
    Check 'read returns the content' (([IO.File]::ReadAllText($file)) -eq $content)
    Check 'size is unchanged after the read' ((Get-Item $file).Length -eq $len)

    # ---- a size that does not match what the server sends: the read fails, nothing partial is returned
    foreach ($case in @(,@("big", ($len + 5)) + ,@("small", ($len - 3)))) {
        $k = $case[0]; $declared = $case[1]
        $null = Req $c @{ type = 'ping' }   # events are queued ahead of the next reply: drain the earlier ones first
        $script:events = @()
        $r = Reg $c "$base-$k" 'm.env' @{ size = $declared }
        Check "register ($k)" ($r.status -eq 'ok') $r.message
        $f = $r.path
        Check "($k) size is the declared $declared" ((Get-Item $f).Length -eq $declared)
        $got = $null; $failed = $false
        try { $got = [IO.File]::ReadAllText($f) } catch { $failed = $true }
        Check "($k) a size mismatch fails the read" ($failed -and -not $got) "got='$got'"
        $null = Req $c @{ type = 'ping' }
        $ev = @($script:events | Where-Object { $_.event -eq 'error' })
        Check "($k) an error event with size_mismatch was pushed" ($ev.Count -ge 1 -and $ev[0].message -eq 'size_mismatch') (($script:events | ConvertTo-Json -Compress))
        Check "($k) no 'opened' event" (@($script:events | Where-Object { $_.event -eq 'opened' }).Count -eq 0)
    }

    # ---- without a size nothing changes: size 0 until a handle has read
    $r = Reg $c "$base-none" 'n.env'
    if ($r.code -eq 'size_required') {
        Check 'no size: this build refuses a registration without one (size_required)' ($r.status -eq 'error') ($r | ConvertTo-Json -Compress)
    } else {
        $f = $r.path
        Check 'no size: reported size is 0 before a read' ((Get-Item $f).Length -eq 0)
        Check 'no size: read returns the content' (([IO.File]::ReadAllText($f)) -eq $content)
    }

    # ---- bad values are refused with bad_request
    foreach ($bad in @('abc', '-1', '1.5', '', 1073741825, '99999999999999999999')) {
        $r = Reg $c "$base-bad" 'b.env' @{ size = $bad }
        Check "size '$bad' is refused" ($r.status -eq 'error' -and $r.code -eq 'bad_request') ($r | ConvertTo-Json -Compress)
    }
    $r = Reg $c "$base-bad" 'b.env' @{ size = 0 }
    Check 'size 0 is accepted (a declared empty file)' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
    $r = Reg $c "$base-bad2" 'b.env' @{ size = 1073741824 }
    Check 'the largest size (1 GiB) is accepted by the check (pipe may be missing)' ($r.code -ne 'bad_request') ($r | ConvertTo-Json -Compress)
}
finally {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID AND Name='powershell.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
    if ($c) { $c.Dispose() }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0
