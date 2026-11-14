# Builds the release zip and checksums into dist\.
#   .\scripts\release.ps1                      # local build, version from the source
#   .\scripts\release.ps1 -Tag v0.1.0          # also checks the tag matches the source version
#   .\scripts\release.ps1 -Tag v0.1.0-rc.1     # a pre-release tag: version plus a suffix
# -WinFsp is the SDK folder for build.ps1 (default: an installed WinFsp).
param(
    [string]$Tag = '',
    [string]$WinFsp = 'C:\Program Files (x86)\WinFsp',
    [string]$Out = (Join-Path $PSScriptRoot '..\dist')
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Out = [IO.Path]::GetFullPath($Out)

# The single source of the version is the #define in the C file.
$source = Get-Content (Join-Path $root 'src\pipe-file-mount.c') -Raw
if ($source -notmatch '#define MOUNT_VERSION "([^"]+)"') { throw 'MOUNT_VERSION not found in the source.' }
$version = $Matches[1]

$releaseVersion = $version
if ($Tag) {
    if ($Tag -notmatch ('^v' + [regex]::Escape($version) + '(-[0-9A-Za-z.]+)?$')) {
        throw "Tag '$Tag' does not match the source version '$version' (expected v$version or v$version-<suffix>)."
    }
    $releaseVersion = $Tag.Substring(1)
}

& (Join-Path $root 'build.ps1') -WinFsp $WinFsp
$exe = Join-Path $root 'pipe-file-mount-winfsp.exe'
$reported = (& $exe --version)
if ($reported -ne "pipe-file-mount $version") { throw "The built binary reports '$reported', expected 'pipe-file-mount $version'." }
$svcExe = Join-Path $root 'PipeFileService-winfsp.exe'
$reported = (& $svcExe --version)
if ($reported -ne "PipeFileService $version") { throw "The built service reports '$reported', expected 'PipeFileService $version'." }

if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory $Out | Out-Null
$name = "pipe-file-mount-$releaseVersion-windows-x64"
$stage = Join-Path $Out $name
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $exe, $svcExe, (Join-Path $root 'LICENSE'), (Join-Path $root 'README.md') $stage
$zip = Join-Path $Out "$name.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
# LF endings and no BOM, so `sha256sum -c SHA256SUMS` works on Linux, WSL and Git Bash
# (Set-Content would write CRLF).
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS'), "$hash  $name.zip`n", (New-Object Text.UTF8Encoding($false)))

$notes = @"
pipe-file-mount $releaseVersion (experimental, Windows x64)

Exposes a Windows named pipe as a read-only file on a WinFsp volume.
``pipe-file-mount-winfsp.exe`` does it for one pipe in one process; ``PipeFileService-winfsp.exe`` is a
Windows service that does it for any local user's app and can link placeholder files to the
pipes (see the README).

**Requires WinFsp** (not included): https://github.com/winfsp/winfsp/releases

**The binary is not Authenticode-signed**, so Windows SmartScreen may warn. Check the
zip against ``SHA256SUMS``:

``````
$hash  $name.zip
``````

The files carry a signed GitHub build attestation (built by this repository's release
workflow from this tag). Verify it with the GitHub CLI:

``````
gh attestation verify $name.zip -R kihneman/pipe-file-service
``````

> WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
> https://github.com/winfsp/winfsp

This program links to the WinFsp DLL under WinFsp's FLOSS exception and is MIT
licensed; do not combine it with proprietary software.
"@
# No byte-order mark: Windows PowerShell's -Encoding utf8 would add one.
[IO.File]::WriteAllText((Join-Path $Out 'RELEASE_NOTES.md'), $notes, (New-Object Text.UTF8Encoding($false)))

Write-Host "Built $zip"
Write-Host "sha256 $hash"
