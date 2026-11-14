# End-to-end check of installing PipeFileService BEFORE its driver (WinFsp or Dokan, see -Backend), for a CLEAN
# VM (Windows 10/11 x64), or for a machine where the driver may be removed first (-RemoveExisting).
# Run in an ELEVATED Windows PowerShell 5.1, with someone logged on interactively (the smoke test is run as
# that user through a scheduled task, unelevated, which is the realistic caller).
#
# Sequence (each step prints PASS/FAIL lines and writes everything to the log):
#   1. Driver absent. Install the service (automatic start), `sc start`: it must stop by itself with exit
#      code 0, leave no control pipe and log no Service Control Manager failure event (7000 7023 7024 7031 7032 7034).
#   2. Install the driver silently, `sc start`: the control pipe appears and tests\service-smoke.ps1 passes.
#   3. Stop the service, uninstall the driver, `sc start` again: it must stop cleanly again.
#   4. Uninstall the service and check nothing is left.
#
# -RemoveExisting first uninstalls a driver that is already installed (so the run does not need a clean VM).
# Dokan keeps its driver service until the next restart after an uninstall; if the existing Dokan leaves a
# restart pending, the script stops and says so: restart, then run it again. After a run that ends with Dokan
# uninstalled, a later Dokan install also needs a restart first.
#
# Copy this repository's PipeFileService-<backend>.exe, tests\ and the driver installer (-DriverInstaller) to the VM.
param(
    [ValidateSet('winfsp', 'dokan')][string]$Backend = 'winfsp',
    [string]$Exe,   # default: ..\PipeFileService-<backend>.exe next to tests\ ($PSScriptRoot is empty in a param default of a script with a mandatory parameter)
    # WinFsp: winfsp-2.1.25156.msi. Dokan: Dokan_x64.msi from Dokany v2.3.1.1000.
    [Parameter(Mandatory)] [Alias('WinFspMsi')] [string]$DriverInstaller,
    # The installers this procedure was written and verified against. A different file is refused unless you pass its
    # own hash, so the script never installs something nobody pinned.
    [string]$DriverSha256,
    [string]$ExpectedSigner,
    [switch]$RemoveExisting,
    [string]$LogDir
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = Join-Path $here "..\PipeFileService-$Backend.exe" }
if (-not $LogDir) { $LogDir = Join-Path $env:SystemDrive 'pfs-install-chain' }
$Exe = [IO.Path]::GetFullPath($Exe)
$DriverInstaller = [IO.Path]::GetFullPath($DriverInstaller)
$smoke = [IO.Path]::GetFullPath((Join-Path $here 'service-smoke.ps1'))
New-Item -ItemType Directory -Force $LogDir | Out-Null
$log = Join-Path $LogDir "install-chain-$Backend.log"
Start-Transcript -Path $log -Force | Out-Null
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
$svc = 'PipeFileService'
$pipePath = "\\.\pipe\$svc"
$scmIds = 7000, 7023, 7024, 7031, 7032, 7034

