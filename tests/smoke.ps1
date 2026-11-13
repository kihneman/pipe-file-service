# Smoke test: serves a named pipe from this script, mounts it with the bridge
# and checks reads, nested paths, rejected paths and unmount. Needs WinFsp
# installed and winfsp-pipe-bridge.exe built (see ..\build.ps1). Windows
# PowerShell 5.1 or later; use a free drive letter.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\winfsp-pipe-bridge.exe'),
    [string]$Drive = 'K'
)
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\Program Files (x86)\WinFsp\bin;$env:PATH"
$pipeName = "winfsp-pipe-bridge-smoke-$PID"
$content = "# smoke`nKEY=value`nQUOTED=`"a \`"b\`" c`"`n"
$script:failures = 0

function Check($name, $ok) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name"; $script:failures++ }
}

# One-reader-at-a-time pipe server that answers every connection with $content.
$server = Start-Job -ArgumentList $pipeName, $content -ScriptBlock {
    param($name, $text)
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    while ($true) {
        $s = New-Object IO.Pipes.NamedPipeServerStream($name, 'Out', 4, 'Byte', 'None')
        try { $s.WaitForConnection(); $s.Write($bytes, 0, $bytes.Length); $s.Flush(); $s.WaitForPipeDrain() }
        catch { } finally { $s.Dispose() }
    }
}
Start-Sleep -Seconds 1

function Start-Bridge($path) {
    $a = @("\\.\pipe\$pipeName", "${Drive}:"); if ($path) { $a += $path }
    $p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    Remove-PSDrive -Name $Drive -Force -ErrorAction SilentlyContinue
    New-PSDrive -Name $Drive -PSProvider FileSystem -Root "${Drive}:\" -Scope Global -ErrorAction SilentlyContinue | Out-Null
    $p
}
function Stop-Bridge($p) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1
    Remove-PSDrive -Name $Drive -Force -ErrorAction SilentlyContinue
}
function Read-All($path) { [IO.File]::ReadAllText($path) }

try {
    foreach ($bad in '\a\..\b.env', '\a\\b.env', '\a\b.env\', '\a:b\c.env', '\a\b?.env') {
        $p = Start-Process -FilePath $Exe -ArgumentList "\\.\pipe\$pipeName", "${Drive}:", $bad -PassThru -Wait -WindowStyle Hidden
        Check "rejects path $bad" ($p.ExitCode -eq 2)
    }

    $p = Start-Bridge $null
    Check 'default path is readable' ((Read-All "${Drive}:\property-file.env") -eq $content)
    Stop-Bridge $p
    Check 'drive disappears after stop' (-not (Test-Path "${Drive}:\"))

    $p = Start-Bridge '\secrets\app\prod.env'
    Check 'root lists the first directory' ((Get-ChildItem "${Drive}:\").Name -eq 'secrets')
    Check 'directory lists the next component' ((Get-ChildItem "${Drive}:\secrets").Name -eq 'app')
    Check 'file is listed in its directory' ((Get-ChildItem "${Drive}:\secrets\app").Name -eq 'prod.env')
    Check 'nested file content' ((Read-All "${Drive}:\secrets\app\prod.env") -eq $content)
    Check 'lookup is case-insensitive' ((Read-All "${Drive}:\SECRETS\App\PROD.ENV") -eq $content)
    Check 'other names do not exist' (-not (Test-Path "${Drive}:\secrets\app\other.env"))
    Check 'default name does not exist' (-not (Test-Path "${Drive}:\property-file.env"))
    $wrote = $true
    try { Set-Content "${Drive}:\secrets\app\new.txt" 'x' -ErrorAction Stop } catch { $wrote = $false }
    Check 'writes are refused' (-not $wrote)
    Stop-Bridge $p

    $p = Start-Bridge 'one/two/f.env'
    Check 'forward slashes, no leading slash' ((Read-All "${Drive}:\one\two\f.env") -eq $content)
    Stop-Bridge $p
}
finally {
    Stop-Job $server -ErrorAction SilentlyContinue; Remove-Job $server -Force -ErrorAction SilentlyContinue
    Get-Process winfsp-pipe-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
