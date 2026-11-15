# Checks specific to the ProjFS build (PipeFileService-projfs.exe). The service must already be running
# (tests\service-up.ps1 -Backend projfs, elevated). Run from an ordinary, unelevated Windows PowerShell 5.1.
# The suites that are the same for every build (service-smoke, lazy-fetch, limits, slow-read, declared-size) are
# run separately; this one covers what ProjFS does differently.
$ErrorActionPreference = 'Stop'
$base = "pfs-projfs-$PID"
$work = Join-Path $env:TEMP "pfs-projfs-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$content = "KEY=value`nOTHER=thing`n"
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

$log = Join-Path $work 'srv.log'
$srv = Start-Job -ArgumentList "$base-s", $content, $log -ScriptBlock {
    param($name, $text, $log)
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    while ($true) {
        $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
        try { $s.WaitForConnection(); Add-Content $log 'conn'; $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() }
        catch { } finally { $s.Dispose() }
    }
}
Start-Sleep -Seconds 1

try {
    $c = New-Client

    # ---- the size is mandatory in this build
    $r = Reg $c "$base-s" 's.env'
    Check 'register without a size is refused (size_required)' ($r.status -eq 'error' -and $r.code -eq 'size_required') ($r | ConvertTo-Json -Compress)

    $r = Reg $c "$base-s" 's.env' @{ size = $len }
    Check 'register with a size' ($r.status -eq 'ok' -and $r.path) $r.message
    $file = $r.path
    $mount = Split-Path $file
    Start-Sleep -Seconds 3   # the registration probe reaches the server a moment late

    # ---- stat and listing never fetch; the size is the declared one
    $before = Conns $log
    Check 'size is the declared size' ((Get-Item $file).Length -eq $len)
    Check 'the listing shows the file with its size' (@(Get-ChildItem $mount | Where-Object { $_.Name -like '*s.env' -and $_.Length -eq $len }).Count -eq 1)
    Check 'stat and listing did not fetch' ((Conns $log) -eq $before) "connections $before -> $(Conns $log)"

    # ---- reads: content, one fetch per read (dehydrate on close), reader identity in the event
    Check 'read returns the content' (([IO.File]::ReadAllText($file)) -eq $content)
    Start-Sleep -Milliseconds 500
    $after1 = Conns $log
    Check 'the first read made one fetch' ($after1 -eq $before + 1) "connections $before -> $after1"
    foreach ($i in 1..3) { $null = [IO.File]::ReadAllText($file); Start-Sleep -Milliseconds 300 }
    Check 'three more sequential reads made three more fetches (the file is removed after each close)' ((Conns $log) -eq $after1 + 3) "connections $after1 -> $(Conns $log)"

    $script:events = @()
    $reader = Start-Process powershell -ArgumentList '-NoProfile', '-Command', "[IO.File]::ReadAllText('$file') | Out-Null" -PassThru -WindowStyle Hidden
    $reader.WaitForExit()
    $null = Req $c @{ type = 'ping' }
    $opened = @($script:events | Where-Object { $_.event -eq 'opened' })
    Check 'the "opened" event names the reading process' ($opened.Count -ge 1 -and $opened[-1].reader_pid -eq $reader.Id) "pid $($reader.Id), events: $($script:events | ConvertTo-Json -Compress)"

    # ---- two concurrent readers: every read succeeds
    $job = { param($f, $text) $ok = 0; $bad = 0; 1..50 | ForEach-Object { try { if ([IO.File]::ReadAllText($f) -eq $text) { $ok++ } else { $bad++ } } catch { $bad++ } }; "ok=$ok bad=$bad" }
    $j1 = Start-Job $job -ArgumentList $file, $content; $j2 = Start-Job $job -ArgumentList $file, $content
    $res = @($j1, $j2 | Wait-Job | Receive-Job)
    Check 'two concurrent readers, 50 reads each: all succeed' (($res -join ' ') -eq 'ok=50 bad=0 ok=50 bad=0') ($res -join ' ')

    # ---- read-only: the folder refuses writes, deletes, renames and new files
    $refused = { param($sb) try { & $sb; $false } catch { $true } }
    Check 'writing the file is refused' (& $refused { Add-Content $file 'x' -ErrorAction Stop })
    Check 'deleting the file is refused' (& $refused { Remove-Item $file -Force -ErrorAction Stop })
    Check 'renaming the file is refused' (& $refused { Rename-Item $file 'moved.env' -ErrorAction Stop })
    Check 'creating a new file in the folder is refused' (& $refused { Set-Content (Join-Path $mount 'new.txt') 'x' -ErrorAction Stop })
    Check 'the file still reads after those attempts' (([IO.File]::ReadAllText($file)) -eq $content)

    # ---- nothing is left hydrated once the readers are done
    Start-Sleep -Seconds 1
    $attrs = (Get-Item $file -Force).Attributes
    Check 'after the last close the file is a placeholder again (not stored on disk)' (($attrs -band 0x400000) -ne 0) "attributes: $attrs (0x400000 = recall on data access)"

    # ---- unregister removes the file
    $r = Req $c @{ type = 'unregister'; pipe = "\\.\pipe\$base-s" }
    Check 'unregister' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
    Start-Sleep -Milliseconds 500
    Check 'the file is gone from the folder' (-not (Test-Path $file))
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
