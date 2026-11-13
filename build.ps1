# Builds pipe-file-mount.exe (x64) with the MSVC compiler. Finds Visual
# Studio's C++ tools itself, so it works from a plain PowerShell.
# -WinFsp must point at a folder containing inc\winfsp\winfsp.h and
# lib\winfsp-x64.lib: an installed WinFsp (with the Developer feature) or the
# folder printed by scripts\get-winfsp-sdk.ps1.
param([string]$WinFsp = 'C:\Program Files (x86)\WinFsp')
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

if (-not (Test-Path "$WinFsp\inc\winfsp\winfsp.h")) {
    throw "WinFsp SDK not found in $WinFsp. Install WinFsp with the Developer feature, or run scripts\get-winfsp-sdk.ps1."
}
Enter-Msvc

Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null
# /MT: no Visual C++ redistributable needed. winfsp-x64.dll is delay-loaded so
# the program can say "install WinFsp" instead of failing in the loader.
cl /nologo /W3 /std:c11 /MT /Fo:build\ /Fe:pipe-file-mount.exe src\pipe-file-mount.c `
    /I"$WinFsp\inc" /link "$WinFsp\lib\winfsp-x64.lib" advapi32.lib delayimp.lib /DELAYLOAD:winfsp-x64.dll
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }

cl /nologo /W3 /std:c11 /MT /Fo:build\ /Fe:PipeFileService.exe src\PipeFileService.c `
    /I"$WinFsp\inc" /link "$WinFsp\lib\winfsp-x64.lib" advapi32.lib delayimp.lib /DELAYLOAD:winfsp-x64.dll
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }
