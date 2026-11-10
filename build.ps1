# Builds winfsp-pipe-bridge.exe with the MSVC compiler from a Visual Studio
# developer shell. Requires WinFsp with its Developer (SDK) feature installed.
param([string]$WinFsp = 'C:\Program Files (x86)\WinFsp')

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error 'cl.exe not found: run this from a "x64 Native Tools" / Developer PowerShell for Visual Studio.'
    exit 1
}
if (-not (Test-Path "$WinFsp\inc\winfsp\winfsp.h")) {
    Write-Error "WinFsp SDK not found in $WinFsp. Install WinFsp with the Developer feature."
    exit 1
}

Set-Location $PSScriptRoot
cl /nologo /W3 /std:c11 /Fe:winfsp-pipe-bridge.exe src\winfsp-pipe-bridge.c `
    /I"$WinFsp\inc" /link "$WinFsp\lib\winfsp-x64.lib" advapi32.lib
exit $LASTEXITCODE
