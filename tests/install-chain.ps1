# End-to-end check of installing PipeFileService BEFORE WinFsp, for a CLEAN VM (Windows 10/11 x64).
# Run in an ELEVATED Windows PowerShell 5.1, with someone logged on interactively (the smoke test is run as
# that user through a scheduled task, unelevated, which is the realistic caller).
#
# Sequence (each step prints PASS/FAIL lines and writes everything to the log):
#   1. WinFsp absent. Install the service (automatic start), `sc start`: it must stop by itself with exit
#      code 0, leave no control pipe and log no Service Control Manager failure event (7000 7023 7024 7031 7032 7034).
#   2. Install the WinFsp MSI silently (driver + DLL + launcher only), `sc start`: the control pipe appears
#      and tests\service-smoke.ps1 passes.
#   3. Stop the service, uninstall WinFsp, `sc start` again: it must stop cleanly again.
#   4. Uninstall the service and check nothing is left.
#
# Copy this repository's PipeFileService.exe, tests\ and the WinFsp MSI (see -WinFspMsi) to the VM.
param(
    [string]$Backend = 'winfsp', [string]$Exe = (Join-Path $PSScriptRoot "..\PipeFileService-$Backend.exe"),
    [Parameter(Mandatory)] [string]$WinFspMsi,
    # The MSI this procedure was written and verified against (winfsp-2.1.25156.msi). A different file is refused unless
    # you pass its own hash, so the script never installs something nobody pinned.
    [string]$WinFspSha256 = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a',
    [string]$ExpectedSigner = 'NAVIMATICS LLC',
    [string]$LogDir = (Join-Path $env:SystemDrive 'fsp-install-chain')
)
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$WinFspMsi = [IO.Path]::GetFullPath($WinFspMsi)
$smoke = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'service-smoke.ps1'))
New-Item -ItemType Directory -Force $LogDir | Out-Null
$log = Join-Path $LogDir 'install-chain.log'
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
# WinFsp 2.x registers its file system driver as "WinFsp+<timestamp>" (Get-Service does not list drivers).
function Test-WinFspDriver { [bool](& sc.exe query type= driver state= all | Select-String -Pattern '^SERVICE_NAME: WinFsp(\+|$)') }
function Test-WinFspInstalled {
    $k = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -ErrorAction SilentlyContinue
    $dll = $k -and (Test-Path (Join-Path $k.InstallDir 'bin\winfsp-x64.dll'))
    [bool]($dll -and (Test-WinFspDriver))
}
function Get-WinFspProductCode {
    $i = New-Object -ComObject WindowsInstaller.Installer
    @($i.RelatedProducts('{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}'))   # WinFsp's UpgradeCode
}
function Invoke-Msi($arguments, $logName) {
    $p = Start-Process msiexec.exe -ArgumentList ($arguments + @('/qn', '/norestart', '/l*v', "`"$(Join-Path $LogDir $logName)`"")) -Wait -PassThru
    $p.ExitCode
}

try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole('Administrators')) { throw 'Run this elevated.' }
    "Windows: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"
    "Service exe: $Exe  $(& $Exe --version)"

    # ---- 0. preconditions
    if ((Test-WinFspInstalled) -or (Test-WinFspDriver) -or (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp')) { throw 'WinFsp is already (partly) installed: use a clean VM.' }
    if (Get-Service $svc -ErrorAction SilentlyContinue) { throw "$svc is already installed: use a clean VM." }
    $hash = (Get-FileHash $WinFspMsi -Algorithm SHA256).Hash.ToLowerInvariant()
    Check 'WinFsp MSI matches the pinned SHA-256' ($hash -eq $WinFspSha256.ToLowerInvariant()) "got $hash"
    $sig = Get-AuthenticodeSignature $WinFspMsi
    Check 'WinFsp MSI Authenticode signature is valid' ($sig.Status -eq 'Valid') $sig.Status
    Check "WinFsp MSI is signed by $ExpectedSigner" ($sig.SignerCertificate.Subject -match [regex]::Escape($ExpectedSigner)) $sig.SignerCertificate.Subject
    if ($script:failures) { throw 'The WinFsp installer was not accepted.' }

    # ---- 1. service installed, WinFsp absent
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
    Check 'no process left' (-not (Get-Process $svc -ErrorAction SilentlyContinue)) 'process still running'
    $svcLog = 'C:\ProgramData\PipeFileService\service.log'
    Check 'service.log says WinFsp is not installed (once)' (@(Select-String -Path $svcLog -Pattern 'WinFsp is not installed').Count -eq 1) ((Get-Content $svcLog -ErrorAction SilentlyContinue) -join "`n")
    Start-Sleep -Seconds 3
    $bad = Get-ScmFailures $t0
    Check 'no Service Control Manager failure events' ($bad.Count -eq 0) (($bad | ForEach-Object { "$($_.Id) $($_.Message)" }) -join "`n")
    # starts again: still harmless
    & sc.exe start $svc | Out-Null
    Check 'a second start is also clean' ((Wait-State $svc 'STOPPED' 15) -and (Get-ExitCodes $svc).Win32 -eq '0' -and (Get-ScmFailures $t0).Count -eq 0) ''

    # ---- 2. install WinFsp silently: driver, DLL and launcher; no developer files
    $t1 = Get-Date
    $rc = Invoke-Msi @('/i', "`"$WinFspMsi`"", 'ADDLOCAL=F.Main,F.User') 'winfsp-install.log'
    Check 'WinFsp install exit code is 0 or 3010' ($rc -in 0, 3010) "exit $rc (see winfsp-install.log)"
    "WinFsp install exit code: $rc"
    $k = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -ErrorAction SilentlyContinue
    Check 'registry InstallDir present' ([bool]$k.InstallDir) ''
    Check 'winfsp-x64.dll present' ([bool]$k -and (Test-Path (Join-Path $k.InstallDir 'bin\winfsp-x64.dll'))) ''
    Check 'developer files not installed' ([bool]$k -and -not (Test-Path (Join-Path $k.InstallDir 'inc'))) ''
    Check 'WinFsp file system driver is registered' ([bool](Test-WinFspDriver)) ''
    $launcher = Get-Service WinFsp.Launcher -ErrorAction SilentlyContinue
    Check 'WinFsp.Launcher service is running' ($launcher -and $launcher.Status -eq 'Running') "$($launcher.Status)"
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
    Register-ScheduledTask -TaskName 'FspInstallChainSmoke' -Action $action -Principal $principal -Force | Out-Null
    Start-ScheduledTask -TaskName 'FspInstallChainSmoke'
    for ($i = 0; $i -lt 240 -and -not (Test-Path $rcf); $i++) { Start-Sleep -Milliseconds 500 }
    Unregister-ScheduledTask -TaskName 'FspInstallChainSmoke' -Confirm:$false
    if (Test-Path $out) { Get-Content $out | Select-Object -Last 15 }
    Check 'service-smoke.ps1 passes (unelevated)' ((Test-Path $rcf) -and ([int](Get-Content $rcf | Select-Object -First 1) -eq 0) -and (Select-String -Path $out -Pattern '^FAIL' -Quiet) -ne $true) "see $out"

    # ---- 3. WinFsp removed again, service started again
    & sc.exe stop $svc | Out-Null
    Check 'service stops' (Wait-State $svc 'STOPPED' 20) ''
    $codes = Get-WinFspProductCode
    "WinFsp product codes: $($codes -join ', ')"
    $rc = 0
    foreach ($c in $codes) { $rc = [Math]::Max($rc, (Invoke-Msi @('/x', $c) 'winfsp-uninstall.log')) }
    Check 'WinFsp uninstall exit code is 0 or 3010' ($rc -in 0, 3010) "exit $rc (see winfsp-uninstall.log)"
    "WinFsp uninstall exit code: $rc"
    Check 'WinFsp is gone' (-not (Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp')) ''
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
    Check 'no process left' (-not (Get-Process $svc -ErrorAction SilentlyContinue)) ''
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
