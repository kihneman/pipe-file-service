# Checks the limits on the control pipe: registrations per connection, registrations per user and connections
# per user. (The limit on connections in total needs several accounts and is not covered.)
# The service must already be running (tests\service-up.ps1, or `FspPipeFileService.exe run` in an
# elevated console). Run this from an ordinary, unelevated Windows PowerShell 5.1.
# The parameters must equal the service's limits. The defaults are 64, 256 and 16; a quicker run is
#   FspPipeFileService.exe run --max-reg-per-conn 4 --max-reg-per-user 8 --max-conn-per-user 3
#   .\tests\limits.ps1 -PerConn 4 -PerUser 8 -ConnPerUser 3
# PerUser must be a multiple of PerConn, and ConnPerUser at least PerUser / PerConn + 1.
param([int]$PerConn = 64, [int]$PerUser = 256, [int]$ConnPerUser = 16)
$ErrorActionPreference = 'Stop'
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
function New-Client {
    $c = New-Object IO.Pipes.NamedPipeClientStream('.', 'FspPipeFileService', 'InOut')
    $c.Connect(5000); $c.ReadMode = 'Message'
    $c
}
function Req($c, $h) {
    $b = [Text.Encoding]::UTF8.GetBytes(($h | ConvertTo-Json -Compress))
    try {
        $c.Write($b, 0, $b.Length); $c.Flush()
        $ms = New-Object IO.MemoryStream; $buf = New-Object byte[] 8192
        do { $n = $c.Read($buf, 0, $buf.Length); if ($n -le 0) { return $null }; $ms.Write($buf, 0, $n) } until ($c.IsMessageComplete)
    } catch [IO.IOException] { return $null }
    [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
}
$base = "fsp-limits-$PID"
$script:servers = New-Object Collections.ArrayList
function Get-Pipe($i) {
    while ($script:servers.Count -le $i) {
        # a listening instance is enough: the service only opens the pipe to prove the caller can reach it
        [void]$script:servers.Add((New-Object IO.Pipes.NamedPipeServerStream("$base-$($script:servers.Count)", 'Out', 1, 'Byte', 'Asynchronous')))
    }
    "\\.\pipe\$base-$i"
}
function Reg($c, $i) { Req $c @{ type = 'register'; pipe = (Get-Pipe $i); file_name = "$base-$i.env" } }
$clients = @()
try {
    # ---- per connection
    $a = New-Client; $clients += $a
    $bad = 0
    for ($i = 0; $i -lt $PerConn; $i++) { $r = Reg $a $i; if ($r.status -ne 'ok') { $bad++ } }
    Check "$PerConn registrations on one connection are accepted" ($bad -eq 0) "$bad refused"
    $r = Reg $a $PerConn
    Check 'the next one is refused with too_many_registrations' ($r.status -eq 'error' -and $r.code -eq 'too_many_registrations') ($r | ConvertTo-Json -Compress)
    $r = Req $a @{ type = 'unregister'; pipe = (Get-Pipe 0) }
    Check 'unregister frees a slot' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
    $r = Reg $a $PerConn
    Check 'the slot can be used again' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)

    # ---- per user, across connections (a is full; fill the others up to the user's total)
    $next = $PerConn + 1
    $more = $PerUser / $PerConn - 1
    for ($k = 0; $k -lt $more; $k++) {
        $c = New-Client; $clients += $c; $bad = 0
        for ($i = 0; $i -lt $PerConn; $i++) { $r = Reg $c $next; $next++; if ($r.status -ne 'ok') { $bad++ } }
        if ($bad) { break }
    }
    Check "the user can hold $PerUser registrations over $($more + 1) connections" ($bad -eq 0) "$bad refused"
    $c5 = New-Client; $clients += $c5
    $r = Reg $c5 $next
    Check 'one more for the same user is refused' ($r.status -eq 'error' -and $r.code -eq 'too_many_registrations') ($r | ConvertTo-Json -Compress)

    # ---- connections per user
    # a connection only counts for its user once the service has seen its first message
    while ($clients.Count -lt $ConnPerUser) { $f = New-Client; $clients += $f; [void](Req $f @{ type = 'unregister'; pipe = '\\.\pipe\none' }) }
    $extra = New-Client   # the connect itself succeeds; the service hangs up when it reads the first message
    $r = Req $extra @{ type = 'unregister'; pipe = '\\.\pipe\none' }
    Check 'one connection over the user limit is dropped' ($null -eq $r) ($r | ConvertTo-Json -Compress)
    $extra.Dispose()
    $r = Req $clients[$ConnPerUser - 1] @{ type = 'unregister'; pipe = '\\.\pipe\none' }
    Check 'the connections within the limit still work' ($r.status -eq 'error' -and $r.code -eq 'not_found') ($r | ConvertTo-Json -Compress)

    # ---- closing connections frees everything
    foreach ($c in $clients) { $c.Dispose() }
    $clients = @()
    Start-Sleep -Seconds 2
    $d = New-Client; $clients += $d
    $r = Reg $d 1000
    Check 'after all connections close, registering works again' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
}
finally {
    foreach ($c in $clients) { $c.Dispose() }
    foreach ($s in $script:servers) { $s.Dispose() }
}
if ($script:failures) { "$($script:failures) FAILED"; exit 1 } else { 'ALL PASSED' }
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
