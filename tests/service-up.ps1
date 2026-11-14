# Installs and starts PipeFileService for testing. Run in an ELEVATED PowerShell.
# Then run tests\service-smoke.ps1 (an ordinary, unelevated PowerShell is the realistic caller),
# and finally tests\service-down.ps1.
param(
    [string]$Backend = 'winfsp', [string]$Exe = (Join-Path $PSScriptRoot "..\PipeFileService-$Backend.exe"),
    [string]$Mount = 'C:\ProgramData\PipeFileService\mount'
)
$ErrorActionPreference = 'Stop'
$Exe = [IO.Path]::GetFullPath($Exe)
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole('Administrators')) { throw 'Run this elevated.' }
if (Get-Service PipeFileService -ErrorAction SilentlyContinue) { throw 'PipeFileService is already installed; run service-down.ps1 first.' }
# For the owner check: an empty file owned by Administrators that the current user may still write.
$adminOwned = Join-Path $env:TEMP 'fsp-service-smoke-adminowned'
Remove-Item $adminOwned -Force -ErrorAction SilentlyContinue
New-Item $adminOwned -ItemType File | Out-Null
icacls $adminOwned /grant "*$($id.User.Value):(M)" | Out-Null
& $Exe install --mount $Mount
if ($LASTEXITCODE) { throw 'install failed' }
Start-Service PipeFileService
Start-Sleep -Seconds 3
Get-Service PipeFileService | Format-List Name, DisplayName, Status
sc.exe qprivs PipeFileService
sc.exe qsidtype PipeFileService
Get-Item $Mount -Force -ErrorAction SilentlyContinue | Format-List FullName, Attributes
Get-Content C:\ProgramData\PipeFileService\service.log -Tail 5
