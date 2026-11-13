# Checks that the test scripts report success through their exit code, not only through the text they print.
#
# A PowerShell script that ends without an explicit `exit` leaves $LASTEXITCODE at whatever the last native
# command inside it returned. When the script is run the way CI runs a step (`& .\tests\x.ps1`, not
# `powershell -File`), that leftover becomes the exit code, so a passing run can be reported as a failure.
#
#   1. every test script ends with an explicit `exit` (needs nothing installed);
#   2. driver-missing.ps1, which deliberately runs a command that exits with 2, still exits with 0 when it passes
#      (needs a built FspPipeFileService.exe; skipped without it).
$ErrorActionPreference = 'Stop'
$script:failures = 0
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}

foreach ($name in 'driver-missing', 'limits', 'smoke', 'lazy-fetch', 'service-smoke', 'install-chain') {
    $lines = @(Get-Content (Join-Path $PSScriptRoot "$name.ps1") | Where-Object { $_.Trim() })
    Check "$name.ps1 ends with an explicit exit" ($lines[-1] -match '^\s*exit\b') $lines[-1]
}

$exe = Join-Path $PSScriptRoot '..\FspPipeFileService.exe'
if (-not (Test-Path $exe)) {
    'SKIP  driver-missing.ps1 exit code: build FspPipeFileService.exe first'
} else {
    $script = Join-Path $PSScriptRoot 'driver-missing.ps1'
    # The same way a CI step runs a script: call it, then exit with the last native exit code.
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& '$script' | Out-Null; exit `$LASTEXITCODE"
    Check 'driver-missing.ps1 exits with 0 when it passes (run with & and -Command)' ($LASTEXITCODE -eq 0) "exit code $LASTEXITCODE"
}

if ($script:failures) { "$($script:failures) FAILED"; exit 1 } else { 'ALL PASSED' }
exit 0
