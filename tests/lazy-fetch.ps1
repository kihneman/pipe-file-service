# Checks that the mount tool fetches the content on the first Read of a handle, not at
# Open: programs that open the file and close it never reach the data server. Uses a
# counting fake server (count-server.ps1). Needs WinFsp installed and the mount tool
# built; use a free drive letter. Windows PowerShell 5.1 or later.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\pipe-file-mount-winfsp.exe'),
    [string]$Drive = 'K'
)
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\Program Files (x86)\WinFsp\bin;$env:PATH"
$work = Join-Path $env:TEMP "lazy-fetch-$PID"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $work | Out-Null
$script:failures = 0
$file = "${Drive}:\app.env"
function Check($name, $ok, $detail) {
    if ($ok) { "PASS  $name" } else { "FAIL  $name   $detail"; $script:failures++ }
}
function Count($log, $pattern) {
    if (-not (Test-Path $log)) { return 0 }
    @(Get-Content $log | Where-Object { $_ -match $pattern }).Count
}

Add-Type -TypeDefinition @'
using System; using System.IO; using System.Threading; using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawFile {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle h, byte[] buf, uint n, out uint got, ref NativeOverlapped o);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(SafeFileHandle h, ref NativeOverlapped o, out uint got, bool wait);
    // Opens one handle and issues two overlapped reads at once; returns the bytes each got (-1 = error).
    public static int[] TwoConcurrentReads(string path) {
        var h = CreateFile(path, 0x80000000, 7, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("open failed: " + Marshal.GetLastWin32Error());
        using (h) {
            var res = new int[2]; var t = new Thread[2];
            for (int i = 0; i < 2; i++) {
                int k = i;
                t[k] = new Thread(() => {
                    var o = new NativeOverlapped(); o.OffsetLow = k * 2;
                    var ev = new ManualResetEvent(false); o.EventHandle = ev.SafeWaitHandle.DangerousGetHandle();
                    var b = new byte[2]; uint got;
                    if (!ReadFile(h, b, 2, out got, ref o)) {
                        if (Marshal.GetLastWin32Error() != 997) { res[k] = -1; return; }
                        if (!GetOverlappedResult(h, ref o, out got, true)) { res[k] = -1; return; }
                    }
                    res[k] = (int)got;
                });
            }
            t[0].Start(); t[1].Start(); t[0].Join(); t[1].Join();
            return res;
        }
    }
    // Opens for reading, optionally reads, closes. Returns bytes read (-1 = open or read failed).
    public static int OpenRead(string path, int reads) {
        var h = CreateFile(path, 0x80000000, 7, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h.IsInvalid) return -1;
        using (h) using (var fs = new FileStream(h, FileAccess.Read, 1, false)) {
            int total = 0; var b = new byte[64];
            for (int i = 0; i < reads; i++) {
                try { total += fs.Read(b, 0, b.Length); } catch (Exception) { return -1; }
            }
            return total;
        }
    }
}
'@

