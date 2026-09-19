[CmdletBinding(SupportsShouldProcess = $true)]
param([switch]$Rebuild, [string[]]$Path = @())

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SempervirensShellRefresh {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
    [DllImport("user32.dll")]
    public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
}
'@

if ($Rebuild) {
    $local = [IO.Path]::GetFullPath([Environment]::GetFolderPath('LocalApplicationData'))
    $cacheDir = [IO.Path]::GetFullPath((Join-Path $local 'Microsoft\Windows\Explorer'))
    $files = @()
    if (Test-Path -LiteralPath $cacheDir) {
        if ((Get-Item -LiteralPath $cacheDir).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Refusing to clean a redirected Explorer cache directory.'
        }
        $files += Get-ChildItem -LiteralPath $cacheDir -File -Force | Where-Object {
            $_.Name -match '^iconcache(?:_[a-zA-Z0-9_]+)?\.db$'
        }
    }
    $legacy = Join-Path $local 'IconCache.db'
    if (Test-Path -LiteralPath $legacy) { $files += Get-Item -LiteralPath $legacy -Force }
    foreach ($file in $files) {
        $parent = [IO.Path]::GetFullPath($file.DirectoryName)
        if (($parent -ne $cacheDir -and $parent -ne $local) -or
            ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unsafe icon cache target: $($file.FullName)"
        }
    }
    if ($PSCmdlet.ShouldProcess('Current user icon cache', 'Restart desktop Explorer and rebuild icon cache')) {
        $windows = [Environment]::GetFolderPath('Windows')
        $explorerPath = Join-Path $windows 'explorer.exe'
        [uint32]$shellProcessId = 0
        [void][SempervirensShellRefresh]::GetWindowThreadProcessId([SempervirensShellRefresh]::GetShellWindow(), [ref]$shellProcessId)
        if ($shellProcessId -eq 0) { throw 'No desktop Explorer process found; no cache files were removed.' }
        $shellProcess = Get-Process -Id $shellProcessId
        if ($shellProcess.SessionId -ne (Get-Process -Id $PID).SessionId -or $shellProcess.Path -ne $explorerPath) {
            throw 'Explorer process identity check failed; no cache files were removed.'
        }
        Write-Host 'Close file copies and Explorer windows first. The desktop and taskbar will briefly disappear.'
        $removed = 0
        $locked = 0
        try {
            Stop-Process -Id $shellProcessId -Force
            [void]$shellProcess.WaitForExit(5000)
            foreach ($file in $files) {
                try { Remove-Item -LiteralPath $file.FullName -Force; $removed++ }
                catch { $locked++ }
            }
        } finally {
            if ([SempervirensShellRefresh]::GetShellWindow() -eq [IntPtr]::Zero) {
                Start-Process -FilePath $explorerPath -WindowStyle Hidden
            }
        }
        Write-Host "Removed $removed generated icon cache files. Windows will recreate them."
        if ($locked) { Write-Warning "$locked cache files were locked. Sign out and back in if stale icons remain." }
    }
}

if ($PSCmdlet.ShouldProcess('Windows Shell', 'Refresh icon display')) {
    foreach ($item in $Path) {
        if (-not (Test-Path -LiteralPath $item)) { continue }
        $pointer = [Runtime.InteropServices.Marshal]::StringToHGlobalUni((Resolve-Path -LiteralPath $item).Path)
        try { [SempervirensShellRefresh]::SHChangeNotify(0x2000, 0x2005, $pointer, [IntPtr]::Zero) }
        finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($pointer) }
    }
    [SempervirensShellRefresh]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    $refresh = Join-Path ([Environment]::GetFolderPath('System')) 'ie4uinit.exe'
    if (Test-Path -LiteralPath $refresh) {
        $process = Start-Process -FilePath $refresh -ArgumentList '-show' -WindowStyle Hidden -Wait -PassThru
        $process.Dispose()
    }
    Write-Host 'Icon refresh requested. Reopen the folder to check the result.'
}
