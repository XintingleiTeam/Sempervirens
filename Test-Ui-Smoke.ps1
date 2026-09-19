$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$executable = [System.IO.Path]::GetFullPath((Join-Path $project 'build\test\Sempervirens.exe'))
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'The native test executable is missing.'
}

# Keep the production mutex present for the whole suite. Automated modes must
# use their own mutex and window class, otherwise a running installed app
# can make every child exit successfully without exercising any UI behavior.
$productionMutexWasCreated = $false
$productionMutex = [System.Threading.Mutex]::new(
    $false, 'Local\SempervirensSingleInstance', [ref]$productionMutexWasCreated)

$arguments = @(
    '--smoke',
    '--smoke-single-instance',
    '--smoke-tray-lifecycle',
    '--smoke-search-input',
    '--smoke-group-toggle',
    '--smoke-progress',
    '--smoke-cancel-scan',
    '--smoke-close-during-scan',
    '--smoke-instance-choice',
    '--smoke-instance-card',
    '--smoke-pointer-feedback',
    '--smoke-drawer-scrollbar',
    '--smoke-session-end',
    '--smoke-session-end-destroy',
    '--smoke-dpi-change',
    '--smoke-content-scrollbars',
    '--smoke-migration-close',
    '--smoke-migration-error',
    '--smoke-real-migration-error',
    '--smoke-real-migration-success',
    '--smoke-real-migration-success-en',
    '--smoke-real-migration-keyboard',
    '--smoke-result-state',
    '--smoke-result-open',
    '--smoke-project-link',
    '--smoke-profile-rescan',
    '--smoke-accessibility',
    '--smoke-accessibility-pages',
    '--smoke-splash',
    '--smoke-splash-skip',
    '--smoke-resize',
    '--smoke-drop',
    '--smoke-drop-target',
    '--smoke-drop-multiple',
    '--capture=build\ui-main.png',
    '--capture-scan-progress=build\ui-scan-progress.png',
    '--capture-scan-indeterminate=build\ui-scan-indeterminate.png',
    '--capture-search-edit=build\ui-search-edit.png',
    '--capture-drawer=build\ui-instance-drawer.png',
    '--capture-close=build\ui-close-dialog.png',
    '--capture-settings=build\ui-settings.png',
    '--capture-settings-migration=build\ui-settings-migration.png',
    '--capture-settings-about=build\ui-settings-about.png',
    '--capture-populated=build\ui-populated.png',
    '--capture-min=build\ui-minimum.png',
    '--capture-language=build\ui-language.png',
    '--capture-settings-min=build\ui-settings-minimum.png',
    '--capture-min-en=build\ui-minimum-en.png',
    '--capture-populated-min-en=build\ui-populated-minimum-en.png',
    '--capture-settings-min-en=build\ui-settings-minimum-en.png',
    '--capture-splash=build\ui-splash-mid.png',
    '--capture-splash-start=build\ui-splash-start.png',
    '--capture-splash-orbit=build\ui-splash-orbit.png',
    '--capture-splash-arrow=build\ui-splash-before-arrow.png',
    '--capture-splash-benchmark=build\ui-splash-benchmark.png',
    '--capture-splash-end=build\ui-splash-end.png',
    '--capture-splash-en=build\ui-splash-en.png',
    '--capture-welcome=build\ui-welcome.png',
    '--capture-error=build\ui-error.png',
    '--capture-error-min=build\ui-error-minimum.png',
    '--capture-error-en=build\ui-error-minimum-en.png',
    '--capture-confirm=build\ui-confirm.png',
    '--capture-confirm-min=build\ui-confirm-minimum.png',
    '--capture-confirm-en=build\ui-confirm-en.png',
    '--capture-confirm-min-en=build\ui-confirm-minimum-en.png',
    '--capture-result=build\ui-result.png',
    '--capture-result-failed=build\ui-result-failed.png',
    '--capture-result-failed-min=build\ui-result-failed-minimum.png',
    '--capture-result-failed-en=build\ui-result-failed-en.png',
    '--capture-post-result=build\ui-post-result.png',
    '--capture-post-result-min=build\ui-post-result-minimum.png'
)
if ([Environment]::OSVersion.Version.Build -ge 22621) {
    $arguments += '--smoke-material-required'
} else {
    $arguments += '--smoke-material'
}

function Invoke-UiTest([string]$argument) {
    $started = [DateTime]::UtcNow
    $process = Start-Process -FilePath $executable -ArgumentList $argument `
        -WorkingDirectory $project -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(20000)) {
            $running = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
            if ($running -and [string]::Equals($running.Path, $executable,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $process.Id -Force
            }
            throw "Native UI test timed out: $argument"
        }
        if ($process.ExitCode -ne 0) {
            throw "Native UI test failed with exit code $($process.ExitCode): $argument"
        }
        if ($argument.StartsWith('--capture')) {
            $relative = $argument.Substring($argument.IndexOf('=') + 1)
            $image = Join-Path $project $relative
            $file = Get-Item -LiteralPath $image -ErrorAction Stop
            if ($file.Length -lt 1024 -or $file.LastWriteTimeUtc -lt $started.AddSeconds(-2)) {
                throw "Native UI capture was not updated: $argument"
            }
        }
    } finally {
        $process.Dispose()
    }
}

foreach ($argument in $arguments) { Invoke-UiTest $argument }

& (Join-Path $project 'tools\Test-Startup-Frames.ps1')

$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $project 'build'))
$settingsRoot = [System.IO.Path]::GetFullPath((Join-Path $buildRoot ('settings-smoke-' + [guid]::NewGuid().ToString('N'))))
if (-not $settingsRoot.StartsWith($buildRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) -or
    -not [System.IO.Path]::GetFileName($settingsRoot).StartsWith('settings-smoke-', [System.StringComparison]::Ordinal)) {
    throw 'The settings smoke fixture is outside the project build directory.'
}
New-Item -ItemType Directory -Path $settingsRoot | Out-Null
try {
    $settingsFile = Join-Path $settingsRoot 'settings.json'
    Invoke-UiTest "--smoke-settings-write=$settingsFile"
    Invoke-UiTest "--smoke-settings-read=$settingsFile"
    $savedSettings = [System.IO.File]::ReadAllText($settingsFile)
    if (-not $savedSettings.Contains('custom-profile.json')) {
        throw 'The settings fixture is missing its custom rule path.'
    }
    [System.IO.File]::WriteAllText($settingsFile,
        $savedSettings.Replace('custom-profile.json', 'missing-profile.json'),
        [System.Text.UTF8Encoding]::new($false))
    Invoke-UiTest "--smoke-settings-invalid=$settingsFile"
} finally {
    if (Test-Path -LiteralPath $settingsRoot) {
        $resolved = (Resolve-Path -LiteralPath $settingsRoot).Path
        $folder = Get-Item -LiteralPath $resolved -Force
        if (-not $resolved.StartsWith($buildRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) -or
            -not $folder.Name.StartsWith('settings-smoke-', [System.StringComparison]::Ordinal) -or
            ($folder.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            throw 'The settings smoke cleanup target failed path validation.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

Write-Output "Native UI smoke tests passed: $($arguments.Count + 3)"