$servers = @()
$mounts = @()
function Start-Server($pipe, $log, $mode, $delay) {
    $a = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'count-server.ps1'), '-Pipe', $pipe, '-Log', $log, '-Mode', $mode, '-DelayMs', $delay)
    $p = Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden
    $script:servers += $p
    Start-Sleep -Milliseconds 1500
    $p
}
function Start-Mount($pipe, $extra) {
    $a = @("\\.\pipe\$pipe", "${Drive}:", '\app.env') + $extra
    $p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden
    $script:mounts += $p
    Start-Sleep -Seconds 3
    $p
}
function Stop-All($mount, $server) {
    Stop-Process -Id $mount.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

try {
    # ---- direct mode: the mount tool reads the data pipe itself
    $pipe = "lazy-direct-$PID"; $log = Join-Path $work 'direct.log'
    $srv = Start-Server $pipe $log 'direct' 0
    $br = Start-Mount $pipe @()
    Check 'mount tool is up' (Test-Path "${Drive}:\") 'drive missing'

    $null = Get-ChildItem "${Drive}:\"; $null = Test-Path $file
    $null = [IO.File]::GetAttributes($file); $null = (New-Object IO.FileInfo $file).Length
    Check 'listing and stat cause no connection' ((Count $log 'conn') -eq 0)

    $n = [RawFile]::OpenRead($file, 0)
    Check 'open for read and close without reading causes no connection' (($n -eq 0) -and ((Count $log 'conn') -eq 0)) "n=$n conns=$(Count $log 'conn')"

    $n = [RawFile]::OpenRead($file, 3)
    Check 'a read returns the content' ($n -eq 10) "n=$n"
    Check 'a read causes exactly one connection (later reads on the handle reuse it)' ((Count $log 'conn') -eq 1) "conns=$(Count $log 'conn')"

    $text = [IO.File]::ReadAllText($file)
    Check 'ReadAllText works' ($text -eq "KEY=value`n")
    Check 'a second handle fetches again' ((Count $log 'conn') -eq 2) "conns=$(Count $log 'conn')"
    Stop-All $br $srv

    # ---- concurrent first reads on one handle share one fetch
    $log = Join-Path $work 'direct-slow.log'
    $pipe = "lazy-slow-$PID"
    $srv = Start-Server $pipe $log 'direct' 1500
    $br = Start-Mount $pipe @()
    $r = [RawFile]::TwoConcurrentReads($file)
    Check 'two concurrent first reads both succeed' (($r[0] -eq 2) -and ($r[1] -eq 2)) "got $($r -join ',')"
    Check 'two concurrent first reads cause one connection' ((Count $log 'conn') -eq 1) "conns=$(Count $log 'conn')"
    Stop-All $br $srv

    # ---- reader-info mode
    $pipe = "lazy-info-$PID"; $log = Join-Path $work 'info.log'
    $srv = Start-Server "$pipe-reader" $log 'allow' 0
    $br = Start-Mount $pipe @('--reader-info-suffix', '-reader')
    $null = Get-ChildItem "${Drive}:\"; $null = [IO.File]::GetAttributes($file)
    $n = [RawFile]::OpenRead($file, 0)
    Check 'reader-info: open and close without reading sends no request' (($n -eq 0) -and ((Count $log 'req') -eq 0)) "n=$n reqs=$(Count $log 'req')"

    $reader = Join-Path $work 'reader.ps1'
    "[IO.File]::ReadAllText('$file') | Out-Null" | Set-Content $reader
    $p1 = Start-Process powershell -ArgumentList '-NoProfile', '-File', $reader -PassThru -WindowStyle Hidden; $p1.WaitForExit()
    $p2 = Start-Process powershell -ArgumentList '-NoProfile', '-File', $reader -PassThru -WindowStyle Hidden; $p2.WaitForExit()
    $reqs = @(Get-Content $log | Where-Object { $_ -match 'req' })
    Check 'reader-info: two readers cause two requests' ($reqs.Count -eq 2) "reqs=$($reqs -join ';')"
    Check 'reader-info: the requests name the right readers' (($reqs[0] -eq "req pid=$($p1.Id)") -and ($reqs[1] -eq "req pid=$($p2.Id)")) "reqs=$($reqs -join ';') pids=$($p1.Id),$($p2.Id)"
    Stop-All $br $srv

    # ---- denied reader: first Read fails, later reads do not ask again
    $log = Join-Path $work 'deny.log'
    $srv = Start-Server "$pipe-reader" $log 'deny' 0
    $br = Start-Mount $pipe @('--reader-info-suffix', '-reader')
    $n = [RawFile]::OpenRead($file, 3)
    Check 'denied reader: the read fails' ($n -eq -1) "n=$n"
    Check 'denied reader: repeated reads on one handle ask the server once' ((Count $log 'req') -eq 1) "reqs=$(Count $log 'req')"
    Stop-All $br $srv

    # ---- server down: the first Read fails cleanly
    $br = Start-Mount $pipe @('--reader-info-suffix', '-reader')
    $n = [RawFile]::OpenRead($file, 1)
    Check 'server down: open succeeds but the first read fails cleanly' ($n -eq -1) "n=$n"
    Check 'server down: the mount tool is still running' (-not $br.HasExited)
    Stop-Process -Id $br.Id -Force -ErrorAction SilentlyContinue
}
finally {
    foreach ($s in $servers) { Stop-Process -Id $s.Id -Force -ErrorAction SilentlyContinue }
    foreach ($b in $mounts) { Stop-Process -Id $b.Id -Force -ErrorAction SilentlyContinue }   # only the ones started here
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failures) { "$script:failures check(s) failed"; exit 1 }
'all checks passed'
exit 0   # explicit: otherwise a leftover $LASTEXITCODE from a native command becomes the exit code
