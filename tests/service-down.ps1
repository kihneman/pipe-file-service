# Stops and uninstalls PipeFileService and checks that nothing is left behind.
# Run in an ELEVATED PowerShell. If service-smoke.ps1 -LeaveLinked left a registration
# behind, this also checks that stopping the service removed its link.
param(
    [string]$Backend = 'winfsp', [string]$Exe = (Join-Path $PSScriptRoot "..\PipeFileService-$Backend.exe"),
    [string]$Mount = 'C:\ProgramData\PipeFileService\mount'
)
$ErrorActionPreference = 'Continue'
$Exe = [IO.Path]::GetFullPath($Exe)
$left = Join-Path $env:TEMP 'fsp-service-smoke-left'
$leftInfo = Join-Path $left 'info.txt'
if (Test-Path $leftInfo) {
    $f = @(Get-Content $leftInfo)
    "before stop: link attrs = $((Get-Item $f[0] -Force).Attributes)"
}
Stop-Service PipeFileService -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (Test-Path $leftInfo) {
    $it = Get-Item $f[0] -Force
    "after stop:  placeholder attrs = $($it.Attributes), length = $($it.Length)  (expected: no ReparsePoint, length 0)"
}
Get-Content C:\ProgramData\PipeFileService\service.log -Tail 12
& $Exe uninstall
Start-Sleep -Seconds 1
"service present:      $([bool](Get-Service PipeFileService -ErrorAction SilentlyContinue))"
"mount point present:   $(Test-Path $Mount)"
"data folder left:      $(Test-Path 'C:\ProgramData\PipeFileService')"
"parameters key left:   $(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\PipeFileService')"
"process left:          $([bool](Get-Process PipeFileService-* -ErrorAction SilentlyContinue))"
"control pipe left:     $(Test-Path '\\.\pipe\PipeFileService')"
Remove-Item (Join-Path $env:TEMP 'fsp-service-smoke-adminowned') -Force -ErrorAction SilentlyContinue
Remove-Item $left -Recurse -Force -ErrorAction SilentlyContinue
