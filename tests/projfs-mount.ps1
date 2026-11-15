# Test of pipe-file-mount-projfs.exe. Needs the Windows feature Client-ProjFS and the built tool; no administrator
# rights and no service are needed. Run from an ordinary Windows PowerShell 5.1.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\pipe-file-mount-projfs.exe')
)
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$work = Join-Path $env:TEMP "projfs-mount-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$content = "KEY=value`n"                      # what count-server.ps1 serves by default
$len = [Text.Encoding]::UTF8.GetByteCount($content)
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
$servers = @(); $mounts = @()

function Start-Server($pipe, $log, $mode) {
    $a = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'count-server.ps1'), '-Pipe', $pipe, '-Log', $log, '-Mode', $mode)
    $script:servers += Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
}
function Start-Mount($pipe, $folder, $errlog, $extra) {
    $a = @("\\.\pipe\$pipe", $folder) + $extra
    $p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardError $errlog
    $script:mounts += $p
    for ($i = 0; $i -lt 40 -and -not (Test-Path $folder); $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Milliseconds 500
    $p
}
function Conns($log) { if (Test-Path $log) { @(Get-Content $log).Count } else { 0 } }
function Run($arguments) {   # runs the tool for its exit code (it should refuse to start)
    $p = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru -WindowStyle Hidden -Wait -RedirectStandardError (Join-Path $work 'run.err')
    $p.ExitCode
}

try {
    # ---- refusals before anything is mounted
    Check 'no --size: refused (exit 2)' ((Run @('\\.\pipe\none', (Join-Path $work 'x1'))) -eq 2)
    Check 'a drive letter is refused' ((Run @('\\.\pipe\none', 'K:', '--size', '1')) -ne 0)
    Check '"*" is refused' ((Run @('\\.\pipe\none', '*', '--size', '1')) -ne 0)
    Check 'bad --size values are refused' (((Run @('\\.\pipe\none', (Join-Path $work 'x2'), '--size', 'abc')) -eq 2) -and ((Run @('\\.\pipe\none', (Join-Path $work 'x2'), '--size', '-1')) -eq 2) -and ((Run @('\\.\pipe\none', (Join-Path $work 'x2'), '--size', '1073741825')) -eq 2))
    $full = Join-Path $work 'notempty'; New-Item -ItemType Directory $full | Out-Null; Set-Content (Join-Path $full 'mine.txt') 'precious'
    Check 'a folder that is not empty is refused' ((Run @('\\.\pipe\none', $full, '--size', '1')) -eq 1)
    Check '... and left untouched' ((Get-Content (Join-Path $full 'mine.txt')) -eq 'precious')

    # ---- direct mode: nested path, size, content, one fetch per read
    $pipe = "pfm-projfs-$PID"; $log = Join-Path $work 'direct.log'; $folder = Join-Path $work 'mnt'
    Start-Server $pipe $log 'direct'
    $m = Start-Mount $pipe $folder (Join-Path $work 'mount.err') @('\secrets\app\prod.env', '--size', $len)
    $file = Join-Path $folder 'secrets\app\prod.env'
    Check 'the mount tool is running' (-not $m.HasExited) (Get-Content (Join-Path $work 'mount.err') -Raw)
    $before = Conns $log
    Check 'listing the root shows the virtual directory' (@(Get-ChildItem $folder | Where-Object { $_.Name -eq 'secrets' -and $_.PSIsContainer }).Count -eq 1)
    Check 'listing the nested directory shows the file with its size' (@(Get-ChildItem (Join-Path $folder 'secrets\app') | Where-Object { $_.Name -eq 'prod.env' -and $_.Length -eq $len }).Count -eq 1)
    Check 'size is the declared size' ((Get-Item $file).Length -eq $len)
    Check 'stat and listing did not fetch' ((Conns $log) -eq $before) "connections $before -> $(Conns $log)"
    Check 'read returns the content' (([IO.File]::ReadAllText($file)) -eq $content)
    Start-Sleep -Milliseconds 500
    Check 'the first read made one fetch' ((Conns $log) -eq $before + 1) "connections $before -> $(Conns $log)"
    $after = Conns $log
    foreach ($i in 1..3) { $null = [IO.File]::ReadAllText($file); Start-Sleep -Milliseconds 300 }
    Check 'three more sequential reads made three more fetches (dehydrate on close)' ((Conns $log) -eq $after + 3) "connections $after -> $(Conns $log)"
    $reader = Start-Process powershell -ArgumentList '-NoProfile', '-Command', "[IO.File]::ReadAllText('$file') | Out-Null" -PassThru -WindowStyle Hidden
    $reader.WaitForExit(); Start-Sleep -Milliseconds 500
    Check 'the log names the reading process' ((Get-Content (Join-Path $work 'mount.err') -Raw) -match "OPEN .* pid=$($reader.Id)\b")
    $acl = Get-Acl $file; $users = @($acl.Access | ForEach-Object { $_.IdentityReference.Value })
    Check 'file ACL: only the user and SYSTEM' ($users.Count -eq 2 -and ($users -contains 'NT AUTHORITY\SYSTEM') -and ($users -contains [Security.Principal.WindowsIdentity]::GetCurrent().Name)) ($users -join ',')

    # ---- read-only
    $refused = { param($sb) try { & $sb; $false } catch { $true } }
    Check 'writing the file is refused' (& $refused { Add-Content $file 'x' -ErrorAction Stop })
    Check 'deleting the file is refused' (& $refused { Remove-Item $file -Force -ErrorAction Stop })
    Check 'renaming the file is refused' (& $refused { Rename-Item $file 'moved.env' -ErrorAction Stop })
    Check 'the file still reads' (([IO.File]::ReadAllText($file)) -eq $content)

    # ---- killed run: the folder is left behind and the next start cleans it
    Stop-Process -Id $m.Id -Force; Start-Sleep -Seconds 1
    $m2 = Start-Mount $pipe $folder (Join-Path $work 'mount2.err') @('\secrets\app\prod.env', '--size', $len)
    Check 'start over the folder a killed run left behind' (-not $m2.HasExited) (Get-Content (Join-Path $work 'mount2.err') -Raw)
    Check 'and it reads again' (([IO.File]::ReadAllText($file)) -eq $content)

    # ---- a different length than declared: the read fails
    $pipe3 = "pfm-projfs3-$PID"; $log3 = Join-Path $work 'wrong.log'; $folder3 = Join-Path $work 'mnt3'
    Start-Server $pipe3 $log3 'direct'
    $m3 = Start-Mount $pipe3 $folder3 (Join-Path $work 'mount3.err') @('--size', ($len + 4))
    $f3 = Join-Path $folder3 'property-file.env'
    Check 'wrong size: the declared size is reported' ((Get-Item $f3).Length -eq $len + 4)
    $got = $null; $failed = $false
    try { $got = [IO.File]::ReadAllText($f3) } catch { $failed = $true }
    Check 'wrong size: the read fails and returns nothing' ($failed -and -not $got) "got='$got'"

    # ---- reader-info mode: the server is asked about the reading process
    $pipe4 = "pfm-projfs4-$PID"; $log4 = Join-Path $work 'ri.log'; $folder4 = Join-Path $work 'mnt4'
    Start-Server "$pipe4-reader" $log4 'allow'
    $m4 = Start-Mount $pipe4 $folder4 (Join-Path $work 'mount4.err') @('--size', $len, '--reader-info-suffix', '-reader')
    $f4 = Join-Path $folder4 'property-file.env'
    $null = Get-ChildItem $folder4; $null = (Get-Item $f4).Length
    Check 'reader-info: stat and listing did not reach the server' ((Conns $log4) -eq 0) "requests $(Conns $log4)"
    $r4 = Start-Process powershell -ArgumentList '-NoProfile', '-Command', "[IO.File]::ReadAllText('$f4') | Out-Null" -PassThru -WindowStyle Hidden
    $r4.WaitForExit(); Start-Sleep -Milliseconds 500
    $reqs = @(Get-Content $log4 | Where-Object { $_ -match 'req' })
    Check 'reader-info: the server was asked once, naming the reading process' ($reqs.Count -eq 1 -and $reqs[0] -match "pid=$($r4.Id)\b") ($reqs -join ';')
}
finally {
    foreach ($p in $mounts) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    foreach ($p in $servers) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    Start-Sleep -Milliseconds 500
    # leftover ProjFS roots are removed without a provider
    Get-ChildItem $work -Directory -ErrorAction SilentlyContinue | ForEach-Object { cmd /c "rmdir /s /q `"$($_.FullName)`"" 2>$null }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0
