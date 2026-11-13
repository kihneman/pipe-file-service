# A fake data server for lazy-fetch.ps1. It records every connection it gets, so the
# test can count how often the bridge reaches the server.
#   -Mode direct   serves the content on every connection to -Pipe
#   -Mode allow    serves the reader-info protocol on -Pipe: reads the request, logs the
#                  reader pid, answers ok and then streams the content
#   -Mode deny     the same, but answers "denied" and sends no content
# -DelayMs delays the answer (used to make concurrent first reads overlap).
param(
    [Parameter(Mandatory)][string]$Pipe,
    [Parameter(Mandatory)][string]$Log,
    [ValidateSet('direct', 'allow', 'deny')][string]$Mode = 'direct',
    [int]$DelayMs = 0,
    [string]$Content = "KEY=value`n"
)
$bytes = [Text.Encoding]::UTF8.GetBytes($Content)

function Read-Exact($s, [int]$n) {
    $buf = New-Object byte[] $n; $got = 0
    while ($got -lt $n) { $r = $s.Read($buf, $got, $n - $got); if ($r -le 0) { return $null }; $got += $r }
    $buf
}

while ($true) {
    if ($Mode -eq 'direct') {
        $s = New-Object IO.Pipes.NamedPipeServerStream($Pipe, 'Out', 4, 'Byte', 'None')
    } else {
        $s = New-Object IO.Pipes.NamedPipeServerStream($Pipe, 'InOut', 4, 'Byte', 'None')
    }
    try {
        $s.WaitForConnection()
        if ($Mode -eq 'direct') {
            Add-Content $Log 'conn'
            if ($DelayMs) { Start-Sleep -Milliseconds $DelayMs }
            $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain()
        } else {
            $hdr = Read-Exact $s 5
            if ($hdr) {
                $len = ($hdr[0] -shl 24) + ($hdr[1] -shl 16) + ($hdr[2] -shl 8) + $hdr[3]
                $req = [Text.Encoding]::UTF8.GetString((Read-Exact $s $len)) | ConvertFrom-Json
                Add-Content $Log "req pid=$($req.reader_pid)"
                if ($DelayMs) { Start-Sleep -Milliseconds $DelayMs }
                $body = if ($Mode -eq 'allow') { '{"ok":true}' } else { '{"denied":true}' }
                $b = [Text.Encoding]::UTF8.GetBytes($body)
                $frame = [byte[]](0, 0, [byte]($b.Length -shr 8), [byte]($b.Length -band 255), 73) + $b
                $s.Write($frame, 0, $frame.Length)
                if ($Mode -eq 'allow') { $s.Write($bytes, 0, $bytes.Length) }
                $s.Flush(); $s.WaitForPipeDrain()
            }
        }
    } catch { } finally { $s.Dispose() }
}
