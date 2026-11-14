# Builds the release zips and checksums into dist\: one zip per file system driver, so a download
# carries the programs of exactly one driver.
#   .\scripts\release.ps1                      # local build, version from the source
#   .\scripts\release.ps1 -Tag v0.1.0          # also checks the tag matches the source version
#   .\scripts\release.ps1 -Tag v0.1.0-rc.1     # a pre-release tag: version plus a suffix
# -WinFsp is the WinFsp SDK folder and -Dokan the Dokan SDK folder for build.ps1
# (defaults: an installed WinFsp; the folder scripts\get-dokan-sdk.ps1 writes to).
#
# dist\ gets:
#   pipe-file-service-<version>-winfsp-windows-x64.zip   PipeFileService-winfsp.exe, pipe-file-mount-winfsp.exe
#   pipe-file-service-<version>-dokan-windows-x64.zip    PipeFileService-dokan.exe, pipe-file-mount-dokan.exe
#   SHA256SUMS                                           both zips
#   RELEASE_NOTES.md                                     includes the SHA-256 of every exe
#   exe\*.exe                                            the four exes, for the build attestation (not published as assets)
param(
    [string]$Tag = '',
    [string]$WinFsp = 'C:\Program Files (x86)\WinFsp',
    [string]$Dokan = (Join-Path $PSScriptRoot '..\.dokan-sdk'),
    [string]$Out = (Join-Path $PSScriptRoot '..\dist')
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Out = [IO.Path]::GetFullPath($Out)
$Dokan = [IO.Path]::GetFullPath($Dokan)

# The single source of the version is the #define in the C file.
$source = Get-Content (Join-Path $root 'src\pfm_core.c') -Raw
if ($source -notmatch '#define MOUNT_VERSION "([^"]+)"') { throw 'MOUNT_VERSION not found in the source.' }
$version = $Matches[1]

$releaseVersion = $version
if ($Tag) {
    if ($Tag -notmatch ('^v' + [regex]::Escape($version) + '(-[0-9A-Za-z.]+)?$')) {
        throw "Tag '$Tag' does not match the source version '$version' (expected v$version or v$version-<suffix>)."
    }
    $releaseVersion = $Tag.Substring(1)
}

& (Join-Path $root 'build.ps1') -Backend all -WinFsp $WinFsp -Dokan $Dokan
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "build.ps1 failed with exit code $LASTEXITCODE" }

$backends = @(
    @{ Key = 'winfsp'; Title = 'WinFsp' },
    @{ Key = 'dokan'; Title = 'Dokan' }
)
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory $Out, (Join-Path $Out 'exe') | Out-Null

$sums = ''
$hashRows = ''
$zipNames = @{}
foreach ($b in $backends) {
    $mountExe = Join-Path $root "pipe-file-mount-$($b.Key).exe"
    $svcExe = Join-Path $root "PipeFileService-$($b.Key).exe"
    $reported = (& $mountExe --version)
    if ($reported -ne "pipe-file-mount $version") { throw "$mountExe reports '$reported', expected 'pipe-file-mount $version'." }
    $reported = (& $svcExe --version)
    if ($reported -ne "PipeFileService $version") { throw "$svcExe reports '$reported', expected 'PipeFileService $version'." }

    $name = "pipe-file-service-$releaseVersion-$($b.Key)-windows-x64"
    $stage = Join-Path $Out $name
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item $mountExe, $svcExe, (Join-Path $root 'LICENSE'), (Join-Path $root 'README.md') $stage
    $zip = Join-Path $Out "$name.zip"
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Remove-Item $stage -Recurse -Force
    Copy-Item $mountExe, $svcExe (Join-Path $Out 'exe')

    $zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $sums += "$zipHash  $name.zip`n"
    $zipNames[$b.Key] = $name
    foreach ($f in $svcExe, $mountExe) {
        $hashRows += "$((Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant())  $(Split-Path $f -Leaf)`n"
    }
}
# LF endings and no BOM, so `sha256sum -c SHA256SUMS` works on Linux, WSL and Git Bash
# (Set-Content would write CRLF).
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS'), $sums, (New-Object Text.UTF8Encoding($false)))

$winfspZip = $zipNames['winfsp']
$dokanZip = $zipNames['dokan']
$notes = @"
pipe-file-service $releaseVersion (experimental, Windows x64)

Shows a Windows named pipe as a read-only file: ``pipe-file-mount-<driver>.exe`` does it for one pipe in
one process; ``PipeFileService-<driver>.exe`` is a Windows service that does it for any local user's app and
can link placeholder files to the pipes (see the README). It runs on a file system driver, and there is one
download per driver. Each executable references exactly one driver, and the driver is not included.
The service name (``PipeFileService``) and its control pipe are the same for both.

| download | driver you install separately |
|---|---|
| ``$winfspZip.zip`` | WinFsp: https://github.com/winfsp/winfsp/releases |
| ``$dokanZip.zip`` | Dokan 2.x: https://github.com/dokan-dev/dokany/releases |

**The binaries are not Authenticode-signed**, so Windows SmartScreen may warn. Check the zips against
``SHA256SUMS``. SHA-256 of the programs inside the zips:

``````
$hashRows
``````

The files carry a signed GitHub build attestation (built by this repository's release
workflow from this tag), for the zips and for each executable. Verify with the GitHub CLI:

``````
gh attestation verify $winfspZip.zip -R kihneman/pipe-file-service
gh attestation verify PipeFileService-dokan.exe -R kihneman/pipe-file-service
``````

Notes: uninstalling Dokan keeps its driver service until the next restart, and the Dokan installer refuses
to install again until then.

> WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
> https://github.com/winfsp/winfsp
>
> Dokan (Dokany), https://github.com/dokan-dev/dokany

The WinFsp programs link to the WinFsp DLL under WinFsp's FLOSS exception; do not combine them with
proprietary software. The Dokan programs load Dokan's LGPL library dynamically and do not contain it. This
project is MIT licensed.
"@
# No byte-order mark: Windows PowerShell's -Encoding utf8 would add one.
[IO.File]::WriteAllText((Join-Path $Out 'RELEASE_NOTES.md'), $notes, (New-Object Text.UTF8Encoding($false)))

Write-Host "Built $(($zipNames.Values | ForEach-Object { "$_.zip" }) -join ', ')"
Write-Host $sums
