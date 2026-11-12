# Installs and starts FspPipeFileService for testing. Run in an ELEVATED PowerShell.
# Then run tests\service-smoke.ps1 (an ordinary, unelevated PowerShell is the realistic caller),
# and finally tests\service-down.ps1.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\FspPipeFileService.exe'),
    [string]$Mount = 'C:\ProgramData\FspPipeFileService\mount'
)
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole('Administrators')) { throw 'Run this elevated.' }
if (Get-Service FspPipeFileService -ErrorAction SilentlyContinue) { throw 'FspPipeFileService is already installed; run service-down.ps1 first.' }
# For the owner check: an empty file owned by Administrators that the current user may still write.
$adminOwned = Join-Path $env:TEMP 'fsp-service-smoke-adminowned'
Remove-Item $adminOwned -Force -ErrorAction SilentlyContinue
New-Item $adminOwned -ItemType File | Out-Null
icacls $adminOwned /grant "*$($id.User.Value):(M)" | Out-Null
& $Exe install --mount $Mount
if ($LASTEXITCODE) { throw 'install failed' }
Start-Service FspPipeFileService
Start-Sleep -Seconds 3
Get-Service FspPipeFileService | Format-List Name, DisplayName, Status
sc.exe qprivs FspPipeFileService
sc.exe qsidtype FspPipeFileService
Get-Item $Mount -Force -ErrorAction SilentlyContinue | Format-List FullName, Attributes
Get-Content C:\ProgramData\FspPipeFileService\service.log -Tail 5
