# Tests how PipeFileService behaves when its driver (WinFsp, Dokan or ProjFS, see -Backend) is missing, without needing a
# machine that lacks it: `run --winfsp-dir <folder>` / `run --dokan-dir <folder>` / `run --projfs-dir <folder>` (a development
# switch that only exists in run mode) makes the check look for <folder>\bin\winfsp-x64.dll / <folder>\dokan2.dll /
# <folder>\projectedfslib.dll instead of the real install.
#
#   override only in run mode       `service` and `install` refuse the switch                (runs unelevated)
#   driver missing at start-up      the service stays up with its control pipe; `register` answers driver_not_installed;
#                                   `register` with defer waits (mounted:false) and the mount appears, with a `mounted` event,
#                                   once the driver does                                      (needs an ELEVATED shell, and
#   gone at register                `register` answers driver_not_installed                    the real driver: it really mounts)
#   present at start-up             the control pipe exists and registration works
#
# Run it from Windows PowerShell 5.1. -Exe selects the binary (the mutation check passes a broken build).
param([ValidateSet('winfsp', 'dokan', 'projfs')][string]$Backend = 'winfsp', [string]$Exe = (Join-Path $PSScriptRoot "..\PipeFileService-$Backend.exe"))
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
# The driver-specific pieces: the name in messages, the development switch, and the file the switch looks for.
$drv = switch ($Backend) {
    'dokan' { @{ Name = 'Dokan'; Flag = '--dokan-dir'; Dll = 'dokan2.dll' } }
    'projfs' { @{ Name = 'Windows Projected File System'; Flag = '--projfs-dir'; Dll = 'projectedfslib.dll' } }
    default { @{ Name = 'WinFsp'; Flag = '--winfsp-dir'; Dll = 'bin\winfsp-x64.dll' } }
}
$work = Join-Path $env:TEMP "fsp-driver-missing-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
$elevated = (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole('Administrators')
$pipePath = '\\.\pipe\PipeFileService'
# A real service already running owns the control pipe: the elevated part would collide with it.
$busy = Test-Path $pipePath
if ($busy) { 'NOTE  a PipeFileService is already running (the control pipe exists): skipping the checks that need the pipe to be free' }

function Start-Svc($mount, $driverDir) {
    $a = @('run', '--mount', "`"$mount`"")
    if ($driverDir) { $a += @($drv.Flag, "`"$driverDir`"") }
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
function Reg($c, $h) {
    $r = Req $c $h
    if ($r.code -eq 'size_required') { $h.size = 11; $r = Req $c $h }   # the ProjFS build needs a size
    $r
}

# A fake driver install for the override: only the file the check looks for.
$fake = Join-Path $work 'fake-driver'
$empty = Join-Path $work 'no-driver'
New-Item -ItemType Directory $empty | Out-Null

# ---- 1. the override is run-mode only (unelevated is enough)
$ErrorActionPreference = 'Continue'   # native stderr must not throw
$out = & $Exe service $drv.Flag $empty 2>&1 | Out-String
Check 'override is refused outside run mode' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"
$out = & $Exe install $drv.Flag $empty 2>&1 | Out-String
Check 'override is refused by install' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"
$ErrorActionPreference = 'Stop'

# ---- the rest needs the real driver and an elevated shell
if ($busy -or -not $elevated) {
    'SKIP  missing / deferred / present / gone: need an elevated shell and no running PipeFileService'
} else {
    # ---- 2. driver missing at start-up: the service stays up; plain register is refused; deferred register waits
    $mount1 = Join-Path $work 'mount1'
    $data = New-Object IO.Pipes.NamedPipeServerStream("fsp-dm-$PID", 'Out', 4, 'Byte', 'Asynchronous')
    $p = Start-Svc $mount1 $empty
    $up = Wait-Pipe $p 20
    Check 'missing: the control pipe appears' $up (Get-Content $p.Err -Raw)
    if ($up) {
        Start-Sleep -Seconds 2
        Check 'missing: the process keeps running' (-not $p.Proc.HasExited) "exit code $($p.Proc.ExitCode)"
        Check "missing: the log says $($drv.Name) is not installed" ((Get-Content $p.Err -Raw) -match "$($drv.Name) is not installed") (Get-Content $p.Err -Raw)
        Check 'missing: nothing is mounted' (-not (Test-Path $mount1))
        $c = New-Client
        $r = Reg $c @{ type = 'register'; id = '1'; pipe = "\\.\pipe\fsp-dm-$PID"; file_name = 'a.env' }
        Check 'missing: register answers driver_not_installed' ($r.status -eq 'error' -and $r.code -eq 'driver_not_installed') ($r | ConvertTo-Json -Compress)
        $r = Reg $c @{ type = 'register'; id = '2'; pipe = "\\.\pipe\fsp-dm-$PID"; file_name = 'a.env'; defer = $true }
        Check 'missing: register with defer is accepted and not mounted' ($r.status -eq 'ok' -and $r.mounted -eq $false) ($r | ConvertTo-Json -Compress)
        Check 'missing: nothing is mounted for a deferred registration either' (-not (Test-Path $mount1))
        $r = Req $c @{ type = 'retry_mount'; id = '3' }
        Check 'missing: retry_mount answers not mounted' ($r.status -eq 'ok' -and $r.mounted -eq $false) ($r | ConvertTo-Json -Compress)

        # the driver "gets installed": the file the check looks for appears
        $dummy = Join-Path $empty $drv.Dll
        New-Item -ItemType Directory (Split-Path $dummy) -Force | Out-Null
        Set-Content $dummy 'not a dll'
        $got = $false
        for ($i = 0; $i -lt 40 -and -not $got; $i++) {
            $null = Req $c @{ type = 'ping' }   # pushed events are delivered ahead of the next reply
            $got = @($script:events | Where-Object { $_.event -eq 'mounted' }).Count -gt 0
            if (-not $got) { Start-Sleep -Milliseconds 500 }
        }
        Check 'deferred: the mount appears once the driver does' (Test-Path $mount1) "mount missing; log: $(Get-Content $p.Err -Raw)"
        if ($c) { $c.Dispose() }
    }
    Stop-Svc $p
    $data.Dispose()
    Start-Sleep -Seconds 1

    # ---- 3. present at start-up, then gone
    $dummy = Join-Path $fake $drv.Dll
    New-Item -ItemType Directory (Split-Path $dummy) -Force | Out-Null
    Set-Content $dummy 'not a dll'
    $p = Start-Svc (Join-Path $work 'mount2') $fake
    $up = Wait-Pipe $p 20
    Check 'present: the control pipe appears' $up (Get-Content $p.Err -Raw)
    if ($up) {
        $data = New-Object IO.Pipes.NamedPipeServerStream("fsp-dm2-$PID", 'Out', 4, 'Byte', 'Asynchronous')
        $c = New-Client
        $r = Reg $c @{ type = 'register'; id = '1'; pipe = "\\.\pipe\fsp-dm2-$PID"; file_name = 'a.env' }
        Check 'present: register works' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
        Remove-Item $dummy -Force   # the driver "disappears" while the service runs
        $r = Reg $c @{ type = 'register'; id = '2'; pipe = "\\.\pipe\fsp-dm2-$PID"; file_name = 'b.env' }
        Check 'gone: register answers driver_not_installed' ($r.status -eq 'error' -and $r.code -eq 'driver_not_installed') ($r | ConvertTo-Json -Compress)
        $r = Req $c @{ type = 'unregister'; id = '3'; pipe = "\\.\pipe\fsp-dm2-$PID" }
        Check 'gone: the earlier registration is left alone (unregister still finds it)' ($r.status -eq 'ok') ($r | ConvertTo-Json -Compress)
        $c.Dispose(); $data.Dispose()
    }
    Stop-Svc $p
    # real driver, no override
    Start-Sleep -Seconds 1
    $p = Start-Svc (Join-Path $work 'mount3') $null
    Check "real $($drv.Name): the control pipe appears" (Wait-Pipe $p 20) (Get-Content $p.Err -Raw)
    Stop-Svc $p
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($script:failures) { "$($script:failures) FAILED"; exit 1 } else { 'ALL PASSED' }
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
