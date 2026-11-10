# Fetches the WinFsp SDK (headers and import library) without installing the
# driver: downloads a pinned, checksum-verified release MSI and extracts it
# with an administrative install. Prints the folder to pass to build.ps1 -WinFsp.
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\.winfsp-sdk'),
    [string]$Tag = 'v2.1',
    [string]$MsiName = 'winfsp-2.1.25156.msi',
    # sha256 as published for the release asset on GitHub.
    [string]$Sha256 = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a'
)
$ErrorActionPreference = 'Stop'
$Dest = [IO.Path]::GetFullPath($Dest)

# Download into its own new directory and never run anything from it.
$work = Join-Path ([IO.Path]::GetTempPath()) ("winfsp-sdk-" + [guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
try {
    $msi = Join-Path $work $MsiName
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest "https://github.com/winfsp/winfsp/releases/download/$Tag/$MsiName" -OutFile $msi
    $actual = (Get-FileHash $msi -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) { throw "Checksum mismatch for ${MsiName}: expected $Sha256, got $actual" }

    if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
    New-Item -ItemType Directory $Dest | Out-Null
    $p = Start-Process msiexec.exe -ArgumentList '/a', "`"$msi`"", '/qn', "TARGETDIR=`"$Dest`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec administrative install failed with exit code $($p.ExitCode)" }
}
finally { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }

$header = Get-ChildItem $Dest -Recurse -Filter winfsp.h | Where-Object { $_.Directory.Name -eq 'winfsp' } | Select-Object -First 1
if (-not $header) { throw "winfsp.h not found under $Dest" }
$root = $header.Directory.Parent.Parent.FullName
if (-not (Test-Path "$root\lib\winfsp-x64.lib")) { throw "winfsp-x64.lib not found under $root\lib" }
$root
