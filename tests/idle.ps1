# Checks that the mount appears when it is needed and goes away when it is not, with the control pipe up the whole time.
# The service must be running with a short grace period: tests\service-up.ps1 -IdleSeconds 3 (elevated), no registrations
# open. Run from an ordinary, unelevated Windows PowerShell 5.1. Backend-neutral. Pass the same value as -IdleSeconds.
param([int]$IdleSeconds = 3, [string]$Mount = 'C:\ProgramData\PipeFileService\mount')
$ErrorActionPreference = 'Stop'
$base = "pfs-idle-$PID"
$content = "KEY=value`n"; $len = [Text.Encoding]::UTF8.GetByteCount($content)
$script:failures = 0
    else { Check $name (Wait-Mount $false $seconds) "mount still at $Mount" }
}
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
function Reg($c, $name, $extra) {
    $h = @{ type = 'register'; pipe = "\\.\pipe\$base-$name"; file_name = "$name.env" }
    if ($extra) { foreach ($k in $extra.Keys) { $h[$k] = $extra[$k] } }
    $r = Req $c $h
    if ($r.code -eq 'size_required') { $h.size = $len; $r = Req $c $h }   # the ProjFS build needs a size
    $r
}
function Unreg($c, $name) { Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-$name" } }
function Wait-Mount($want, $seconds) {
    for ($i = 0; $i -lt $seconds * 4; $i++) { if ((Test-Path $Mount) -eq $want) { return $true }; Start-Sleep -Milliseconds 250 }
    (Test-Path $Mount) -eq $want
}

# One data server per pipe the test registers.
$bytes = [Text.Encoding]::UTF8.GetBytes($content)
foreach ($n in 'a', 'b', 'c', 'd') {
    Start-Job -ArgumentList "$base-$n", $bytes -ScriptBlock {
        param($name, $bytes)
        while ($true) {
            $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
            try { $s.WaitForConnection(); $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() } catch { } finally { $s.Dispose() }
        }
    } | Out-Null
}
Start-Sleep -Seconds 2

try {
    Check 'the control pipe is up' (Test-Path '\\.\pipe\PipeFileService')
    Check 'nothing is mounted while nothing is registered' (Wait-Mount $false 1) "mount present at $Mount"
    $c = New-Client

    $r = Reg $c 'a' @{ defer = 'maybe' }
    Check 'defer must be true or false' ($r.status -eq 'error' -and $r.code -eq 'bad_request') ($r | ConvertTo-Json -Compress)
    Check 'a refused registration mounts nothing' (-not (Test-Path $Mount))

    # ---- the first registration mounts
    $r = Reg $c 'a'
    Check 'register' ($r.status -eq 'ok' -and $r.path) ($r | ConvertTo-Json -Compress)
    Check 'the mount exists after the first registration' (Test-Path $Mount)
    $file = $r.path
    Check 'the file reads' (([IO.File]::ReadAllText($file)) -eq $content)

    # ---- an open handle keeps the mount after the last registration is gone
    $h = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite,Delete')
    $null = Unreg $c 'a'
    Start-Sleep -Seconds ($IdleSeconds + 3)
    Check 'an open handle keeps the mount after the last unregister' (Test-Path $Mount)
    $h.Dispose()
    Check 'the mount is removed after the handle closes and the grace period passes' (Wait-Mount $false ($IdleSeconds + 8)) "mount still at $Mount"
    Check 'the control pipe is still up' (Test-Path '\\.\pipe\PipeFileService')

    # ---- it mounts again, and a deferred registration with the driver present reports mounted
    $r = Reg $c 'b' @{ defer = $true }
    Check 'register with defer (driver present)' ($r.status -eq 'ok' -and $r.mounted -eq $true) ($r | ConvertTo-Json -Compress)
    Check 'the mount is back' (Test-Path $Mount)
    Check 'and the file reads' (([IO.File]::ReadAllText($r.path)) -eq $content)
    $null = Req $c @{ type = 'ping' }
    Check 'no "mounted" event is sent when the reply already said mounted' (@($script:events | Where-Object { $_.event -eq 'mounted' }).Count -eq 0)

    # ---- retry_mount
    $r = Req $c @{ type = 'retry_mount' }
    Check 'retry_mount when mounted answers mounted' ($r.status -eq 'ok' -and $r.mounted -eq $true) ($r | ConvertTo-Json -Compress)

    # ---- closing the connection drops its registrations, and the mount follows
    $c.Dispose()
    Check 'the mount is removed after the connection closes' (Wait-Mount $false ($IdleSeconds + 8)) "mount still at $Mount"

    # ---- registrations that arrive during the grace period keep the mount
    $c = New-Client
    $r = Reg $c 'c'
    $null = Unreg $c 'c'
    Start-Sleep -Seconds 1
    $r = Reg $c 'd'
    Start-Sleep -Seconds ($IdleSeconds + 3)
    Check 'a registration made inside the grace period keeps the mount' ((Test-Path $Mount) -and ([IO.File]::ReadAllText($r.path)) -eq $content)
    $c.Dispose()
}
finally {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID AND Name='powershell.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
    if ($c) { $c.Dispose() }
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0
