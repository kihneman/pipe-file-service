# Checks `install --display-name` and `--description`: the labels in the Services list change, the service name does not.
# Run in an ELEVATED Windows PowerShell with no PipeFileService installed. Installs the service without starting it, then removes it.
param([ValidateSet('winfsp', 'dokan', 'projfs')][string]$Backend = 'winfsp', [string]$Exe = (Join-Path $PSScriptRoot "..\PipeFileService-$Backend.exe"))
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole('Administrators')) { throw 'Run this elevated.' }
if (Get-Service PipeFileService -ErrorAction SilentlyContinue) { throw 'PipeFileService is already installed; run service-down.ps1 first.' }
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
function Description { (& sc.exe qdescription PipeFileService | Out-String) }
$ErrorActionPreference = 'Continue'   # native stderr must not throw

try {
    # ---- defaults
    & $Exe install | Out-Null
    $s = Get-Service PipeFileService
    Check 'default display name' ($s.DisplayName -eq 'Pipe File Service') $s.DisplayName
    Check 'default description names the backend' ((Description) -match 'Shows named pipes as read-only files') (Description)
    & $Exe uninstall | Out-Null

    # ---- custom labels
    $name = 'Keeper Pipe File Service'; $desc = 'Shows Keeper secrets as read-only files for apps that cannot use a pipe.'
    & $Exe install --display-name $name --description $desc | Out-Null
    $s = Get-Service PipeFileService
    Check 'the service name stays PipeFileService' ($s.Name -eq 'PipeFileService')
    Check 'display name is the one given' ($s.DisplayName -eq $name) $s.DisplayName
    Check 'description is the one given' ((Description) -match [regex]::Escape($desc)) (Description)
    Check 'the Services list finds it by the display name' ($null -ne (Get-Service -DisplayName $name -ErrorAction SilentlyContinue))
    & $Exe uninstall | Out-Null
    Check 'uninstall removes it' (-not (Get-Service PipeFileService -ErrorAction SilentlyContinue))

    # ---- only the display name
    & $Exe install --display-name 'Only A Name' | Out-Null
    $s = Get-Service PipeFileService
    Check 'display name alone' ($s.DisplayName -eq 'Only A Name') $s.DisplayName
    Check 'and the default description stays' ((Description) -match 'Shows named pipes as read-only files') (Description)
    & $Exe uninstall | Out-Null

    # ---- bad values are refused and install nothing
    foreach ($bad in @(@('--display-name', ''), @('--display-name', ('x' * 300)), @('--description', ''), @('--description', ('y' * 600)), @('--display-name'))) {
        $out = & $Exe install @bad 2>&1 | Out-String
        $label = "$($bad[0]) $(if ($bad.Count -gt 1) { "($($bad[1].Length) chars)" } else { '(no value)' }) is refused"
        Check $label (($LASTEXITCODE -eq 2) -and -not (Get-Service PipeFileService -ErrorAction SilentlyContinue)) "exit $LASTEXITCODE"
        if (Get-Service PipeFileService -ErrorAction SilentlyContinue) { & $Exe uninstall | Out-Null }
    }
    $out = & $Exe run --display-name 'x' 2>&1 | Out-String
    Check 'run does not take the labels' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"
}
finally {
    if (Get-Service PipeFileService -ErrorAction SilentlyContinue) { & $Exe uninstall | Out-Null }
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0
