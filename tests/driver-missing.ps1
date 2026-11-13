# Tests how PipeFileService behaves when WinFsp is missing, without needing a machine that lacks WinFsp:
# `run --winfsp-dir <folder>` (a development switch that only exists in run mode) makes the WinFsp check look
# for <folder>\bin\winfsp-x64.dll instead of the real install.
#
#   missing at start-up   exit code 0, a log line, no control pipe        (runs unelevated)
#   gone at register      `register` answers driver_not_installed          (needs an ELEVATED shell and the
#   present at start-up   the control pipe exists and registration works    real WinFsp: it really mounts a volume)
#
# Run it from Windows PowerShell 5.1. -Exe selects the binary (the mutation check passes a broken build).
param([string]$Exe = (Join-Path $PSScriptRoot '..\PipeFileService.exe'))
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$work = Join-Path $env:TEMP "fsp-driver-missing-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
$elevated = (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole('Administrators')
$pipePath = '\\.\pipe\PipeFileService'
# A real service already running owns the control pipe: then "no pipe" cannot be judged and the elevated part would collide.
$busy = Test-Path $pipePath
if ($busy) { 'NOTE  a PipeFileService is already running (the control pipe exists): skipping the checks that need the pipe to be free' }

function Start-Svc($mount, $winfspDir) {
    $a = @('run', '--mount', "`"$mount`"")
    if ($winfspDir) { $a += @('--winfsp-dir', "`"$winfspDir`"") }
    $err = Join-Path $work ("err-" + [guid]::NewGuid().ToString('N') + '.txt')
    $p = Start-Process -FilePath $Exe -ArgumentList $a -WindowStyle Hidden -PassThru -RedirectStandardError $err
    $null = $p.Handle   # PowerShell 5.1 loses ExitCode unless the handle is touched while the process runs
    [pscustomobject]@{ Proc = $p; Err = $err }
}
function Wait-Pipe($p, $seconds) {
    for ($i = 0; $i -lt $seconds * 10; $i++) {
        if (Test-Path $pipePath) { return $true }
        if ($p.Proc.HasExited) { return $false }
        Start-Sleep -Milliseconds 100
    }
    $false
}
function Stop-Svc($p) {
    if (-not $p.Proc.HasExited) { Stop-Process -Id $p.Proc.Id -Force; $p.Proc.WaitForExit(5000) | Out-Null }
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

# A fake "WinFsp install" for the override: only the file the check looks for.
$fake = Join-Path $work 'fake-winfsp'
$empty = Join-Path $work 'no-winfsp'
New-Item -ItemType Directory $empty | Out-Null

# ---- 1. WinFsp missing at start-up (unelevated is enough: the service gives up before touching anything)
$p = Start-Svc (Join-Path $work 'mount1') $empty
$exited = $p.Proc.WaitForExit(10000)
Check 'missing: the process exits by itself' $exited 'still running after 10 s'
if ($exited) {
    Check 'missing: exit code is 0' ($p.Proc.ExitCode -eq 0) "exit code $($p.Proc.ExitCode)"
}
Stop-Svc $p
$log = Get-Content $p.Err -Raw
Check 'missing: one log line says WinFsp is not installed' ($log -match 'WinFsp is not installed') $log
if ($busy) { 'SKIP  missing: no control pipe' } else { Check 'missing: no control pipe' (-not (Test-Path $pipePath)) 'the control pipe exists' }
Check 'missing: nothing mounted' (-not (Test-Path (Join-Path $work 'mount1'))) 'mount folder exists'

# ---- 2. the override is run-mode only
$ErrorActionPreference = 'Continue'   # native stderr must not throw
$out = & $Exe service --winfsp-dir $empty 2>&1 | Out-String
Check 'override is refused outside run mode' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"
$out = & $Exe install --winfsp-dir $empty 2>&1 | Out-String
Check 'override is refused by install' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"
$ErrorActionPreference = 'Stop'

# ---- 3 and 4 need the real WinFsp and an elevated shell
if ($busy -or -not $elevated) {
    'SKIP  present-at-start and gone-at-register: need an elevated shell and no running PipeFileService'
} else {
    New-Item -ItemType Directory (Join-Path $fake 'bin') -Force | Out-Null
    $dummy = Join-Path $fake 'bin\winfsp-x64.dll'
    Set-Content $dummy 'not a dll'
    $p = Start-Svc (Join-Path $work 'mount2') $fake
    $up = Wait-Pipe $p 20
    Check 'present: the control pipe appears' $up (Get-Content $p.Err -Raw)
    if ($up) {
        $data = New-Object IO.Pipes.NamedPipeServerStream("fsp-dm-$PID", 'Out', 4, 'Byte', 'Asynchronous')
        $c = New-Client
        $r = Req $c @{ type = 'register'; id = '1'; pipe = "\\.\pipe\fsp-dm-$PID"; file_name = 'a.env' }
        Check 'present: register works' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
        Remove-Item $dummy -Force   # WinFsp "disappears" while the service runs
        $r = Req $c @{ type = 'register'; id = '2'; pipe = "\\.\pipe\fsp-dm-$PID"; file_name = 'b.env' }
        Check 'gone: register answers driver_not_installed' ($r.status -eq 'error' -and $r.code -eq 'driver_not_installed') ($r | ConvertTo-Json -Compress)
        $r = Req $c @{ type = 'unregister'; id = '3'; pipe = "\\.\pipe\fsp-dm-$PID" }
        Check 'gone: the earlier registration is left alone (unregister still finds it)' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
        $c.Dispose(); $data.Dispose()
    }
    Stop-Svc $p
    # real WinFsp, no override
    Start-Sleep -Seconds 1
    $p = Start-Svc (Join-Path $work 'mount3') $null
    Check 'real WinFsp: the control pipe appears' (Wait-Pipe $p 20) (Get-Content $p.Err -Raw)
    Stop-Svc $p
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($script:failures) { "$($script:failures) FAILED"; exit 1 } else { 'ALL PASSED' }
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
