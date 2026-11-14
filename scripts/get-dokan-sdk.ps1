# Fetches what build.ps1 needs to build the Dokan backend, without installing the driver: the three
# public headers (from the pinned release tag) and the x64 import library dokan2.lib (from the release's
# dokan.zip). Every download is checked against a pinned SHA-256. Prints the folder to pass to
# build.ps1 -Dokan; it holds inc\ (dokan.h, fileinfo.h, public.h) and lib\dokan2.lib.
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\.dokan-sdk'),
    [string]$Tag = 'v2.3.1.1000'
)
$ErrorActionPreference = 'Stop'
$Dest = [IO.Path]::GetFullPath($Dest)
$zipSha = 'cbfb89718d4e671f8dd65187912c1141fde0e5197f40ac2979b3216753d42c7c'   # dokan.zip, as published for the release asset
$headers = @(
    @{ Path = 'dokan/dokan.h';    Sha = '986dad86d25a422089f430d7976d914a3c8babb04e0c5bd041e9544c26f8322e' },
    @{ Path = 'dokan/fileinfo.h'; Sha = 'c463ca2e97804865ddef3397d30a9d36e74853cef47dae0e123ab47d2471b7a1' },
    @{ Path = 'sys/public.h';     Sha = 'd8485c7600947a396500a0d847ccf44d65ca831e3c63c1f92c724da36bf90895' }
)
if ($Tag -ne 'v2.3.1.1000') { throw 'The pinned hashes are for v2.3.1.1000; update them together with the tag.' }

# Download into its own new directory and never run anything from it.
$work = Join-Path ([IO.Path]::GetTempPath()) ("dokan-sdk-" + [guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null
try {
    $ProgressPreference = 'SilentlyContinue'
    if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
    New-Item -ItemType Directory "$Dest\inc", "$Dest\lib" | Out-Null

    foreach ($h in $headers) {
        $name = Split-Path $h.Path -Leaf
        $file = Join-Path $work $name
        Invoke-WebRequest "https://raw.githubusercontent.com/dokan-dev/dokany/$Tag/$($h.Path)" -OutFile $file
        $actual = (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $h.Sha) { throw "Checksum mismatch for $($h.Path): expected $($h.Sha), got $actual" }
        Copy-Item $file "$Dest\inc\$name"
    }

    $zip = Join-Path $work 'dokan.zip'
    Invoke-WebRequest "https://github.com/dokan-dev/dokany/releases/download/$Tag/dokan.zip" -OutFile $zip
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $zipSha) { throw "Checksum mismatch for dokan.zip: expected $zipSha, got $actual" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $za = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $za.GetEntry('x64/Release/dokan2.lib')
        if (-not $entry) { throw 'x64/Release/dokan2.lib not found in dokan.zip' }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$Dest\lib\dokan2.lib")
    }
    finally { $za.Dispose() }
}
finally { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
$Dest
