# A read that waits a long time for the data server (for example while the server asks the user to approve
# the reader) must neither fail nor take the volume down. The Dokan driver unmounts a volume when one request
# stays pending longer than its timeout (15 s by default), so this runs a server that answers after -Delay
# seconds. The service must already be running (tests\service-up.ps1, elevated); run this unelevated.
param([int]$Delay = 20)
$ErrorActionPreference = 'Stop'
$base = "fsp-slow-$PID"
$content = "slow`nKEY=value`n"
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
function New-Client {
    $c = New-Object IO.Pipes.NamedPipeClientStream('.', 'PipeFileService', 'InOut')
    $c.Connect(5000); $c.ReadMode = 'Message'
    $c
}
function Req($c, $h) {
    $b = [Text.Encoding]::UTF8.GetBytes(($h | ConvertTo-Json -Compress))
    $c.Write($b, 0, $b.Length); $c.Flush()
    $ms = New-Object IO.MemoryStream; $buf = New-Object byte[] 8192
    do { $n = $c.Read($buf, 0, $buf.Length); if ($n -le 0) { return $null }; $ms.Write($buf, 0, $n) } until ($c.IsMessageComplete)
    [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
}

# Answers every connection after $delay seconds. The first connection is the service probing the pipe at
# registration (it connects and closes at once), which gets no delay.
$server = Start-Job -ArgumentList "$base-d", $content, $Delay -ScriptBlock {
    param($name, $text, $delay)
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $first = $true
    while ($true) {
        $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
        try {
            $s.WaitForConnection()
            if ($first) { $first = $false } else { Start-Sleep -Seconds $delay; $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() }
        } catch { } finally { $s.Dispose() }
    }
}
try {
    Start-Sleep -Milliseconds 500
    $c = New-Client
    $r = Req $c @{ type = 'register'; pipe = "\\.\pipe\$base-d"; file_name = 'slow.env' }
    Check 'register' ($r.status -eq 'ok' -and $r.path) ($r | ConvertTo-Json -Compress)
    $file = $r.path
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $text = $null; $err = $null
    try { $text = [IO.File]::ReadAllText($file) } catch { $err = $_.Exception.Message }
    $sw.Stop()
    Check "the read waits $Delay s and succeeds" ($text -eq $content -and $sw.Elapsed.TotalSeconds -ge $Delay - 1) "text=[$text] error=[$err] after $([int]$sw.Elapsed.TotalSeconds) s"
    Check 'the volume is still mounted afterwards' (Test-Path $file) 'the file is gone'
    Check 'the directory still lists' (@(Get-ChildItem (Split-Path $file)).Count -ge 1) 'listing failed'
    $c.Dispose()
} finally {
    Get-Process -Id $PID | Out-Null
    Remove-Job $server -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$($script:failures) FAILED"; exit 1 } else { 'ALL PASSED'; exit 0 }
