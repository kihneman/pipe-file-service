# Stops and uninstalls FspPipeFileService and checks that nothing is left behind.
# Run in an ELEVATED PowerShell. If service-smoke.ps1 -LeaveLinked left a registration
# behind, this also checks that stopping the service removed its link.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\FspPipeFileService.exe'),
    [string]$Mount = 'C:\ProgramData\FspPipeFileService\mount'
)
$ErrorActionPreference = 'Continue'
$Exe = [IO.Path]::GetFullPath($Exe)
$left = Join-Path $env:TEMP 'fsp-service-smoke-left'
$leftInfo = Join-Path $left 'info.txt'
if (Test-Path $leftInfo) {
    $f = @(Get-Content $leftInfo)
    "before stop: link attrs = $((Get-Item $f[0] -Force).Attributes)"
}
Stop-Service FspPipeFileService -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (Test-Path $leftInfo) {
    $it = Get-Item $f[0] -Force
    "after stop:  placeholder attrs = $($it.Attributes), length = $($it.Length)  (expected: no ReparsePoint, length 0)"
}
Get-Content C:\ProgramData\FspPipeFileService\service.log -Tail 12
& $Exe uninstall
Start-Sleep -Seconds 1
"service present:      $([bool](Get-Service FspPipeFileService -ErrorAction SilentlyContinue))"
"mount point present:   $(Test-Path $Mount)"
"data folder left:      $(Test-Path 'C:\ProgramData\FspPipeFileService')"
"parameters key left:   $(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\FspPipeFileService')"
"process left:          $([bool](Get-Process FspPipeFileService -ErrorAction SilentlyContinue))"
"control pipe left:     $(Test-Path '\\.\pipe\FspPipeFileService')"
Remove-Item (Join-Path $env:TEMP 'fsp-service-smoke-adminowned') -Force -ErrorAction SilentlyContinue
Remove-Item $left -Recurse -Force -ErrorAction SilentlyContinue
