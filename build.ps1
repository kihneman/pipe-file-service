# Builds the programs (x64) with the MSVC compiler. Finds Visual Studio's C++
# tools itself, so it works from a plain PowerShell.
#
# Each backend gives its own pair of programs, so a shipped exe references exactly one driver:
#   -Backend winfsp  PipeFileService-winfsp.exe, pipe-file-mount-winfsp.exe
#   -Backend dokan   PipeFileService-dokan.exe, pipe-file-mount-dokan.exe
#   -Backend all     both
#
# -WinFsp must point at a folder containing inc\winfsp\winfsp.h and
# lib\winfsp-x64.lib: an installed WinFsp (with the Developer feature) or the
# folder printed by scripts\get-winfsp-sdk.ps1.
# -Dokan must point at a folder containing inc\dokan.h and lib\dokan2.lib: the
# folder printed by scripts\get-dokan-sdk.ps1.
param(
    [ValidateSet('winfsp', 'dokan', 'all')][string]$Backend = 'winfsp',
    [string]$WinFsp = 'C:\Program Files (x86)\WinFsp',
    [string]$Dokan = (Join-Path $PSScriptRoot '.dokan-sdk')
)
$ErrorActionPreference = 'Stop'

function Enter-Msvc {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio C++ build tools not found (no vswhere.exe).' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 tools was found.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -DevCmdArguments '-arch=x64 -host_arch=x64' -SkipAutomaticLocation | Out-Null
}

$buildWinFsp = $Backend -in 'winfsp', 'all'
$buildDokan = $Backend -in 'dokan', 'all'
if ($buildWinFsp -and -not (Test-Path "$WinFsp\inc\winfsp\winfsp.h")) {
    throw "WinFsp SDK not found in $WinFsp. Install WinFsp with the Developer feature, or run scripts\get-winfsp-sdk.ps1."
}
if ($buildDokan -and -not (Test-Path "$Dokan\inc\dokan.h")) {
    throw "Dokan SDK not found in $Dokan. Run scripts\get-dokan-sdk.ps1."
}
Enter-Msvc

Set-Location $PSScriptRoot
# /MT: no Visual C++ redistributable needed. The driver's DLL is delay-loaded so
# the program can say "install the driver" instead of failing in the loader.
function Invoke-Cl($out, $objDir, $sources, $include, $lib, $dll) {
    New-Item -ItemType Directory -Force $objDir | Out-Null
    cl /nologo /W3 /std:c11 /MT "/Fo:$objDir\" "/Fe:$out" @sources /Isrc "/I$include" /link $lib advapi32.lib delayimp.lib "/DELAYLOAD:$dll"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }
}

if ($buildWinFsp) {
    Invoke-Cl 'pipe-file-mount-winfsp.exe' 'build\winfsp-mount' @('src\pfm_core.c', 'src\pfm_backend_winfsp.c') "$WinFsp\inc" "$WinFsp\lib\winfsp-x64.lib" 'winfsp-x64.dll'
    Invoke-Cl 'PipeFileService-winfsp.exe' 'build\winfsp' @('src\pfs_core.c', 'src\pfs_backend_winfsp.c') "$WinFsp\inc" "$WinFsp\lib\winfsp-x64.lib" 'winfsp-x64.dll'
}
if ($buildDokan) {
    Invoke-Cl 'pipe-file-mount-dokan.exe' 'build\dokan-mount' @('src\pfm_core.c', 'src\pfm_backend_dokan.c') "$Dokan\inc" "$Dokan\lib\dokan2.lib" 'dokan2.dll'
    Invoke-Cl 'PipeFileService-dokan.exe' 'build\dokan' @('src\pfs_core.c', 'src\pfs_backend_dokan.c') "$Dokan\inc" "$Dokan\lib\dokan2.lib" 'dokan2.dll'
}