function Get-ScmFailures($since) {
    $ev = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager'; Id = $scmIds; StartTime = $since } -ErrorAction SilentlyContinue
    @($ev | Where-Object { $_.Message -match 'PipeFileService|Pipe File Service' })
}
function Get-ExitCodes($name) {
    $q = & sc.exe query $name | Out-String
    [pscustomobject]@{
        State = ([regex]::Match($q, 'STATE\s*:\s*\d+\s+(\w+)')).Groups[1].Value
        Win32 = ([regex]::Match($q, 'WIN32_EXIT_CODE\s*:\s*(\d+)')).Groups[1].Value
        Specific = ([regex]::Match($q, 'SERVICE_EXIT_CODE\s*:\s*(\d+)')).Groups[1].Value
    }
}
function Wait-State($name, $state, $seconds) {
    for ($i = 0; $i -lt $seconds * 4; $i++) {
        if ((Get-ExitCodes $name).State -eq $state) { return $true }
        Start-Sleep -Milliseconds 250
    }
    $false
}
function Invoke-Msi($arguments, $logName) {
    $p = Start-Process msiexec.exe -ArgumentList ($arguments + @('/qn', '/norestart', '/l*v', "`"$(Join-Path $LogDir $logName)`"")) -Wait -PassThru
    $p.ExitCode
}

# ---- the driver-specific pieces
if ($Backend -eq 'winfsp') {
    if (-not $DriverSha256) { $DriverSha256 = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a' }   # winfsp-2.1.25156.msi
    if (-not $ExpectedSigner) { $ExpectedSigner = 'NAVIMATICS LLC' }
    $drv = @{ Name = 'WinFsp'; Tag = 'winfsp' }
    # WinFsp 2.x registers its file system driver as "WinFsp+<timestamp>" (Get-Service does not list drivers).
    function Test-DriverService { [bool](& sc.exe query type= driver state= all | Select-String -Pattern '^SERVICE_NAME: WinFsp(\+|$)') }
    function Test-DriverInstalled {
        $k = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -ErrorAction SilentlyContinue
        $dll = $k -and (Test-Path (Join-Path $k.InstallDir 'bin\winfsp-x64.dll'))
        [bool]($dll -and (Test-DriverService))
    }
    function Test-DriverTraces { (Test-DriverInstalled) -or (Test-DriverService) -or (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp') }
    function Get-DriverProductCodes {
        $i = New-Object -ComObject WindowsInstaller.Installer
        @($i.RelatedProducts('{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}'))   # WinFsp's UpgradeCode
    }
    function Install-Driver { Invoke-Msi @('/i', "`"$DriverInstaller`"", 'ADDLOCAL=F.Main,F.User') 'driver-install.log' }   # driver + DLL + launcher; no developer files
    function Test-DriverGone { -not (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp') }
    function Test-DriverPostInstall {
        $k = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -ErrorAction SilentlyContinue
        Check 'registry InstallDir present' ([bool]$k.InstallDir) ''
        Check 'winfsp-x64.dll present' ([bool]$k -and (Test-Path (Join-Path $k.InstallDir 'bin\winfsp-x64.dll'))) ''
        Check 'developer files not installed' ([bool]$k -and -not (Test-Path (Join-Path $k.InstallDir 'inc'))) ''
        Check 'WinFsp file system driver is registered' ([bool](Test-DriverService)) ''
        $launcher = Get-Service WinFsp.Launcher -ErrorAction SilentlyContinue
        Check 'WinFsp.Launcher service is running' ($launcher -and $launcher.Status -eq 'Running') "$($launcher.Status)"
    }
    function Test-RestartPending { $false }
} else {
    if (-not $DriverSha256) { $DriverSha256 = '69ff8cb37bfec3a75921c85ffd1c6370b50a9ec4ecef2cf3a009d488dcbf5465' }   # Dokan_x64.msi, Dokany v2.3.1.1000
    if (-not $ExpectedSigner) { $ExpectedSigner = 'LEOSAC' }
    $drv = @{ Name = 'Dokan'; Tag = 'dokan' }
    function Get-DokanUninstallKeys {
        @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
            Where-Object { (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DisplayName -like 'Dokan Library*' })
    }
    function Test-DriverService { [bool](Get-Service Dokan2 -ErrorAction SilentlyContinue) -or [bool](& sc.exe query Dokan2 2>$null | Select-String 'SERVICE_NAME') }
    function Test-DriverInstalled { (Test-Path (Join-Path $env:SystemRoot 'System32\dokan2.dll')) -and (Test-DriverService) }
    function Test-DriverTraces { (Test-DriverInstalled) -or (Test-DriverService) -or (Get-DokanUninstallKeys).Count -gt 0 -or (Test-Path (Join-Path $env:SystemRoot 'System32\dokan2.dll')) }
    function Get-DriverProductCodes { @(Get-DokanUninstallKeys | ForEach-Object { $_.PSChildName }) }
    function Install-Driver { Invoke-Msi @('/i', "`"$DriverInstaller`"") 'driver-install.log' }
    function Test-DriverGone { ((Get-DokanUninstallKeys).Count -eq 0) -and -not (Test-Path (Join-Path $env:SystemRoot 'System32\dokan2.dll')) }
    # Uninstalling the Dokan driver marks its service for deletion until the next restart, and the MSI refuses to
    # install again while that is pending.
    function Test-RestartPending {
        $k = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\Dokan2' -ErrorAction SilentlyContinue
        [bool]($k -and $k.DeleteFlag -eq 1)
    }
    function Test-DriverPostInstall {
        $dir = [Environment]::GetEnvironmentVariable('DokanLibrary2', 'Machine')
        Check 'dokan2.dll is in the system folder' (Test-Path (Join-Path $env:SystemRoot 'System32\dokan2.dll')) ''
        Check 'Dokan2 driver service is registered' ([bool](Test-DriverService)) ''
        Check 'install folder is set (DokanLibrary2)' ([bool]$dir -and (Test-Path $dir)) "DokanLibrary2=$dir"
        Check 'developer files not installed' ([bool]$dir -and -not (Test-Path (Join-Path $dir 'include')) -and -not (Test-Path (Join-Path $dir 'lib'))) ''
        $d = Get-Service Dokan2 -ErrorAction SilentlyContinue
        Check 'Dokan2 driver is running' ($d -and $d.Status -eq 'Running') "$($d.Status)"
    }
}
function Remove-Driver {
    $rc = 0
    foreach ($c in (Get-DriverProductCodes)) { $rc = [Math]::Max($rc, (Invoke-Msi @('/x', $c) 'driver-uninstall.log')) }
    $rc
}

try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole('Administrators')) { throw 'Run this elevated.' }
    "Windows: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"
    "Backend: $Backend   Service exe: $Exe  $(& $Exe --version)"

    # ---- 0. preconditions
    if (Get-Service $svc -ErrorAction SilentlyContinue) { throw "$svc is already installed: remove it first (tests\service-down.ps1) or use a clean VM." }
    if (Test-DriverTraces) {
        if (-not $RemoveExisting) { throw "$($drv.Name) is already (partly) installed: use a clean VM, or pass -RemoveExisting to uninstall it first." }
        "$($drv.Name) is installed: removing it first (-RemoveExisting)"
        $rc = Remove-Driver
        "$($drv.Name) pre-uninstall exit code: $rc"
        if ($rc -notin 0, 3010) { throw "$($drv.Name) could not be uninstalled (exit $rc, see $LogDir\driver-uninstall.log)." }
        Start-Sleep -Seconds 2
        if ((Test-RestartPending) -or $rc -eq 3010) { throw "$($drv.Name) was uninstalled, but Windows needs a restart before it can be installed again. Restart, then run this script again." }
        if (Test-DriverTraces) { throw "$($drv.Name) is still partly installed after the uninstall; look at $LogDir\driver-uninstall.log." }
    }
    if (Test-RestartPending) { throw "A restart is pending after a $($drv.Name) uninstall: restart, then run this script again." }
    $hash = (Get-FileHash $DriverInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    Check "$($drv.Name) installer matches the pinned SHA-256" ($hash -eq $DriverSha256.ToLowerInvariant()) "got $hash"
    $sig = Get-AuthenticodeSignature $DriverInstaller
    Check "$($drv.Name) installer Authenticode signature is valid" ($sig.Status -eq 'Valid') $sig.Status
    Check "$($drv.Name) installer is signed by $ExpectedSigner" ($sig.SignerCertificate.Subject -match [regex]::Escape($ExpectedSigner)) $sig.SignerCertificate.Subject
    if ($script:failures) { throw "The $($drv.Name) installer was not accepted." }

    # ---- 1. service installed, driver absent
    $t0 = Get-Date
    & $Exe install --auto-start
    Check 'service installed' ($LASTEXITCODE -eq 0) "exit $LASTEXITCODE"
    $cfg = Get-CimInstance Win32_Service -Filter "Name='$svc'"
    Check 'start type is Auto' ($cfg.StartMode -eq 'Auto') $cfg.StartMode
    & sc.exe start $svc | Out-Host
    "sc start exit code: $LASTEXITCODE (informational)"
    Check 'service stops by itself' (Wait-State $svc 'STOPPED' 15) ((Get-ExitCodes $svc) | Out-String)
    $ec = Get-ExitCodes $svc
    Check 'exit code is 0 (not a service error)' ($ec.Win32 -eq '0' -and $ec.Specific -eq '0') ($ec | Out-String)
    Check 'no control pipe' (-not (Test-Path $pipePath)) 'the pipe exists'
    Check 'no process left' (-not (Get-Process "$svc*" -ErrorAction SilentlyContinue)) 'process still running'
    $svcLog = 'C:\ProgramData\PipeFileService\service.log'
    Check "service.log says $($drv.Name) is not installed (once)" (@(Select-String -Path $svcLog -Pattern "$($drv.Name) is not installed").Count -eq 1) ((Get-Content $svcLog -ErrorAction SilentlyContinue) -join "`n")
    Start-Sleep -Seconds 3
    $bad = Get-ScmFailures $t0
    Check 'no Service Control Manager failure events' ($bad.Count -eq 0) (($bad | ForEach-Object { "$($_.Id) $($_.Message)" }) -join "`n")
    # starts again: still harmless
    & sc.exe start $svc | Out-Null
    Check 'a second start is also clean' ((Wait-State $svc 'STOPPED' 15) -and (Get-ExitCodes $svc).Win32 -eq '0' -and (Get-ScmFailures $t0).Count -eq 0) ''

    # ---- 2. install the driver silently
    $t1 = Get-Date
    $rc = Install-Driver
    Check "$($drv.Name) install exit code is 0 or 3010" ($rc -in 0, 3010) "exit $rc (see driver-install.log)"
    "$($drv.Name) install exit code: $rc"
    Test-DriverPostInstall
    & sc.exe start $svc | Out-Host
    $up = $false
    for ($i = 0; $i -lt 60 -and -not $up; $i++) { $up = Test-Path $pipePath; if (-not $up) { Start-Sleep -Milliseconds 250 } }
    Check 'service runs and the control pipe appears' ($up -and (Get-ExitCodes $svc).State -eq 'RUNNING') ((Get-ExitCodes $svc | Out-String) + (Get-Content $svcLog -Tail 5 -ErrorAction SilentlyContinue | Out-String))

    # smoke test as the logged-on user, NOT elevated, through a scheduled task with a limited token
    $out = Join-Path $LogDir 'smoke-output.txt'; $rcf = Join-Path $LogDir 'smoke-exit.txt'
    Remove-Item $out, $rcf -ErrorAction SilentlyContinue
    $cmd = "& '$smoke' *> '$out'; `$LASTEXITCODE | Out-File '$rcf'"
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -Command `"$cmd`""
    $principal = New-ScheduledTaskPrincipal -UserId $id.Name -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName 'PfsInstallChainSmoke' -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName 'PfsInstallChainSmoke'
    for ($i = 0; $i -lt 240 -and -not (Test-Path $rcf); $i++) { Start-Sleep -Milliseconds 500 }
    Unregister-ScheduledTask -TaskName 'PfsInstallChainSmoke' -Confirm:$false
    if (Test-Path $out) { Get-Content $out | Select-Object -Last 15 }
    Check 'service-smoke.ps1 passes (unelevated)' ((Test-Path $rcf) -and ([int](Get-Content $rcf | Select-Object -First 1) -eq 0) -and (Select-String -Path $out -Pattern '^FAIL' -Quiet) -ne $true) "see $out"

    # ---- 3. driver removed again, service started again
    & sc.exe stop $svc | Out-Null
    Check 'service stops' (Wait-State $svc 'STOPPED' 20) ''
    $codes = Get-DriverProductCodes
    "$($drv.Name) product codes: $($codes -join ', ')"
    $rc = Remove-Driver
    Check "$($drv.Name) uninstall exit code is 0 or 3010" ($rc -in 0, 3010) "exit $rc (see driver-uninstall.log)"
    "$($drv.Name) uninstall exit code: $rc"
    Check "$($drv.Name) is gone" (Test-DriverGone) ''
    $t2 = Get-Date
    & sc.exe start $svc | Out-Null
    Check 'service stops by itself again' (Wait-State $svc 'STOPPED' 15) ((Get-ExitCodes $svc) | Out-String)
    Check 'exit code is 0 again' ((Get-ExitCodes $svc).Win32 -eq '0') ''
    Check 'no control pipe again' (-not (Test-Path $pipePath)) ''
    Start-Sleep -Seconds 3
    $bad = Get-ScmFailures $t0
    Check 'no Service Control Manager failure events in the whole run' ($bad.Count -eq 0) (($bad | ForEach-Object { "$($_.Id) $($_.Message)" }) -join "`n")

    # ---- 4. remove the service
    & $Exe uninstall
    Start-Sleep -Seconds 1
    Check 'service removed' (-not (Get-Service $svc -ErrorAction SilentlyContinue)) ''
    Check 'data folder removed' (-not (Test-Path 'C:\ProgramData\PipeFileService')) ''
    Check 'no process left' (-not (Get-Process "$svc*" -ErrorAction SilentlyContinue)) ''
    if (Test-RestartPending) { "NOTE  $($drv.Name) is uninstalled but Windows keeps its driver service until the next restart; restart before installing $($drv.Name) again." }
}
catch { "ERROR  $($_.Exception.Message)"; $script:failures++ }
finally {
    "Event log (System, Service Control Manager, this run, PipeFileService only):"
    if ($t0) { Get-ScmFailures $t0 | ForEach-Object { "  $($_.TimeCreated) $($_.Id) $($_.Message)" } }
    if ($script:failures) { "$($script:failures) FAILED" } else { 'ALL PASSED' }
    "Logs are in $LogDir"
    Stop-Transcript | Out-Null
}
exit ([int]($script:failures -gt 0))
