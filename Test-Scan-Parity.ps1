param(
    [string]$OriginalDll,
    [string]$OriginalProject
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$buildRoot = Join-Path $projectRoot 'build'
$nativeExe = Join-Path $buildRoot 'Sempervirens-inspect.exe'
$nativeMigrationExe = Join-Path $buildRoot 'parity-migrate.exe'
$referenceProject = Join-Path $projectRoot 'tests\parity-reference\ParityReference.csproj'
$referenceExe = Join-Path $projectRoot 'tests\parity-reference\bin\Release\net8.0-windows\ParityReference.dll'
if (-not $OriginalDll) {
    if (-not $OriginalProject) {
        $OriginalProject = Join-Path (Split-Path -Parent $projectRoot) 'Sempervirens\dotnet\Sempervirens.csproj'
    }
    if (-not (Test-Path -LiteralPath $OriginalProject -PathType Leaf)) {
        throw "Original .NET project not found: $OriginalProject"
    }
    & dotnet build $OriginalProject -c Release --verbosity quiet
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET build failed' }
    $OriginalDll = Join-Path (Split-Path -Parent $OriginalProject) 'bin\Release\net8.0-windows\win-x64\Sempervirens.dll'
}
if (-not (Test-Path -LiteralPath $OriginalDll -PathType Leaf)) {
    throw "Original .NET assembly not found: $OriginalDll"
}

& (Join-Path $projectRoot 'Build.cmd')
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed' }
& dotnet build $referenceProject -c Release "-p:ReferenceDll=$OriginalDll" --verbosity quiet
if ($LASTEXITCODE -ne 0) { throw '.NET reference adapter build failed' }

$fixture = Join-Path $buildRoot ('scan-parity-' + [guid]::NewGuid().ToString('N'))
$utf8 = [System.Text.UTF8Encoding]::new($false)
function Get-RelativeFixturePath([string]$basePath, [string]$itemPath) {
    $fullBase = [System.IO.Path]::GetFullPath($basePath)
    if (-not $fullBase.EndsWith('\')) { $fullBase += '\' }
    $fullItem = [System.IO.Path]::GetFullPath($itemPath)
    if (-not $fullItem.StartsWith($fullBase, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The fixture item is outside its expected directory: $itemPath"
    }
    return $fullItem.Substring($fullBase.Length)
}
function Write-Fixture([string]$relativePath, [string]$content) {
    $path = Join-Path $fixture $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [System.IO.File]::WriteAllText($path, $content, $utf8)
}
function ConvertTo-WindowsCommandLineArgument([string]$argument) {
    if ($argument.Length -gt 0 -and $argument -notmatch '[\s"]') { return $argument }
    $quoted = [System.Text.StringBuilder]::new()
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($character in $argument.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$quoted.Append(('\' * ($slashes * 2 + 1)))
            [void]$quoted.Append('"')
        } else {
            if ($slashes -gt 0) { [void]$quoted.Append(('\' * $slashes)) }
            [void]$quoted.Append($character)
        }
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$quoted.Append(('\' * ($slashes * 2))) }
    [void]$quoted.Append('"')
    return $quoted.ToString()
}
function Invoke-QuietProcess([string]$fileName, [string[]]$arguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $fileName
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = (($arguments | ForEach-Object {
        ConvertTo-WindowsCommandLineArgument $_
    }) -join ' ')
    $process = [System.Diagnostics.Process]::Start($start)
    $standardOutput = $process.StandardOutput.ReadToEnd()
    $standardError = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        StandardOutput = $standardOutput
        StandardError = $standardError
    }
}
function Compare-Profile([string]$name, [string]$content, [byte[]]$bytes = $null) {
    $relative = "profile-parity-$name.json"
    Write-Fixture $relative $content
    $path = Join-Path $fixture $relative
    if ($null -ne $bytes) { [System.IO.File]::WriteAllBytes($path, $bytes) }
    $referenceJson = & dotnet $referenceExe --profile $path
    if ($LASTEXITCODE -ne 0) { throw "Original .NET profile adapter failed: $name" }
    $nativeJson = & $nativeExe --profile-json $path
    if ($LASTEXITCODE -ne 0) { throw "C++ profile adapter failed: $name" }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    if ($reference.valid -ne $native.valid) {
        throw "Profile validity differs for ${name}: .NET=$($reference.valid), C++=$($native.valid)"
    }
    if (-not $reference.valid) {
        Write-Output "Profile ${name}: both loaders reject it."
        return
    }
    foreach ($field in @('schemaVersion', 'name', 'description')) {
        if ($reference.$field -cne $native.$field) {
            throw "Profile $name field '$field' differs: .NET='$($reference.$field)', C++='$($native.$field)'"
        }
    }
    $referenceRules = @($reference.rules)
    $nativeRules = @($native.rules)
    if ($referenceRules.Count -ne $nativeRules.Count) { throw "Profile rule count differs for $name" }
    for ($index = 0; $index -lt $referenceRules.Count; $index++) {
        foreach ($field in @('id', 'group', 'name', 'description', 'kind', 'builtin',
                'category', 'source', 'target', 'recursive', 'defaultSelected', 'risk')) {
            if ($referenceRules[$index].$field -cne $nativeRules[$index].$field) {
                throw "Profile $name rule $index '$field' differs: .NET='$($referenceRules[$index].$field)', C++='$($nativeRules[$index].$field)'"
            }
        }
    }
    Write-Output "Profile ${name}: $($referenceRules.Count) matching rule(s)."
}
function Compare-DefaultProfile([string]$name, [string]$relativeDirectory) {
    $directory = Join-Path $fixture $relativeDirectory
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $referenceJson = & dotnet $referenceExe --default-profile $directory
    if ($LASTEXITCODE -ne 0) { throw "Original .NET default profile adapter failed: $name" }
    $nativeJson = & $nativeExe --default-profile-json $directory
    if ($LASTEXITCODE -ne 0) { throw "C++ default profile adapter failed: $name" }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    if ($reference.valid -ne $native.valid -or
        ($reference.valid -and ($reference.name -cne $native.name -or
            $reference.ruleCount -ne $native.ruleCount))) {
        throw "Default profile selection differs for ${name}: .NET=$referenceJson C++=$nativeJson"
    }
    Write-Output "Default profile ${name}: matching validity and selected rule set."
}
function Compare-ConfigFields([string]$name, [string]$extension, [string]$sourceText, [string]$targetText,
    [byte[]]$sourceBytes = $null, [byte[]]$targetBytes = $null) {
    $relative = "config-fields-$name"
    $profileRelative = "$relative\profile.json"
    $fileRelative = "config\settings.$extension"
    $profileText = @{
        schemaVersion = 1
        name = "Config fields $name"
        rules = @(@{
            id = 'config'
            name = 'Config'
            group = 'Test'
            kind = 'file'
            source = $fileRelative.Replace('\', '/')
            target = $fileRelative.Replace('\', '/')
            defaultSelected = $true
        })
    } | ConvertTo-Json -Depth 6 -Compress
    Write-Fixture $profileRelative $profileText
    Write-Fixture "$relative\source\$fileRelative" $sourceText
    Write-Fixture "$relative\target\$fileRelative" $targetText
    if ($null -ne $sourceBytes) {
        [System.IO.File]::WriteAllBytes((Join-Path $fixture "$relative\source\$fileRelative"), $sourceBytes)
    }
    if ($null -ne $targetBytes) {
        [System.IO.File]::WriteAllBytes((Join-Path $fixture "$relative\target\$fileRelative"), $targetBytes)
    }
    $profilePath = Join-Path $fixture $profileRelative
    $sourcePath = Join-Path $fixture "$relative\source"
    $targetPath = Join-Path $fixture "$relative\target"
    $referenceJson = & dotnet $referenceExe $profilePath $sourcePath $targetPath
    if ($LASTEXITCODE -ne 0) { throw "Original .NET config scan failed: $name" }
    $nativeJson = & $nativeExe --plan-json $profilePath $sourcePath $targetPath
    if ($LASTEXITCODE -ne 0) { throw "C++ config scan failed: $name" }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    $referenceItems = @($reference.operations)
    $nativeItems = @($native.operations)
    if ($referenceItems.Count -ne 1 -or $nativeItems.Count -ne 1 -or
        $referenceItems[0].status -cne $nativeItems[0].status) {
        throw "Config scan status differs for $name"
    }
    if ($referenceItems[0].difference -cne $nativeItems[0].difference) {
        throw "Config explanation differs for ${name}:`n.NET: $($referenceItems[0].difference)`nC++: $($nativeItems[0].difference)"
    }
    $referenceFields = $referenceItems[0].settings
    $nativeFields = $nativeItems[0].settings
    if (($null -eq $referenceFields) -ne ($null -eq $nativeFields)) {
        throw "Config field availability differs for $name"
    }
    if ($null -eq $referenceFields) { $referenceChanges = @() }
    else { $referenceChanges = @($referenceFields) }
    if ($null -eq $nativeFields) { $nativeChanges = @() }
    else { $nativeChanges = @($nativeFields) }
    if ($referenceChanges.Count -ne $nativeChanges.Count) {
        throw "Config field count differs for ${name}: .NET=$($referenceChanges.Count), C++=$($nativeChanges.Count)"
    }
    for ($index = 0; $index -lt $referenceChanges.Count; $index++) {
        foreach ($field in @('key', 'target', 'source')) {
            if ($referenceChanges[$index].$field -cne $nativeChanges[$index].$field) {
                throw "Config $name field $index '$field' differs: .NET='$($referenceChanges[$index].$field)', C++='$($nativeChanges[$index].$field)'"
            }
        }
    }
    Write-Output "Config fields ${name}: $($referenceChanges.Count) matching change(s)."
}
function Compare-Discovery([string]$name, [string]$path) {
    $referenceJson = & dotnet $referenceExe --discover $path
    if ($LASTEXITCODE -ne 0) { throw "Original .NET discovery failed: $name" }
    $nativeJson = & $nativeExe --discover-json $path
    if ($LASTEXITCODE -ne 0) { throw "C++ discovery failed: $name" }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    $referenceItems = @($reference.candidates)
    $nativeItems = @($native.candidates)
    if ($referenceItems.Count -ne $nativeItems.Count) {
        throw "Discovery candidate count differs for ${name}: .NET=$($referenceItems.Count), C++=$($nativeItems.Count)"
    }
    for ($index = 0; $index -lt $referenceItems.Count; $index++) {
        foreach ($field in @('name', 'root', 'isolated', 'markers', 'usable', 'options', 'servers', 'screenshots', 'worlds', 'note')) {
            if ($referenceItems[$index].$field -cne $nativeItems[$index].$field) {
                throw "Discovery $name candidate $index field '$field' differs: .NET='$($referenceItems[$index].$field)', C++='$($nativeItems[$index].$field)'"
            }
        }
    }
    Write-Output "Instance discovery ${name}: $($referenceItems.Count) matching candidate(s)."
}
function Compare-GlobPattern([string]$name, [string]$pattern, [bool]$unsafeReferenceTarget = $false) {
    $profileRelative = "glob-$name.json"
    $profileContent = @{
        schemaVersion = 1
        name = "Glob parity $name"
        rules = @(@{
            id = 'glob'
            name = 'Matched files'
            group = 'Test'
            kind = 'glob'
            source = $pattern
            target = 'copied'
            defaultSelected = $true
        })
    } | ConvertTo-Json -Depth 6 -Compress
    Write-Fixture $profileRelative $profileContent
    $profilePath = Join-Path $fixture $profileRelative
    $sourcePath = Join-Path $fixture 'glob-source'
    $targetPath = Join-Path $fixture 'glob-target'
    $referenceJson = & dotnet $referenceExe $profilePath $sourcePath $targetPath
    if ($LASTEXITCODE -ne 0) { throw "Original .NET glob scan failed: $name" }
    $nativeJson = & $nativeExe --plan-json $profilePath $sourcePath $targetPath
    if ($LASTEXITCODE -ne 0) { throw "C++ glob scan failed: $name" }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    if ($reference.valid -ne $native.valid -or $reference.error -cne $native.error) {
        throw "Glob scan validity differs: $name"
    }
    $referenceItems = @($reference.operations | Sort-Object source, target)
    $nativeItems = @($native.operations | Sort-Object source, target)
    if ($referenceItems.Count -ne $nativeItems.Count) {
        throw "Glob match count differs for ${name}: .NET=$($referenceItems.Count), C++=$($nativeItems.Count)"
    }
    for ($index = 0; $index -lt $referenceItems.Count; $index++) {
        foreach ($field in @('status', 'selected', 'source', 'target', 'size')) {
            if ($referenceItems[$index].$field -cne $nativeItems[$index].$field) {
                if ($unsafeReferenceTarget -and $field -eq 'target' -and
                    $referenceItems[$index].target.StartsWith('../', [System.StringComparison]::Ordinal) -and
                    $nativeItems[$index].target -ceq 'copied/v1/file.dat') { continue }
                throw "Glob $name item $index '$field' differs: .NET='$($referenceItems[$index].$field)', C++='$($nativeItems[$index].$field)'"
            }
        }
    }
    if ($unsafeReferenceTarget) {
        if ($referenceItems.Count -ne 1 -or $nativeItems.Count -ne 1 -or
            -not $referenceItems[0].target.StartsWith('../', [System.StringComparison]::Ordinal) -or
            $nativeItems[0].target -cne 'copied/v1/file.dat') {
            throw 'Wildcard-directory safety fixture no longer has the expected path relationship'
        }
        $safeTarget = Join-Path $fixture 'glob-safe-target'
        New-Item -ItemType Directory -Path $safeTarget -Force | Out-Null
        $resultJson = & $nativeMigrationExe replace $profilePath $sourcePath $safeTarget
        if ($LASTEXITCODE -ne 0) { throw 'C++ wildcard-directory safe migration failed' }
        $result = $resultJson | ConvertFrom-Json
        $output = Join-Path $safeTarget 'copied\v1\file.dat'
        if ($result.failed -ne 0 -or $result.copied -ne 1 -or
            -not (Test-Path -LiteralPath $output -PathType Leaf) -or
            [System.IO.File]::ReadAllText($output) -cne 'wildcard directory') {
            throw 'C++ wildcard-directory migration did not preserve the matched relative hierarchy'
        }
        Write-Output 'Wildcard-directory safety: .NET scan escapes the target; C++ writes only inside the isolated destination.'
        return
    }
    if ($reference.valid) {
        $referenceTarget = Join-Path $fixture "glob-migrate-reference-$name"
        $nativeTarget = Join-Path $fixture "glob-migrate-native-$name"
        New-Item -ItemType Directory -Path $referenceTarget | Out-Null
        New-Item -ItemType Directory -Path $nativeTarget | Out-Null
        $referenceResultJson = & dotnet $referenceExe --execute replace $profilePath $sourcePath $referenceTarget
        if ($LASTEXITCODE -ne 0) { throw "Original .NET glob migration failed: $name" }
        $nativeResultJson = & $nativeMigrationExe replace $profilePath $sourcePath $nativeTarget
        if ($LASTEXITCODE -ne 0) { throw "C++ glob migration failed: $name" }
        $referenceResult = $referenceResultJson | ConvertFrom-Json
        $nativeResult = $nativeResultJson | ConvertFrom-Json
        foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
            if ($referenceResult.$field -ne $nativeResult.$field) {
                throw "Glob $name migration '$field' differs: .NET='$($referenceResult.$field)', C++='$($nativeResult.$field)'"
            }
        }
        $referenceResults = @($referenceResult.items | Sort-Object target)
        $nativeResults = @($nativeResult.items | Sort-Object target)
        if ($referenceResults.Count -ne $nativeResults.Count) {
            throw "Glob $name migration item count differs"
        }
        for ($index = 0; $index -lt $referenceResults.Count; $index++) {
            foreach ($field in @('target', 'status', 'copied', 'overwritten')) {
                if ($referenceResults[$index].$field -cne $nativeResults[$index].$field) {
                    throw "Glob $name migration item $index '$field' differs"
                }
            }
            $relative = $referenceResults[$index].target.Replace('/', '\')
            if ([System.IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)') {
                throw "Glob $name produced an unsafe destination path"
            }
            $referenceFile = Join-Path $referenceTarget $relative
            $nativeFile = Join-Path $nativeTarget $relative
            if (-not (Test-Path -LiteralPath $referenceFile -PathType Leaf) -or
                -not (Test-Path -LiteralPath $nativeFile -PathType Leaf) -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $referenceFile).Hash -cne
                (Get-FileHash -Algorithm SHA256 -LiteralPath $nativeFile).Hash) {
                throw "Glob $name copied file differs: $relative"
            }
        }
    }
    Write-Output ("Glob pattern ${name}: $($referenceItems.Count) scan item(s); " +
        $(if ($reference.valid) { 'migration outputs agree.' } else { 'no transferable match.' }))
}

try {
    $pathSafetyProfile = Join-Path $projectRoot 'profiles\vanilla.json'
    $pathSafetyRoot = Join-Path $fixture 'path-safety-instance'
    $pathSafetyChild = Join-Path $pathSafetyRoot 'child-instance'
    New-Item -ItemType Directory -Path $pathSafetyChild -Force | Out-Null
    foreach ($case in @(
        @{ Name = 'same instance'; Source = $pathSafetyRoot; Target = $pathSafetyRoot },
        @{ Name = 'destination inside source'; Source = $pathSafetyRoot; Target = $pathSafetyChild },
        @{ Name = 'source inside destination'; Source = $pathSafetyChild; Target = $pathSafetyRoot }
    )) {
        $referenceFailure = Invoke-QuietProcess 'dotnet' @(
            $referenceExe, $pathSafetyProfile, $case.Source, $case.Target)
        $nativeFailure = Invoke-QuietProcess $nativeExe @(
            '--plan-json', $pathSafetyProfile, $case.Source, $case.Target)
        if ($referenceFailure.ExitCode -eq 0 -or $nativeFailure.ExitCode -eq 0) {
            throw "Unsafe path relationship was not rejected for $($case.Name): .NET=$($referenceFailure.ExitCode), C++=$($nativeFailure.ExitCode)"
        }
    }
    Write-Output 'Path safety: original .NET and C++ both reject same and nested instances.'

    Write-Fixture 'default-order-nested\profiles\vanilla.json' '{"name":"Nested rules","rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Write-Fixture 'default-order-nested\vanilla.json' '{"name":"Adjacent rules","rules":[{"id":"two","name":"Two","source":"a","target":"b"}]}'
    Compare-DefaultProfile 'nested-over-adjacent' 'default-order-nested'
    Write-Fixture 'default-order-adjacent\vanilla.json' '{"name":"Adjacent rules","rules":[{"id":"two","name":"Two","source":"a","target":"b"}]}'
    Compare-DefaultProfile 'adjacent' 'default-order-adjacent'
    Write-Fixture 'default-order-malformed\profiles\vanilla.json' '{invalid JSON'
    Write-Fixture 'default-order-malformed\vanilla.json' '{"name":"Adjacent rules","rules":[{"id":"two","name":"Two","source":"a","target":"b"}]}'
    Compare-DefaultProfile 'malformed-nested' 'default-order-malformed'
    Compare-DefaultProfile 'embedded' 'default-order-embedded'
    Compare-Profile 'defaults' '{"rules":[{"id":"one","name":"One","source":"options.txt","target":"options.txt"}]}'
    Compare-Profile 'case-duplicate-fields' '{"NAME":"Earlier","name":"Later","rules":[{"id":"one","name":"One","SOURCE":"old.txt","source":"new.txt","target":"copy.txt"}]}'
    Compare-Profile 'mixed-enum-case' '{"name":"Mixed","rules":[{"id":"one","name":"One","kind":"GlOb","source":"mods/*.jar","target":"mods","risk":"optional"}]}'
    Compare-Profile 'numeric-rule-kind' '{"name":"Numeric","rules":[{"id":"one","name":"One","kind":"1","source":"config","target":"config"}]}'
    Compare-Profile 'numeric-builtin-kind' '{"name":"Numeric","rules":[{"id":"one","name":"One","kind":"3","builtin":"1","source":"servers.dat","target":"servers.dat"}]}'
    Compare-Profile 'trimmed-enum-kind' '{"name":"Trimmed","rules":[{"id":"one","name":"One","kind":" Directory ","source":"config","target":"config"}]}'
    Compare-Profile 'unsupported-kind' '{"name":"Bad","rules":[{"id":"one","name":"One","kind":"mystery","source":"a","target":"b"}]}'
    Compare-Profile 'fractional-schema' '{"schemaVersion":1.5,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Compare-Profile 'exponent-schema' '{"schemaVersion":1e2,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Compare-Profile 'overflow-schema' '{"schemaVersion":2147483648,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Compare-Profile 'null-schema' '{"schemaVersion":null,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Compare-Profile 'null-profile-name' '{"name":null,"rules":[{"id":"one","name":"One","source":"a","target":"b"}]}'
    Compare-Profile 'null-kind' '{"rules":[{"id":"one","name":"One","kind":null,"source":"a","target":"b"}]}'
    Compare-Profile 'null-risk' '{"rules":[{"id":"one","name":"One","source":"a","target":"b","risk":null}]}'
    Compare-Profile 'null-recursive' '{"rules":[{"id":"one","name":"One","source":"a","target":"b","recursive":null}]}'
    Compare-Profile 'null-default-selected' '{"rules":[{"id":"one","name":"One","source":"a","target":"b","defaultSelected":null}]}'
    $profileUtf16 = [System.Text.Encoding]::Unicode
    $profileUtf16Text = '{"name":"Encoded","rules":[{"id":"one","name":"One","source":"a.txt","target":"b.txt"}]}'
    Compare-Profile 'utf16-le' $profileUtf16Text `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $profileUtf16.GetBytes($profileUtf16Text)))
    $profileUtf16Be = [System.Text.Encoding]::BigEndianUnicode
    Compare-Profile 'utf16-be' $profileUtf16Text `
        ([byte[]]([byte[]]@(0xFE, 0xFF) + $profileUtf16Be.GetBytes($profileUtf16Text)))
    $profileUtf32Le = [System.Text.UTF32Encoding]::new($false, $true)
    Compare-Profile 'utf32-le' $profileUtf16Text `
        ([byte[]]([byte[]]@(0xFF, 0xFE, 0x00, 0x00) + $profileUtf32Le.GetBytes($profileUtf16Text)))
    $profileUtf32Be = [System.Text.UTF32Encoding]::new($true, $true)
    Compare-Profile 'utf32-be' $profileUtf16Text `
        ([byte[]]([byte[]]@(0x00, 0x00, 0xFE, 0xFF) + $profileUtf32Be.GetBytes($profileUtf16Text)))
    Compare-Profile 'unsafe-source' '{"name":"Bad","rules":[{"id":"one","name":"One","source":"../outside.txt","target":"safe.txt"}]}'
    Compare-Profile 'unsupported-risk' '{"name":"Bad","rules":[{"id":"one","name":"One","source":"a.txt","target":"b.txt","risk":"danger"}]}'
    $upperRuleId = 'R' + ([char]0x00DC).ToString() + 'LE'
    $lowerRuleId = 'r' + ([char]0x00FC).ToString() + 'le'
    Compare-Profile 'unicode-duplicate-id' `
        ('{"name":"Bad","rules":[{"id":"' + $upperRuleId + '","name":"One","source":"a.txt","target":"a.txt"},{"id":"' + $lowerRuleId + '","name":"Two","source":"b.txt","target":"b.txt"}]}')
    Compare-ConfigFields 'json-nested' 'json' '{"enabled":true,"nested":{"count":2,"names":["a","b"]},"removed":null}' '{"enabled":false,"nested":{"count":1,"names":["a","c"]},"added":5}'
    Compare-ConfigFields 'json-hotkeys' 'json' '{"hotkeys":{"openMenu":"K"},"newOption":null}' '{"hotkeys":{"openMenu":"M"}}'
    Compare-ConfigFields 'json5-comments' 'json5' "{`n// comment`n`"enabled`": true,`n`"items`": [1,2,],`n}" "{`n`"enabled`": false,`n`"items`": [1,3]`n}"
    Compare-ConfigFields 'cfg-sections' 'cfg' "# comment`n[ui]`nenabled=true`nsize: large`n[controls]`nforward = W`n" "[ui]`nenabled=false`nsize: large`n[controls]`nforward = Up`nextra: yes`n"
    Compare-ConfigFields 'properties-case' 'properties' "Name=source`nAdded: yes`n" "name=target`nRemoved: yes`n"
    Compare-ConfigFields 'properties-duplicate' 'properties' "key=one`nkey=two`n" "key=other`n"
    Compare-ConfigFields 'properties-size-label' 'properties' ("key=one`nkey=two`n" + ('x' * 1100)) ("key=other`n" + ('x' * 1200))
    Compare-ConfigFields 'json-format-only' 'json' '{"a":1,"b":2}' '{ "b": 2, "a": 1 }'
    Compare-ConfigFields 'json-number-spelling' 'json' `
        '{"exponent":1e3,"fraction":1.0,"negativeZero":-0,"large":9007199254740993}' `
        '{"exponent":1000,"fraction":1,"negativeZero":0,"large":9007199254740992}'
    Compare-ConfigFields 'json-big-exponent' 'json' '{"value":1e999999}' '{"value":1e999998}'
    Compare-ConfigFields 'json-number-vs-string' 'json' '{"value":1e3}' '{"value":"1e3"}'
    Compare-ConfigFields 'json-invalid-number' 'json' '{"value":01}' '{"value":1}'
    Compare-ConfigFields 'json-duplicate-case' 'json' '{"Name":"first","name":"source"}' '{"Name":"target"}'
    $utf16leConfig = [System.Text.Encoding]::Unicode
    Compare-ConfigFields 'json-utf16-le' 'json' '{"enabled":true}' '{"enabled":false}' `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16leConfig.GetBytes('{"enabled":true}'))) `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16leConfig.GetBytes('{"enabled":false}')))
    $utf16beConfig = [System.Text.Encoding]::BigEndianUnicode
    Compare-ConfigFields 'json-utf16-be' 'json' '{"enabled":true}' '{"enabled":false}' `
        ([byte[]]([byte[]]@(0xFE, 0xFF) + $utf16beConfig.GetBytes('{"enabled":true}'))) `
        ([byte[]]([byte[]]@(0xFE, 0xFF) + $utf16beConfig.GetBytes('{"enabled":false}')))
    $utf32leConfig = [System.Text.UTF32Encoding]::new($false, $true)
    Compare-ConfigFields 'json-utf32-le' 'json' '{"enabled":true}' '{"enabled":false}' `
        ([byte[]]([byte[]]@(0xFF, 0xFE, 0x00, 0x00) + $utf32leConfig.GetBytes('{"enabled":true}'))) `
        ([byte[]]([byte[]]@(0xFF, 0xFE, 0x00, 0x00) + $utf32leConfig.GetBytes('{"enabled":false}')))
    $utf32beConfig = [System.Text.UTF32Encoding]::new($true, $true)
    Compare-ConfigFields 'json-utf32-be' 'json' '{"enabled":true}' '{"enabled":false}' `
        ([byte[]]([byte[]]@(0x00, 0x00, 0xFE, 0xFF) + $utf32beConfig.GetBytes('{"enabled":true}'))) `
        ([byte[]]([byte[]]@(0x00, 0x00, 0xFE, 0xFF) + $utf32beConfig.GetBytes('{"enabled":false}')))
    Compare-ConfigFields 'cfg-utf16-cr-only' 'cfg' "[ui]`renabled=true`r" "[ui]`renabled=false`r" `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16leConfig.GetBytes("[ui]`renabled=true`r"))) `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16leConfig.GetBytes("[ui]`renabled=false`r")))
    Compare-ConfigFields 'properties-utf8-bom' 'properties' 'enabled=true' 'enabled=false' `
        ([byte[]]([byte[]]@(0xEF, 0xBB, 0xBF) + $utf8.GetBytes('enabled=true'))) `
        ([byte[]]([byte[]]@(0xEF, 0xBB, 0xBF) + $utf8.GetBytes('enabled=false')))
    $upperConfigKey = ([char]0x00C4).ToString() + 'ction'
    $lowerConfigKey = ([char]0x00E4).ToString() + 'ction'
    Compare-ConfigFields 'json-unicode-case' 'json' ('{"' + $upperConfigKey + '":1}') `
        ('{"' + $lowerConfigKey + '":2}')
    Compare-ConfigFields 'cfg-unicode-case' 'cfg' ($upperConfigKey + '=source') `
        ($lowerConfigKey + '=target')
    Compare-ConfigFields 'cfg-unicode-duplicate' 'cfg' `
        ($upperConfigKey + '=first' + "`n" + $lowerConfigKey + '=second') `
        ($upperConfigKey + '=target')
    New-Item -ItemType Directory -Path (Join-Path $fixture 'source\saves\Empty') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fixture 'target') -Force | Out-Null
    Write-Fixture 'discovery\.minecraft\options.txt' 'gamma:1.0'
    Write-Fixture 'discovery\.minecraft\screenshots\nested\shot.JPG' 'image'
    Write-Fixture 'discovery\.minecraft\screenshots\nested\ignored.gif' 'image'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'discovery\.minecraft\saves\World-A') -Force | Out-Null
    Write-Fixture 'discovery\.minecraft\versions\zebra\servers.dat' 'server'
    $unicodeVersion = ([char]0x00E4).ToString() + 'ther'
    Write-Fixture ("discovery\.minecraft\versions\$unicodeVersion\options.txt") 'key_key.forward:key.keyboard.w'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'discovery\.minecraft\versions\Empty') -Force | Out-Null
    Write-Fixture 'discovery\direct\mods\alpha.jar' 'mod'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'discovery\empty') -Force | Out-Null
    Compare-Discovery 'minecraft-and-versions' (Join-Path $fixture 'discovery\.minecraft')
    Compare-Discovery 'direct-folder' (Join-Path $fixture 'discovery\direct')
    Compare-Discovery 'empty-folder' (Join-Path $fixture 'discovery\empty')
    Write-Fixture 'glob-source\mods\A.JAR' 'upper-case extension'
    Write-Fixture 'glob-source\mods\ab.jar' 'two-character name'
    Write-Fixture 'glob-source\mods\long.jar' 'long name'
    Write-Fixture 'glob-source\mods\lib\nested.jar' 'nested file'
    Write-Fixture 'glob-source\mods\lib\sub\deep.jar' 'deep file'
    Write-Fixture 'glob-source\mods\lib\ignored.txt' 'not a jar'
    Write-Fixture 'glob-source\mods\v1\file.dat' 'wildcard directory'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'glob-target') -Force | Out-Null
    Compare-GlobPattern 'case-insensitive' 'mods/*.jar'
    Compare-GlobPattern 'single-character' 'mods/??.jar'
    Compare-GlobPattern 'nested' 'mods/**/*.jar'
    Compare-GlobPattern 'missing' 'mods/*.zip'
    Compare-GlobPattern 'wildcard-directory' 'mods/v*/file.dat' $true
    Write-Fixture 'profile.json' @'
{"schemaVersion":1,"name":"Parity fixture","rules":[
  {"id":"keys","name":"Keys","kind":"builtin","builtin":"options","category":"bindings","source":"options.txt","target":"options.txt","defaultSelected":true},
  {"id":"audio","name":"Audio","kind":"builtin","builtin":"options","category":"audio","source":"options.txt","target":"options.txt","defaultSelected":true},
  {"id":"video","name":"Video","kind":"builtin","builtin":"options","category":"video","source":"options.txt","target":"options.txt","defaultSelected":false},
  {"id":"servers","name":"Servers","kind":"builtin","builtin":"servers","source":"servers.dat","target":"servers.dat","defaultSelected":true},
  {"id":"config","name":"Config","kind":"file","source":"config/settings.json","target":"config/settings.json","defaultSelected":true},
  {"id":"locked","name":"Read-only target","kind":"file","source":"locked.txt","target":"locked.txt","defaultSelected":true},
  {"id":"identical","name":"Identical","kind":"file","source":"config/identical.cfg","target":"config/identical.cfg","defaultSelected":true},
  {"id":"images","name":"Images","kind":"directory","source":"screenshots","target":"screenshots","defaultSelected":true},
  {"id":"maps","name":"Maps","kind":"directory","source":"maps","target":"maps","defaultSelected":true},
  {"id":"top-only","name":"Top-level files","kind":"directory","source":"top-only","target":"top-only","recursive":false,"defaultSelected":true},
  {"id":"emptydir","name":"Empty directory","kind":"directory","source":"empty-data","target":"empty-data","defaultSelected":true},
  {"id":"worlds","name":"Worlds","kind":"builtin","builtin":"worlds","source":"saves","target":"saves","defaultSelected":true},
  {"id":"mods","name":"Mods","kind":"glob","source":"mods/*.jar","target":"mods","defaultSelected":true},
  {"id":"missing","name":"Missing","kind":"file","source":"absent.txt","target":"absent.txt","defaultSelected":true}
]}
'@
    Write-Fixture 'source\options.txt' "key_key.forward:key.keyboard.w`nkey_key.jump:key.keyboard.space`nsoundCategory_master:0.8`nmusic:0.7`nfov:95`ngamma:1.0`n# source comment`n"
    Write-Fixture 'target\options.txt' "key_key.forward:key.keyboard.up`r`nkey_key.jump:key.keyboard.space`r`nsoundCategory_master:0.2`r`nmusic:0.4`r`nfov:80`r`ngamma:0.5`r`nunrelated:true`r`n"
    Write-Fixture 'source\config\settings.json' '{"enabled":true}'
    Write-Fixture 'target\config\settings.json' '{"enabled":false}'
    Write-Fixture 'source\locked.txt' 'incoming'
    Write-Fixture 'target\locked.txt' 'existing'
    Write-Fixture 'source\config\identical.cfg' 'same=true'
    Write-Fixture 'target\config\identical.cfg' 'same=true'
    Write-Fixture 'source\screenshots\same.png' 'same'
    Write-Fixture 'target\screenshots\same.png' 'same'
    Write-Fixture 'source\screenshots\new.png' 'new'
    Write-Fixture 'source\maps\waypoint.txt' 'new waypoint'
    Write-Fixture 'target\maps\waypoint.txt' 'old waypoint'
    Write-Fixture 'source\top-only\root.txt' 'incoming top-level file'
    Write-Fixture 'source\top-only\nested\ignored.txt' 'must not migrate'
    Write-Fixture 'target\top-only\root.txt' 'existing top-level file'
    Write-Fixture 'target\top-only\nested\preserved.txt' 'keep destination nested file'
    New-Item -ItemType Directory -Path (Join-Path $fixture 'source\empty-data') -Force | Out-Null
    Write-Fixture 'source\saves\Changed\level.dat' 'save'
    Write-Fixture 'target\saves\Changed\level.dat' 'save'
    Write-Fixture 'target\saves\Changed\note.txt' 'target only'
    Write-Fixture 'source\mods\alpha.jar' 'mod'

    $profile = Join-Path $fixture 'profile.json'
    $source = Join-Path $fixture 'source'
    $target = Join-Path $fixture 'target'
    & $nativeMigrationExe --fixture-servers $source $target
    if ($LASTEXITCODE -ne 0) { throw 'Could not create isolated NBT server fixtures' }
    $referenceJson = & dotnet $referenceExe $profile $source $target
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET scan failed' }
    $nativeJson = & $nativeExe --plan-json $profile $source $target
    if ($LASTEXITCODE -ne 0) { throw 'C++ scan failed' }
    $reference = $referenceJson | ConvertFrom-Json
    $native = $nativeJson | ConvertFrom-Json
    if ($reference.valid -ne $native.valid -or $reference.error -ne $native.error) {
        throw 'Scan plan validity differs between .NET and C++'
    }
    $referenceOperations = @($reference.operations | Sort-Object id, source, target)
    $nativeOperations = @($native.operations | Sort-Object id, source, target)
    if ($referenceOperations.Count -ne $nativeOperations.Count) {
        throw "Scan item count differs: .NET=$($referenceOperations.Count), C++=$($nativeOperations.Count)"
    }
    $fields = @('id', 'name', 'status', 'selected', 'source', 'target', 'size', 'directory', 'builtin')
    for ($index = 0; $index -lt $referenceOperations.Count; $index++) {
        foreach ($field in $fields) {
            $expected = $referenceOperations[$index].$field
            $actual = $nativeOperations[$index].$field
            if ($expected -cne $actual) {
                throw "Scan item $index field '$field' differs: .NET='$expected', C++='$actual'`n.NET: $referenceJson`nC++: $nativeJson"
            }
        }
    }
    Write-Output "Original .NET and C++ scans agree on $($referenceOperations.Count) isolated items."

    foreach ($strategy in @('backup', 'replace', 'skip')) {
    $referenceTarget = Join-Path $fixture "target-reference-$strategy"
    $nativeTarget = Join-Path $fixture "target-native-$strategy"
    Copy-Item -LiteralPath $target -Destination $referenceTarget -Recurse
    Copy-Item -LiteralPath $target -Destination $nativeTarget -Recurse
    (Get-Item -LiteralPath (Join-Path $referenceTarget 'locked.txt')).IsReadOnly = $true
    (Get-Item -LiteralPath (Join-Path $nativeTarget 'locked.txt')).IsReadOnly = $true
    $referenceResultJson = & dotnet $referenceExe --execute $strategy $profile $source $referenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET migration failed' }
    $nativeResultJson = & $nativeMigrationExe $strategy $profile $source $nativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ migration failed' }
    $referenceResult = $referenceResultJson | ConvertFrom-Json
    $nativeResult = $nativeResultJson | ConvertFrom-Json
    $expectedFailures = if ($strategy -eq 'skip') { 0 } else { 1 }
    if ($referenceResult.failed -ne $expectedFailures -or $nativeResult.failed -ne $expectedFailures) {
        throw "Read-only target did not produce the expected failure count for $strategy"
    }
    $expectedLockedStatus = if ($strategy -eq 'skip') { 'Skipped' } else { 'Failed' }
    $lockedResult = @($nativeResult.items | Where-Object { $_.target -eq 'locked.txt' })
    if ($lockedResult.Count -ne 1 -or $lockedResult[0].status -ne $expectedLockedStatus -or
        [System.IO.File]::ReadAllText((Join-Path $nativeTarget 'locked.txt')) -cne 'existing') {
        throw "Read-only destination was modified or misreported under $strategy"
    }
    Write-Verbose ".NET migration: $referenceResultJson"
    Write-Verbose "C++ migration: $nativeResultJson"
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($referenceResult.$field -ne $nativeResult.$field) {
            throw "Migration summary '$field' differs: .NET=$($referenceResult.$field), C++=$($nativeResult.$field)`n.NET: $referenceResultJson`nC++: $nativeResultJson"
        }
    }
    $referenceItems = @($referenceResult.items | Sort-Object target, name)
    $nativeItems = @($nativeResult.items | Sort-Object target, name)
    if ($referenceItems.Count -ne $nativeItems.Count) {
        throw "Migration result count differs: .NET=$($referenceItems.Count), C++=$($nativeItems.Count)"
    }
    for ($index = 0; $index -lt $referenceItems.Count; $index++) {
        foreach ($field in @('name', 'target', 'status', 'copied', 'overwritten', 'backedUp')) {
            $expected = $referenceItems[$index].$field
            $actual = $nativeItems[$index].$field
            if ($expected -cne $actual) {
                throw "Migration item $index '$field' differs: .NET='$expected', C++='$actual'"
            }
        }
    }
    function Get-MigratedFiles([string]$root) {
        @(
            Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
                $relative = (Get-RelativeFixturePath $root $_.FullName).Replace('\', '/')
                if ($relative -ine 'servers.dat' -and
                    -not ($relative.StartsWith('SempervirensReports/', [System.StringComparison]::OrdinalIgnoreCase) -or
                           $relative.StartsWith('SempervirensBackups/', [System.StringComparison]::OrdinalIgnoreCase))) {
                    "$relative|$((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)"
                }
            } | Sort-Object
        )
    }
    $referenceFiles = @(Get-MigratedFiles $referenceTarget)
    $nativeFiles = @(Get-MigratedFiles $nativeTarget)
    if ($referenceFiles.Count -ne $nativeFiles.Count) {
        throw "Migrated file count differs: .NET=$($referenceFiles.Count), C++=$($nativeFiles.Count)"
    }
    for ($index = 0; $index -lt $referenceFiles.Count; $index++) {
        if ($referenceFiles[$index] -cne $nativeFiles[$index]) {
            throw "Migrated file differs: .NET='$($referenceFiles[$index])', C++='$($nativeFiles[$index])'"
        }
    }
    foreach ($resultRoot in @($referenceTarget, $nativeTarget)) {
        if (Test-Path -LiteralPath (Join-Path $resultRoot 'top-only\nested\ignored.txt')) {
            throw "Non-recursive directory rule copied a nested source file under $resultRoot"
        }
        if ([System.IO.File]::ReadAllText((Join-Path $resultRoot 'top-only\nested\preserved.txt')) -cne
            'keep destination nested file') {
            throw "Non-recursive directory rule changed destination-only nested data under $resultRoot"
        }
    }
    $referenceServers = & $nativeMigrationExe --nbt-json (Join-Path $referenceTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect original .NET server output' }
    $nativeServers = & $nativeMigrationExe --nbt-json (Join-Path $nativeTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect C++ server output' }
    if ($referenceServers -cne $nativeServers) {
        throw "Migrated server entries differ:`n.NET: $referenceServers`nC++: $nativeServers"
    }
    function Get-BackupFiles([string]$root) {
        $backupRoot = Join-Path $root 'SempervirensBackups'
        if (-not (Test-Path -LiteralPath $backupRoot)) { return @() }
        @(
            Get-ChildItem -LiteralPath $backupRoot -Recurse -File | ForEach-Object {
                $relative = (Get-RelativeFixturePath $backupRoot $_.FullName).Replace('\', '/')
                $separator = $relative.IndexOf('/')
                if ($separator -lt 0) { throw "Backup file has no timestamp folder: $relative" }
                "$($relative.Substring($separator + 1))|$((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)"
            } | Sort-Object
        )
    }
    $referenceBackups = @(Get-BackupFiles $referenceTarget)
    $nativeBackups = @(Get-BackupFiles $nativeTarget)
    if ($strategy -eq 'backup' -and -not @($nativeBackups | Where-Object { $_ -like 'locked.txt|*' }).Count) {
        throw 'Read-only destination was not backed up before the failed replacement'
    }
    if ($referenceBackups.Count -ne $nativeBackups.Count) {
        throw "Backup file count differs: .NET=$($referenceBackups.Count), C++=$($nativeBackups.Count)"
    }
    for ($index = 0; $index -lt $referenceBackups.Count; $index++) {
        if ($referenceBackups[$index] -cne $nativeBackups[$index]) {
            throw "Backup file differs: .NET='$($referenceBackups[$index])', C++='$($nativeBackups[$index])'"
        }
    }
    function Get-NormalizedReport([string]$root) {
        $reportRoot = Join-Path $root 'SempervirensReports'
        $reports = @(Get-ChildItem -LiteralPath $reportRoot -File)
        if ($reports.Count -ne 1) { throw "Expected one migration report under $reportRoot" }
        $content = [System.IO.File]::ReadAllText($reports[0].FullName, [System.Text.Encoding]::UTF8)
        $content = $content.Replace($root, '<target>')
        $content = [regex]::Replace($content, '(?m)^Created: [^\r\n]+', 'Created: <time>')
        $content = [regex]::Replace($content, 'SempervirensBackups[\\/][^\\/\r\n]+', 'SempervirensBackups/<stamp>')
        return $content
    }
    $referenceReport = Get-NormalizedReport $referenceTarget
    $nativeReport = Get-NormalizedReport $nativeTarget
    if ($referenceReport -cne $nativeReport) {
        throw "Migration report differs:`n.NET:`n$referenceReport`nC++:`n$nativeReport"
    }
    Write-Output "$strategy migration: summaries, $($referenceFiles.Count) ordinary files, server NBT, $($referenceBackups.Count) backups and record agree."
    }

    $unicodeRootRelative = 'unicode-paths'
    $unicodeSourceName = [string]::Concat([char[]]@(0x8FC1, 0x51FA, 0x5B9E, 0x4F8B)) + ' A'
    $unicodeTargetName = [string]::Concat([char[]]@(0x8FC1, 0x5165, 0x5B9E, 0x4F8B))
    $unicodeFileStem = [string]::Concat([char[]]@(0x914D, 0x7F6E, 0x952E, 0x4F4D))
    $unicodeFile = "config/$unicodeFileStem settings.txt"
    $unicodeLocalFile = $unicodeFile.Replace('/', '\')
    $unicodeSourceRelative = Join-Path $unicodeRootRelative $unicodeSourceName
    $unicodeReferenceRelative = Join-Path $unicodeRootRelative ($unicodeTargetName + ' reference')
    $unicodeNativeRelative = Join-Path $unicodeRootRelative ($unicodeTargetName + ' native')
    $unicodeProfileRelative = Join-Path $unicodeRootRelative 'profile.json'
    $unicodeProfileText = @{
        schemaVersion = 1
        name = $unicodeFileStem + ' profile'
        rules = @(@{
            id = 'unicode-file'
            group = 'Test'
            name = $unicodeFileStem
            kind = 'file'
            source = $unicodeFile
            target = $unicodeFile
            defaultSelected = $true
        })
    } | ConvertTo-Json -Depth 6 -Compress
    Write-Fixture $unicodeProfileRelative $unicodeProfileText
    Write-Fixture (Join-Path $unicodeSourceRelative $unicodeLocalFile) 'source content'
    Write-Fixture (Join-Path $unicodeReferenceRelative $unicodeLocalFile) 'previous content'
    Write-Fixture (Join-Path $unicodeNativeRelative $unicodeLocalFile) 'previous content'
    $unicodeProfile = Join-Path $fixture $unicodeProfileRelative
    $unicodeSource = Join-Path $fixture $unicodeSourceRelative
    $unicodeReferenceTarget = Join-Path $fixture $unicodeReferenceRelative
    $unicodeNativeTarget = Join-Path $fixture $unicodeNativeRelative
    $unicodeReferenceScanJson = & dotnet $referenceExe $unicodeProfile $unicodeSource $unicodeReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET Unicode-path scan failed' }
    $unicodeNativeScanJson = & $nativeExe --plan-json $unicodeProfile $unicodeSource $unicodeNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ Unicode-path scan failed' }
    $unicodeReferenceScan = $unicodeReferenceScanJson | ConvertFrom-Json
    $unicodeNativeScan = $unicodeNativeScanJson | ConvertFrom-Json
    if (-not $unicodeReferenceScan.valid -or -not $unicodeNativeScan.valid -or
        @($unicodeReferenceScan.operations).Count -ne 1 -or
        @($unicodeNativeScan.operations).Count -ne 1) {
        throw 'Unicode-path scan did not produce one valid migration item in both versions'
    }
    foreach ($field in @('id', 'name', 'status', 'selected', 'source', 'target', 'size')) {
        if ($unicodeReferenceScan.operations[0].$field -cne $unicodeNativeScan.operations[0].$field) {
            throw "Unicode-path scan field '$field' differs"
        }
    }
    $unicodeReferenceResultJson = & dotnet $referenceExe --execute backup $unicodeProfile $unicodeSource $unicodeReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET Unicode-path migration failed' }
    $unicodeNativeResultJson = & $nativeMigrationExe backup $unicodeProfile $unicodeSource $unicodeNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ Unicode-path migration failed' }
    $unicodeReferenceResult = $unicodeReferenceResultJson | ConvertFrom-Json
    $unicodeNativeResult = $unicodeNativeResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($unicodeReferenceResult.$field -ne $unicodeNativeResult.$field) {
            throw "Unicode-path migration summary '$field' differs"
        }
    }
    foreach ($field in @('name', 'target', 'status', 'copied', 'overwritten', 'backedUp')) {
        if ($unicodeReferenceResult.items[0].$field -cne $unicodeNativeResult.items[0].$field) {
            throw "Unicode-path migration item '$field' differs"
        }
    }
    $unicodeReferenceFile = Join-Path $unicodeReferenceTarget $unicodeLocalFile
    $unicodeNativeFile = Join-Path $unicodeNativeTarget $unicodeLocalFile
    if ((Get-FileHash -LiteralPath $unicodeReferenceFile -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $unicodeNativeFile -Algorithm SHA256).Hash) {
        throw 'Unicode-path migrated file differs'
    }
    $unicodeReferenceBackups = @(Get-BackupFiles $unicodeReferenceTarget)
    $unicodeNativeBackups = @(Get-BackupFiles $unicodeNativeTarget)
    if ($unicodeReferenceBackups.Count -ne 1 -or
        $unicodeNativeBackups.Count -ne 1 -or
        $unicodeReferenceBackups[0] -cne $unicodeNativeBackups[0]) {
        throw 'Unicode-path backup differs'
    }
    if ((Get-NormalizedReport $unicodeReferenceTarget) -cne
        (Get-NormalizedReport $unicodeNativeTarget)) {
        throw 'Unicode-path migration record differs'
    }
    Write-Output 'Unicode paths and spaces: scan, migrated file, backup and record agree.'

    $longProfileRelative = 'long-path-profile.json'
    $longProfileText = @{
        schemaVersion = 1
        name = 'Long path profile'
        rules = @(@{
            id = 'long-file'
            group = 'Test'
            name = 'Long path file'
            kind = 'file'
            source = 'config/settings.txt'
            target = 'config/settings.txt'
            defaultSelected = $true
        })
    } | ConvertTo-Json -Depth 6 -Compress
    Write-Fixture $longProfileRelative $longProfileText
    $longBase = $fixture
    foreach ($segment in 1..5) {
        $longBase = [System.IO.Path]::Combine($longBase,
            ('segment-' + $segment + '-' + ('x' * 45)))
    }
    $longSource = [System.IO.Path]::Combine($longBase, 'source-instance')
    $longReferenceTarget = [System.IO.Path]::Combine($longBase, 'reference-target')
    $longNativeTarget = [System.IO.Path]::Combine($longBase, 'native-target')
    foreach ($directory in @($longSource, $longReferenceTarget, $longNativeTarget)) {
        [System.IO.Directory]::CreateDirectory([System.IO.Path]::Combine($directory, 'config')) | Out-Null
    }
    $longSourceFile = [System.IO.Path]::Combine($longSource, 'config', 'settings.txt')
    $longReferenceFile = [System.IO.Path]::Combine($longReferenceTarget, 'config', 'settings.txt')
    $longNativeFile = [System.IO.Path]::Combine($longNativeTarget, 'config', 'settings.txt')
    [System.IO.File]::WriteAllText($longSourceFile, 'new long-path content', $utf8)
    [System.IO.File]::WriteAllText($longReferenceFile, 'old long-path content', $utf8)
    [System.IO.File]::WriteAllText($longNativeFile, 'old long-path content', $utf8)
    if ($longSourceFile.Length -le 260 -or $longReferenceFile.Length -le 260 -or
        $longNativeFile.Length -le 260) {
        throw 'The long-path fixture did not exceed the traditional MAX_PATH limit'
    }
    $longProfile = Join-Path $fixture $longProfileRelative
    $longReferenceScanJson = & dotnet $referenceExe $longProfile $longSource $longReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET long-path scan failed' }
    $longNativeScanJson = & $nativeExe --plan-json $longProfile $longSource $longNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ long-path scan failed' }
    $longReferenceScan = $longReferenceScanJson | ConvertFrom-Json
    $longNativeScan = $longNativeScanJson | ConvertFrom-Json
    $longReferenceItems = @($longReferenceScan.operations)
    $longNativeItems = @($longNativeScan.operations)
    if (-not $longReferenceScan.valid -or -not $longNativeScan.valid -or
        $longReferenceItems.Count -ne 1 -or $longNativeItems.Count -ne 1) {
        throw 'Long-path scan did not produce one valid migration item in both versions'
    }
    foreach ($field in @('id', 'name', 'status', 'selected', 'size')) {
        if ($longReferenceItems[0].$field -cne $longNativeItems[0].$field) {
            throw "Long-path scan field '$field' differs"
        }
    }
    $longReferenceResultJson = & dotnet $referenceExe --execute backup $longProfile $longSource $longReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET long-path migration failed' }
    $longNativeResultJson = & $nativeMigrationExe backup $longProfile $longSource $longNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ long-path migration failed' }
    $longReferenceResult = $longReferenceResultJson | ConvertFrom-Json
    $longNativeResult = $longNativeResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($longReferenceResult.$field -ne $longNativeResult.$field) {
            throw "Long-path migration summary '$field' differs"
        }
    }
    $longExpectedHash = (Get-FileHash -LiteralPath $longSourceFile -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $longReferenceFile -Algorithm SHA256).Hash -ne $longExpectedHash -or
        (Get-FileHash -LiteralPath $longNativeFile -Algorithm SHA256).Hash -ne $longExpectedHash) {
        throw 'Long-path migrated target content differs'
    }
    $longReferenceBackups = @(Get-BackupFiles $longReferenceTarget)
    $longNativeBackups = @(Get-BackupFiles $longNativeTarget)
    if ($longReferenceBackups.Count -ne 1 -or $longNativeBackups.Count -ne 1 -or
        $longReferenceBackups[0] -cne $longNativeBackups[0]) {
        throw 'Long-path backup differs'
    }
    if ((Get-NormalizedReport $longReferenceTarget) -cne
        (Get-NormalizedReport $longNativeTarget)) {
        throw 'Long-path migration record differs'
    }
    Write-Output "Long paths: $($longSourceFile.Length)-character source path, scan, migration, backup and record agree."

    $emptyWorldRoot = Join-Path $fixture 'empty-world-parity'
    $emptyWorldProfile = Join-Path $emptyWorldRoot 'profile.json'
    $emptyWorldSource = Join-Path $emptyWorldRoot 'source'
    $emptyWorldReferenceTarget = Join-Path $emptyWorldRoot 'reference-target'
    $emptyWorldNativeTarget = Join-Path $emptyWorldRoot 'native-target'
    $emptyWorldProfileText = @{
        schemaVersion = 1
        name = 'Empty world parity'
        rules = @(@{
            id = 'worlds'
            group = 'Test'
            name = 'Worlds'
            kind = 'builtin'
            builtin = 'worlds'
            source = 'saves'
            target = 'saves'
            defaultSelected = $true
        })
    } | ConvertTo-Json -Depth 6 -Compress
    [System.IO.Directory]::CreateDirectory((Join-Path $emptyWorldSource 'saves\Empty')) | Out-Null
    [System.IO.Directory]::CreateDirectory($emptyWorldReferenceTarget) | Out-Null
    [System.IO.Directory]::CreateDirectory($emptyWorldNativeTarget) | Out-Null
    [System.IO.File]::WriteAllText($emptyWorldProfile, $emptyWorldProfileText, $utf8)

    $emptyReferenceScanJson = & dotnet $referenceExe $emptyWorldProfile $emptyWorldSource $emptyWorldReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET empty-world scan failed' }
    $emptyNativeScanJson = & $nativeExe --plan-json $emptyWorldProfile $emptyWorldSource $emptyWorldNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ empty-world scan failed' }
    $emptyReferenceScan = $emptyReferenceScanJson | ConvertFrom-Json
    $emptyNativeScan = $emptyNativeScanJson | ConvertFrom-Json
    if (@($emptyReferenceScan.operations).Count -ne 1 -or @($emptyNativeScan.operations).Count -ne 1 -or
        $emptyReferenceScan.operations[0].status -cne 'Found' -or
        $emptyNativeScan.operations[0].status -cne 'Found' -or
        -not $emptyReferenceScan.operations[0].selected -or -not $emptyNativeScan.operations[0].selected) {
        throw 'Missing destination empty world was not offered identically by both scanners'
    }
    $emptyReferenceResultJson = & dotnet $referenceExe --execute replace $emptyWorldProfile $emptyWorldSource $emptyWorldReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET empty-world migration failed' }
    $emptyNativeResultJson = & $nativeMigrationExe replace $emptyWorldProfile $emptyWorldSource $emptyWorldNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ empty-world migration failed' }
    $emptyReferenceResult = $emptyReferenceResultJson | ConvertFrom-Json
    $emptyNativeResult = $emptyNativeResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($emptyReferenceResult.$field -ne $emptyNativeResult.$field) {
            throw "Empty-world migration summary '$field' differs"
        }
    }
    if (@($emptyReferenceResult.items).Count -ne 1 -or @($emptyNativeResult.items).Count -ne 1 -or
        $emptyReferenceResult.items[0].status -cne $emptyNativeResult.items[0].status -or
        (Test-Path -LiteralPath (Join-Path $emptyWorldReferenceTarget 'saves\Empty')) -or
        (Test-Path -LiteralPath (Join-Path $emptyWorldNativeTarget 'saves\Empty'))) {
        throw 'Empty-world migration did not preserve the original no-directory result'
    }

    [System.IO.Directory]::CreateDirectory((Join-Path $emptyWorldReferenceTarget 'saves\Empty')) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $emptyWorldNativeTarget 'saves\Empty')) | Out-Null
    $emptyReferenceScanJson = & dotnet $referenceExe $emptyWorldProfile $emptyWorldSource $emptyWorldReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET matching empty-world scan failed' }
    $emptyNativeScanJson = & $nativeExe --plan-json $emptyWorldProfile $emptyWorldSource $emptyWorldNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ matching empty-world scan failed' }
    $emptyReferenceScan = $emptyReferenceScanJson | ConvertFrom-Json
    $emptyNativeScan = $emptyNativeScanJson | ConvertFrom-Json
    if ($emptyReferenceScan.operations[0].status -cne 'Identical' -or
        $emptyNativeScan.operations[0].status -cne 'Identical') {
        throw 'Matching empty world folders were not identified equally'
    }

    [System.IO.File]::WriteAllText((Join-Path $emptyWorldReferenceTarget 'saves\Empty\target-only.txt'), 'keep', $utf8)
    [System.IO.File]::WriteAllText((Join-Path $emptyWorldNativeTarget 'saves\Empty\target-only.txt'), 'keep', $utf8)
    $emptyReferenceScanJson = & dotnet $referenceExe $emptyWorldProfile $emptyWorldSource $emptyWorldReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET target-only empty-world scan failed' }
    $emptyNativeScanJson = & $nativeExe --plan-json $emptyWorldProfile $emptyWorldSource $emptyWorldNativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ target-only empty-world scan failed' }
    $emptyReferenceScan = $emptyReferenceScanJson | ConvertFrom-Json
    $emptyNativeScan = $emptyNativeScanJson | ConvertFrom-Json
    if ($emptyReferenceScan.operations[0].status -cne 'Conflict' -or
        $emptyNativeScan.operations[0].status -cne 'Conflict' -or
        $emptyReferenceScan.operations[0].difference -notlike '*target-only.txt*' -or
        $emptyNativeScan.operations[0].difference -notlike '*target-only.txt*') {
        throw 'Destination-only content in an empty world was not explained equally'
    }
    Write-Output 'Empty worlds: missing, matching and destination-only states plus migration result agree.'

    $sourceServers = Join-Path $source 'servers.dat'
    $targetServers = Join-Path $target 'servers.dat'
    $sourceServersBytes = [System.IO.File]::ReadAllBytes($sourceServers)
    $targetServersBytes = [System.IO.File]::ReadAllBytes($targetServers)
    $invalidGzip = [byte[]]@(0x1f, 0x8b, 0x08, 0x00, 0x00)
    try {
        foreach ($damagedSide in @('source', 'target')) {
            if ($damagedSide -eq 'source') {
                [System.IO.File]::WriteAllBytes($sourceServers, $invalidGzip)
            } else {
                [System.IO.File]::WriteAllBytes($sourceServers, $sourceServersBytes)
                [System.IO.File]::WriteAllBytes($targetServers, $invalidGzip)
            }
            $referenceInvalidJson = & dotnet $referenceExe $profile $source $target
            if ($LASTEXITCODE -ne 0) { throw "Original .NET scan failed with damaged $damagedSide servers.dat" }
            $nativeInvalidJson = & $nativeExe --plan-json $profile $source $target
            if ($LASTEXITCODE -ne 0) { throw "C++ scan failed with damaged $damagedSide servers.dat" }
            $referenceInvalid = $referenceInvalidJson | ConvertFrom-Json
            $nativeInvalid = $nativeInvalidJson | ConvertFrom-Json
            $referenceServerItems = @($referenceInvalid.operations | Where-Object { $_.id -eq 'servers' })
            $nativeServerItems = @($nativeInvalid.operations | Where-Object { $_.id -eq 'servers' })
            if ($referenceInvalid.valid -ne $nativeInvalid.valid -or
                $referenceServerItems.Count -ne 1 -or $nativeServerItems.Count -ne 1 -or
                $referenceServerItems[0].status -ne 'ConfigError' -or
                $nativeServerItems[0].status -ne 'ConfigError' -or
                $referenceServerItems[0].selected -or $nativeServerItems[0].selected) {
                throw "Damaged $damagedSide server-list behavior differs:`n.NET: $referenceInvalidJson`nC++: $nativeInvalidJson"
            }
            Write-Output "Damaged $damagedSide servers.dat: both scanners report one unselected configuration error."
        }
    } finally {
        [System.IO.File]::WriteAllBytes($sourceServers, $sourceServersBytes)
        [System.IO.File]::WriteAllBytes($targetServers, $targetServersBytes)
    }

    try {
        & $nativeMigrationExe --fixture-duplicate-servers $targetServers
        if ($LASTEXITCODE -ne 0) { throw 'Could not create duplicate-address server fixture' }
        $referenceDuplicateJson = & dotnet $referenceExe $profile $source $target
        if ($LASTEXITCODE -ne 0) { throw 'Original .NET scan failed with duplicate server addresses' }
        $nativeDuplicateJson = & $nativeExe --plan-json $profile $source $target
        if ($LASTEXITCODE -ne 0) { throw 'C++ scan failed with duplicate server addresses' }
        $referenceDuplicate = $referenceDuplicateJson | ConvertFrom-Json
        $nativeDuplicate = $nativeDuplicateJson | ConvertFrom-Json
        $referenceServerItems = @($referenceDuplicate.operations | Where-Object { $_.id -eq 'servers' })
        $nativeServerItems = @($nativeDuplicate.operations | Where-Object { $_.id -eq 'servers' })
        if ($referenceServerItems.Count -ne 1 -or $nativeServerItems.Count -ne 1 -or
            $referenceServerItems[0].status -ne 'ConfigError' -or
            $nativeServerItems[0].status -ne 'ConfigError' -or
            $referenceServerItems[0].selected -or $nativeServerItems[0].selected) {
            throw "Duplicate-address server-list behavior differs:`n.NET: $referenceDuplicateJson`nC++: $nativeDuplicateJson"
        }
        Write-Output 'Duplicate target server addresses: both scanners report one unselected configuration error.'
    } finally {
        [System.IO.File]::WriteAllBytes($targetServers, $targetServersBytes)
    }

    Write-Fixture 'server-only-profile.json' '{"schemaVersion":1,"name":"Server edge cases","rules":[{"id":"servers","name":"Servers","kind":"builtin","builtin":"servers","source":"servers.dat","target":"servers.dat","defaultSelected":true}]}'
    $serverOnlyProfile = Join-Path $fixture 'server-only-profile.json'
    $nanServerSource = Join-Path $fixture 'server-nan-source'
    $nanServerTarget = Join-Path $fixture 'server-nan-target'
    New-Item -ItemType Directory -Path $nanServerSource,$nanServerTarget -Force | Out-Null
    & $nativeMigrationExe --fixture-nan-servers `
        (Join-Path $nanServerSource 'servers.dat') (Join-Path $nanServerTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not create matching NaN server fixtures' }
    $referenceNanJson = & dotnet $referenceExe $serverOnlyProfile $nanServerSource $nanServerTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET scan failed for matching NaN server metadata' }
    $nativeNanJson = & $nativeExe --plan-json $serverOnlyProfile $nanServerSource $nanServerTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ scan failed for matching NaN server metadata' }
    $referenceNanItems = @(($referenceNanJson | ConvertFrom-Json).operations)
    $nativeNanItems = @(($nativeNanJson | ConvertFrom-Json).operations)
    if ($referenceNanItems.Count -ne $nativeNanItems.Count -or $referenceNanItems.Count -eq 0) {
        throw "NaN server scan count differs:`n.NET: $referenceNanJson`nC++: $nativeNanJson"
    }
    for ($index = 0; $index -lt $referenceNanItems.Count; $index++) {
        if ($referenceNanItems[$index].status -cne 'Identical' -or
            $nativeNanItems[$index].status -cne 'Identical') {
            throw "Matching NaN server metadata was not identical in both implementations:`n.NET: $referenceNanJson`nC++: $nativeNanJson"
        }
    }
    Write-Output 'Matching NaN server metadata: original and C++ scans both report identical entries.'
    $invalidUtf8ServerSource = Join-Path $fixture 'server-invalid-utf8-source'
    $invalidUtf8ServerTarget = Join-Path $fixture 'server-invalid-utf8-target'
    New-Item -ItemType Directory -Path $invalidUtf8ServerSource,$invalidUtf8ServerTarget -Force | Out-Null
    & $nativeMigrationExe --fixture-invalid-utf8-servers `
        (Join-Path $invalidUtf8ServerSource 'servers.dat') `
        (Join-Path $invalidUtf8ServerTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not create invalid UTF-8 server fixtures' }
    $referenceInvalidUtf8Json = & dotnet $referenceExe $serverOnlyProfile `
        $invalidUtf8ServerSource $invalidUtf8ServerTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET scan failed for invalid UTF-8 server metadata' }
    $nativeInvalidUtf8Json = & $nativeExe --plan-json $serverOnlyProfile `
        $invalidUtf8ServerSource $invalidUtf8ServerTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ scan failed for invalid UTF-8 server metadata' }
    $referenceInvalidUtf8Items = @(($referenceInvalidUtf8Json | ConvertFrom-Json).operations)
    $nativeInvalidUtf8Items = @(($nativeInvalidUtf8Json | ConvertFrom-Json).operations)
    if ($referenceInvalidUtf8Items.Count -ne 1 -or $nativeInvalidUtf8Items.Count -ne 1 -or
        $referenceInvalidUtf8Items[0].status -cne 'Identical' -or
        $nativeInvalidUtf8Items[0].status -cne 'Identical' -or
        $referenceInvalidUtf8Items[0].difference -cne $nativeInvalidUtf8Items[0].difference) {
        throw "Invalid UTF-8 server replacement behavior differs:`n.NET: $referenceInvalidUtf8Json`nC++: $nativeInvalidUtf8Json"
    }
    Write-Output 'Invalid UTF-8 server name: both scanners replace malformed bytes and keep the entry usable.'
    $invalidUtf8ReferenceTarget = Join-Path $fixture 'server-invalid-utf8-reference-output'
    $invalidUtf8NativeTarget = Join-Path $fixture 'server-invalid-utf8-native-output'
    New-Item -ItemType Directory -Path $invalidUtf8ReferenceTarget,$invalidUtf8NativeTarget -Force | Out-Null
    $referenceInvalidUtf8ResultJson = & dotnet $referenceExe --execute replace $serverOnlyProfile `
        $invalidUtf8ServerSource $invalidUtf8ReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET migration failed for invalid UTF-8 server metadata' }
    $nativeInvalidUtf8ResultJson = & $nativeMigrationExe replace $serverOnlyProfile `
        $invalidUtf8ServerSource $invalidUtf8NativeTarget
    if ($LASTEXITCODE -ne 0) { throw 'C++ migration failed for invalid UTF-8 server metadata' }
    $referenceInvalidUtf8Result = $referenceInvalidUtf8ResultJson | ConvertFrom-Json
    $nativeInvalidUtf8Result = $nativeInvalidUtf8ResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($referenceInvalidUtf8Result.$field -ne $nativeInvalidUtf8Result.$field) {
            throw "Invalid UTF-8 server migration summary '$field' differs:`n.NET: $referenceInvalidUtf8ResultJson`nC++: $nativeInvalidUtf8ResultJson"
        }
    }
    $referenceInvalidUtf8Snapshot = & $nativeMigrationExe --nbt-json `
        (Join-Path $invalidUtf8ReferenceTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect original .NET invalid UTF-8 server output' }
    $nativeInvalidUtf8Snapshot = & $nativeMigrationExe --nbt-json `
        (Join-Path $invalidUtf8NativeTarget 'servers.dat')
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect C++ invalid UTF-8 server output' }
    if ($referenceInvalidUtf8Snapshot -cne $nativeInvalidUtf8Snapshot -or
        $referenceInvalidUtf8Snapshot -notlike '*Bad�*') {
        throw "Invalid UTF-8 migrated server output differs:`n.NET: $referenceInvalidUtf8Snapshot`nC++: $nativeInvalidUtf8Snapshot"
    }
    Write-Output 'Invalid UTF-8 server migration: summaries and normalized destination NBT agree.'
    foreach ($serverCase in @('blank', 'duplicate')) {
        $serverSource = Join-Path $fixture "server-$serverCase-source"
        $serverReferenceTarget = Join-Path $fixture "server-$serverCase-reference"
        $serverNativeTarget = Join-Path $fixture "server-$serverCase-native"
        foreach ($directory in @($serverSource, $serverReferenceTarget, $serverNativeTarget)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        $fixtureCommand = if ($serverCase -eq 'blank') { '--fixture-blank-server' } else { '--fixture-duplicate-servers' }
        & $nativeMigrationExe $fixtureCommand (Join-Path $serverSource 'servers.dat')
        if ($LASTEXITCODE -ne 0) { throw "Could not create $serverCase source server fixture" }
        $referenceEdgeJson = & dotnet $referenceExe $serverOnlyProfile $serverSource $serverReferenceTarget
        if ($LASTEXITCODE -ne 0) { throw "Original .NET scan failed for $serverCase source server" }
        $nativeEdgeJson = & $nativeExe --plan-json $serverOnlyProfile $serverSource $serverNativeTarget
        if ($LASTEXITCODE -ne 0) { throw "C++ scan failed for $serverCase source server" }
        $referenceEdge = $referenceEdgeJson | ConvertFrom-Json
        $nativeEdge = $nativeEdgeJson | ConvertFrom-Json
        $referenceEdgeItems = @($referenceEdge.operations)
        $nativeEdgeItems = @($nativeEdge.operations)
        $expectedCount = if ($serverCase -eq 'blank') { 1 } else { 3 }
        if (-not $referenceEdge.valid -or -not $nativeEdge.valid -or
            $referenceEdgeItems.Count -ne $expectedCount -or $nativeEdgeItems.Count -ne $expectedCount) {
            throw "$serverCase source server scan count or validity differs:`n.NET: $referenceEdgeJson`nC++: $nativeEdgeJson"
        }
        for ($index = 0; $index -lt $expectedCount; $index++) {
            foreach ($field in $fields) {
                if ($referenceEdgeItems[$index].$field -cne $nativeEdgeItems[$index].$field) {
                    throw "$serverCase source server item $index field '$field' differs:`n.NET: $referenceEdgeJson`nC++: $nativeEdgeJson"
                }
            }
        }
        $referenceEdgeResultJson = & dotnet $referenceExe --execute replace $serverOnlyProfile $serverSource $serverReferenceTarget
        if ($LASTEXITCODE -ne 0) { throw "Original .NET migration failed for $serverCase source server" }
        $nativeEdgeResultJson = & $nativeMigrationExe replace $serverOnlyProfile $serverSource $serverNativeTarget
        if ($LASTEXITCODE -ne 0) { throw "C++ migration failed for $serverCase source server" }
        $referenceEdgeResult = $referenceEdgeResultJson | ConvertFrom-Json
        $nativeEdgeResult = $nativeEdgeResultJson | ConvertFrom-Json
        foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
            if ($referenceEdgeResult.$field -ne $nativeEdgeResult.$field) {
                throw "$serverCase source server migration summary '$field' differs:`n.NET: $referenceEdgeResultJson`nC++: $nativeEdgeResultJson"
            }
        }
        if (@($referenceEdgeResult.items).Count -ne 1 -or @($nativeEdgeResult.items).Count -ne 1 -or
            $referenceEdgeResult.items[0].status -cne $nativeEdgeResult.items[0].status) {
            throw "$serverCase source server migration item status differs:`n.NET: $referenceEdgeResultJson`nC++: $nativeEdgeResultJson"
        }
        $referenceOutput = Join-Path $serverReferenceTarget 'servers.dat'
        $nativeOutput = Join-Path $serverNativeTarget 'servers.dat'
        if ((Test-Path -LiteralPath $referenceOutput) -ne (Test-Path -LiteralPath $nativeOutput)) {
            throw "$serverCase source server destination file presence differs"
        }
        if ($serverCase -eq 'blank') {
            if ($referenceEdgeResult.success -ne 1 -or $referenceEdgeResult.failed -ne 0 -or
                -not (Test-Path -LiteralPath $referenceOutput)) {
                throw 'Blank-address server was not migrated by the original application'
            }
            $referenceSnapshot = & $nativeMigrationExe --nbt-json $referenceOutput
            $nativeSnapshot = & $nativeMigrationExe --nbt-json $nativeOutput
            if ($referenceSnapshot -cne $nativeSnapshot) {
                throw "Blank-address server output differs:`n.NET: $referenceSnapshot`nC++: $nativeSnapshot"
            }
        } elseif ($referenceEdgeResult.failed -ne 1 -or (Test-Path -LiteralPath $referenceOutput)) {
            throw 'Duplicate source server was not rejected by the original application'
        }
        Write-Output "$serverCase source server: scan, migration result and destination file agree."
    }

    Write-Fixture 'options-only-profile.json' '{"schemaVersion":1,"name":"Option edge case","rules":[{"id":"bindings","name":"Bindings","kind":"builtin","builtin":"options","category":"bindings","source":"options.txt","target":"options.txt","defaultSelected":true}]}'
    Write-Fixture 'duplicate-options-source\options.txt' "key_key.forward:key.keyboard.w`n"
    $duplicateOptionsText = "key_key.forward:key.keyboard.up`nfov:70`nFOV:90`n"
    Write-Fixture 'duplicate-options-reference\options.txt' $duplicateOptionsText
    Write-Fixture 'duplicate-options-native\options.txt' $duplicateOptionsText
    $optionsOnlyProfile = Join-Path $fixture 'options-only-profile.json'
    $duplicateOptionsSource = Join-Path $fixture 'duplicate-options-source'
    $duplicateOptionsReference = Join-Path $fixture 'duplicate-options-reference'
    $duplicateOptionsNative = Join-Path $fixture 'duplicate-options-native'
    $referenceOptionPlanJson = & dotnet $referenceExe $optionsOnlyProfile $duplicateOptionsSource $duplicateOptionsReference
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET scan failed with duplicate target option keys' }
    $nativeOptionPlanJson = & $nativeExe --plan-json $optionsOnlyProfile $duplicateOptionsSource $duplicateOptionsNative
    if ($LASTEXITCODE -ne 0) { throw 'C++ scan failed with duplicate target option keys' }
    $referenceOptionItem = ($referenceOptionPlanJson | ConvertFrom-Json).operations[0]
    $nativeOptionItem = ($nativeOptionPlanJson | ConvertFrom-Json).operations[0]
    if ($referenceOptionItem.status -ne 'Conflict' -or $nativeOptionItem.status -ne 'Conflict' -or
        -not $referenceOptionItem.selected -or -not $nativeOptionItem.selected) {
        throw "Duplicate target option scan differs:`n.NET: $referenceOptionPlanJson`nC++: $nativeOptionPlanJson"
    }
    $referenceOptionResultJson = & dotnet $referenceExe --execute replace $optionsOnlyProfile $duplicateOptionsSource $duplicateOptionsReference
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET migration failed with duplicate target option keys' }
    $nativeOptionResultJson = & $nativeMigrationExe replace $optionsOnlyProfile $duplicateOptionsSource $duplicateOptionsNative
    if ($LASTEXITCODE -ne 0) { throw 'C++ migration failed with duplicate target option keys' }
    $referenceOptionResult = $referenceOptionResultJson | ConvertFrom-Json
    $nativeOptionResult = $nativeOptionResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($referenceOptionResult.$field -ne $nativeOptionResult.$field) {
            throw "Duplicate target option migration summary '$field' differs:`n.NET: $referenceOptionResultJson`nC++: $nativeOptionResultJson"
        }
    }
    if ($referenceOptionResult.failed -ne 1 -or
        $referenceOptionResult.items[0].status -ne 'Failed' -or
        $nativeOptionResult.items[0].status -ne 'Failed' -or
        [System.IO.File]::ReadAllText((Join-Path $duplicateOptionsReference 'options.txt')) -cne $duplicateOptionsText -or
        [System.IO.File]::ReadAllText((Join-Path $duplicateOptionsNative 'options.txt')) -cne $duplicateOptionsText) {
        throw "Duplicate target options were changed or misreported:`n.NET: $referenceOptionResultJson`nC++: $nativeOptionResultJson"
    }
    Write-Output 'Duplicate target option keys: both migrations fail without changing options.txt.'

    $unicodeUpperKey = 'key_mod.B' + [char]0x00DC + 'CHER'
    $unicodeLowerKey = 'key_mod.b' + [char]0x00FC + 'cher'
    Write-Fixture 'unicode-options-source\options.txt' "${unicodeUpperKey}:key.keyboard.w`n"
    Write-Fixture 'unicode-options-reference\options.txt' "${unicodeLowerKey}:key.keyboard.w`n"
    Write-Fixture 'unicode-options-native\options.txt' "${unicodeLowerKey}:key.keyboard.w`n"
    $unicodeOptionSource = Join-Path $fixture 'unicode-options-source'
    $unicodeOptionReference = Join-Path $fixture 'unicode-options-reference'
    $unicodeOptionNative = Join-Path $fixture 'unicode-options-native'
    $referenceSameOptionJson = & dotnet $referenceExe $optionsOnlyProfile $unicodeOptionSource $unicodeOptionReference
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET matching Unicode option scan failed' }
    $nativeSameOptionJson = & $nativeExe --plan-json $optionsOnlyProfile $unicodeOptionSource $unicodeOptionNative
    if ($LASTEXITCODE -ne 0) { throw 'C++ matching Unicode option scan failed' }
    if (($referenceSameOptionJson | ConvertFrom-Json).operations[0].status -ne 'Identical' -or
        ($nativeSameOptionJson | ConvertFrom-Json).operations[0].status -ne 'Identical') {
        throw "Matching Unicode-case option keys differ:`n.NET: $referenceSameOptionJson`nC++: $nativeSameOptionJson"
    }
    Write-Fixture 'unicode-options-source\options.txt' "${unicodeUpperKey}:key.keyboard.x`n"
    $referenceChangedOptionJson = & dotnet $referenceExe $optionsOnlyProfile $unicodeOptionSource $unicodeOptionReference
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET changed Unicode option scan failed' }
    $nativeChangedOptionJson = & $nativeExe --plan-json $optionsOnlyProfile $unicodeOptionSource $unicodeOptionNative
    if ($LASTEXITCODE -ne 0) { throw 'C++ changed Unicode option scan failed' }
    if (($referenceChangedOptionJson | ConvertFrom-Json).operations[0].status -ne 'Conflict' -or
        ($nativeChangedOptionJson | ConvertFrom-Json).operations[0].status -ne 'Conflict') {
        throw "Changed Unicode-case option keys differ:`n.NET: $referenceChangedOptionJson`nC++: $nativeChangedOptionJson"
    }
    $referenceUnicodeResultJson = & dotnet $referenceExe --execute replace $optionsOnlyProfile $unicodeOptionSource $unicodeOptionReference
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET Unicode option migration failed' }
    $nativeUnicodeResultJson = & $nativeMigrationExe replace $optionsOnlyProfile $unicodeOptionSource $unicodeOptionNative
    if ($LASTEXITCODE -ne 0) { throw 'C++ Unicode option migration failed' }
    $referenceUnicodeResult = $referenceUnicodeResultJson | ConvertFrom-Json
    $nativeUnicodeResult = $nativeUnicodeResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($referenceUnicodeResult.$field -ne $nativeUnicodeResult.$field) {
            throw "Unicode option migration summary '$field' differs:`n.NET: $referenceUnicodeResultJson`nC++: $nativeUnicodeResultJson"
        }
    }
    $expectedUnicodeOptions = "${unicodeUpperKey}:key.keyboard.x`r`n"
    foreach ($outputFile in @((Join-Path $unicodeOptionReference 'options.txt'),
                               (Join-Path $unicodeOptionNative 'options.txt'))) {
        if ([System.IO.File]::ReadAllText($outputFile) -cne $expectedUnicodeOptions) {
            throw "Unicode option migration did not replace the existing key: $outputFile"
        }
    }
    Write-Output 'Unicode-case key binding: matching scan, changed scan and merged options.txt agree.'

    function Compare-OptionBytes([string]$name, [byte[]]$sourceBytes, [byte[]]$targetBytes) {
        $sourceFolder = Join-Path $fixture "option-variant-$name-source"
        $referenceFolder = Join-Path $fixture "option-variant-$name-reference"
        $nativeFolder = Join-Path $fixture "option-variant-$name-native"
        foreach ($folder in @($sourceFolder, $referenceFolder, $nativeFolder)) {
            New-Item -ItemType Directory -Path $folder -Force | Out-Null
        }
        [System.IO.File]::WriteAllBytes((Join-Path $sourceFolder 'options.txt'), $sourceBytes)
        [System.IO.File]::WriteAllBytes((Join-Path $referenceFolder 'options.txt'), $targetBytes)
        [System.IO.File]::WriteAllBytes((Join-Path $nativeFolder 'options.txt'), $targetBytes)
        $referenceScanJson = & dotnet $referenceExe $optionsOnlyProfile $sourceFolder $referenceFolder
        if ($LASTEXITCODE -ne 0) { throw "Original .NET option variant scan failed: $name" }
        $nativeScanJson = & $nativeExe --plan-json $optionsOnlyProfile $sourceFolder $nativeFolder
        if ($LASTEXITCODE -ne 0) { throw "C++ option variant scan failed: $name" }
        $referenceScan = $referenceScanJson | ConvertFrom-Json
        $nativeScan = $nativeScanJson | ConvertFrom-Json
        foreach ($field in @('valid', 'error')) {
            if ($referenceScan.$field -cne $nativeScan.$field) {
                throw "Option variant $name scan '$field' differs:`n.NET: $referenceScanJson`nC++: $nativeScanJson"
            }
        }
        $referenceItem = @($referenceScan.operations)[0]
        $nativeItem = @($nativeScan.operations)[0]
        foreach ($field in @('status', 'selected', 'size')) {
            if ($referenceItem.$field -cne $nativeItem.$field) {
                throw "Option variant $name item '$field' differs:`n.NET: $referenceScanJson`nC++: $nativeScanJson"
            }
        }
        $referenceResultJson = & dotnet $referenceExe --execute replace $optionsOnlyProfile $sourceFolder $referenceFolder
        if ($LASTEXITCODE -ne 0) { throw "Original .NET option variant migration failed: $name" }
        $nativeResultJson = & $nativeMigrationExe replace $optionsOnlyProfile $sourceFolder $nativeFolder
        if ($LASTEXITCODE -ne 0) { throw "C++ option variant migration failed: $name" }
        $referenceResult = $referenceResultJson | ConvertFrom-Json
        $nativeResult = $nativeResultJson | ConvertFrom-Json
        foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
            if ($referenceResult.$field -cne $nativeResult.$field) {
                throw "Option variant $name migration '$field' differs:`n.NET: $referenceResultJson`nC++: $nativeResultJson"
            }
        }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $referenceFolder 'options.txt')).Hash -cne
            (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $nativeFolder 'options.txt')).Hash) {
            throw "Option variant $name destination bytes differ"
        }
        Write-Output "Option variant $name`: scan, migration summary and output bytes agree."
    }

    $bom = [byte[]]@(0xEF, 0xBB, 0xBF)
    Compare-OptionBytes 'bom-mixed-endings' `
        ([byte[]]($bom + $utf8.GetBytes("key_key.forward:key.keyboard.w`r`nkey_key.jump:key.keyboard.space`n"))) `
        ([byte[]]($bom + $utf8.GetBytes("key_key.forward:key.keyboard.up`n")))
    Compare-OptionBytes 'carriage-return-only' `
        ($utf8.GetBytes("key_key.forward:key.keyboard.w`rkey_key.jump:key.keyboard.space`r")) `
        ($utf8.GetBytes("key_key.forward:key.keyboard.up`r"))
    $invalidSource = [System.Text.Encoding]::ASCII.GetBytes("key_mod.X:key.keyboard.w`n")
    $invalidTarget = [System.Text.Encoding]::ASCII.GetBytes("key_mod.X:key.keyboard.up`n")
    $invalidSource[8] = 0xFF
    $invalidTarget[8] = 0xFF
    Compare-OptionBytes 'invalid-utf8-key' $invalidSource $invalidTarget
    Compare-OptionBytes 'duplicate-source-key' `
        ($utf8.GetBytes("key_key.forward:key.keyboard.w`nkey_key.forward:key.keyboard.x`n")) `
        ($utf8.GetBytes("key_key.forward:key.keyboard.up`n"))
    $utf16le = [System.Text.Encoding]::Unicode
    Compare-OptionBytes 'utf16-le-bom' `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16le.GetBytes("key_key.forward:key.keyboard.w`r`n"))) `
        ([byte[]]([byte[]]@(0xFF, 0xFE) + $utf16le.GetBytes("key_key.forward:key.keyboard.up`r`n")))
    $utf16be = [System.Text.Encoding]::BigEndianUnicode
    Compare-OptionBytes 'utf16-be-bom' `
        ([byte[]]([byte[]]@(0xFE, 0xFF) + $utf16be.GetBytes("key_key.forward:key.keyboard.w`r`n"))) `
        ([byte[]]([byte[]]@(0xFE, 0xFF) + $utf16be.GetBytes("key_key.forward:key.keyboard.up`r`n")))
    $utf32le = [System.Text.UTF32Encoding]::new($false, $true)
    Compare-OptionBytes 'utf32-le-bom' `
        ([byte[]]($utf32le.GetPreamble() + $utf32le.GetBytes("key_key.forward:key.keyboard.w`r`n"))) `
        ([byte[]]($utf32le.GetPreamble() + $utf32le.GetBytes("key_key.forward:key.keyboard.up`r`n")))
    $utf32be = [System.Text.UTF32Encoding]::new($true, $true)
    Compare-OptionBytes 'utf32-be-bom' `
        ([byte[]]($utf32be.GetPreamble() + $utf32be.GetBytes("key_key.forward:key.keyboard.w`r`n"))) `
        ([byte[]]($utf32be.GetPreamble() + $utf32be.GetBytes("key_key.forward:key.keyboard.up`r`n")))

    foreach ($topOnlyPath in @((Join-Path $source 'top-only'), (Join-Path $target 'top-only'))) {
        $resolvedTopOnlyPath = [System.IO.Path]::GetFullPath($topOnlyPath)
        $allowedFixturePrefix = [System.IO.Path]::GetFullPath($fixture).TrimEnd('\') + '\'
        if (-not $resolvedTopOnlyPath.StartsWith($allowedFixturePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            [System.IO.Path]::GetFileName($resolvedTopOnlyPath) -cne 'top-only') {
            throw "Refusing to remove unexpected non-recursive fixture path: $resolvedTopOnlyPath"
        }
        if (Test-Path -LiteralPath $resolvedTopOnlyPath) {
            Remove-Item -LiteralPath $resolvedTopOnlyPath -Recurse -Force
        }
    }

    $defaultProfile = Join-Path $projectRoot 'profiles\vanilla.json'
    $defaultRules = @((Get-Content -LiteralPath $defaultProfile -Raw -Encoding UTF8 | ConvertFrom-Json).rules)
    foreach ($rule in $defaultRules) {
        if ($rule.kind -eq 'directory') {
            Write-Fixture "source\$($rule.source)\state.txt" "source-$($rule.id)"
            Write-Fixture "target\$($rule.target)\state.txt" "target-$($rule.id)"
            Write-Fixture "source\$($rule.source)\nested\extra.txt" "nested-$($rule.id)"
        } elseif ($rule.kind -eq 'file') {
            $extension = [System.IO.Path]::GetExtension($rule.source).ToLowerInvariant()
            if ($extension -eq '.json' -or $extension -eq '.json5') {
                $sourceContent = '{"value":"source-' + $rule.id + '"}'
                $targetContent = '{"value":"target-' + $rule.id + '"}'
            } else {
                $sourceContent = "value=source-$($rule.id)`n"
                $targetContent = "value=target-$($rule.id)`n"
            }
            Write-Fixture "source\$($rule.source)" $sourceContent
            Write-Fixture "target\$($rule.target)" $targetContent
        }
    }
    $largeSourceFile = Join-Path $source 'schematics\large.litematic'
    $largeTargetFile = Join-Path $target 'schematics\large.litematic'
    $largePayload = [byte[]]::new(4 * 1024 * 1024 + 1)
    $largePayload[0] = 0x53
    [System.IO.File]::WriteAllBytes($largeSourceFile, $largePayload)
    $largePayload[$largePayload.Length - 1] = 0x7f
    [System.IO.File]::WriteAllBytes($largeTargetFile, $largePayload)
    $largeSourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $largeSourceFile).Hash
    $largeTargetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $largeTargetFile).Hash
    $referenceDefaultJson = & dotnet $referenceExe $defaultProfile $source $target
    if ($LASTEXITCODE -ne 0) { throw 'Original .NET default-profile scan failed' }
    $nativeDefaultJson = & $nativeExe --plan-json $defaultProfile $source $target
    if ($LASTEXITCODE -ne 0) { throw 'C++ default-profile scan failed' }
    $referenceDefault = $referenceDefaultJson | ConvertFrom-Json
    $nativeDefault = $nativeDefaultJson | ConvertFrom-Json
    $referenceDefaultItems = @($referenceDefault.operations)
    $nativeDefaultItems = @($nativeDefault.operations)
    if (-not $referenceDefault.valid -or -not $nativeDefault.valid -or
        $referenceDefaultItems.Count -ne $nativeDefaultItems.Count -or
        $referenceDefaultItems.Count -le $defaultRules.Count) {
        throw "Default-profile scan validity or item count differs:`n.NET: $referenceDefaultJson`nC++: $nativeDefaultJson"
    }
    for ($index = 0; $index -lt $referenceDefaultItems.Count; $index++) {
        foreach ($field in $fields) {
            if ($referenceDefaultItems[$index].$field -cne $nativeDefaultItems[$index].$field) {
                throw "Default-profile item $index field '$field' differs:`n.NET: $referenceDefaultJson`nC++: $nativeDefaultJson"
            }
        }
    }
    $schematicItems = @($nativeDefaultItems | Where-Object { $_.id -eq 'sempervirens_schematics' })
    if ($schematicItems.Count -ne 1 -or $schematicItems[0].status -ne 'Conflict' -or
        $largeSourceHash -eq $largeTargetHash) {
        throw 'The multi-megabyte schematic fixture was not recognized as changed'
    }
    Write-Output "Default profile: $($defaultRules.Count) rules produce $($referenceDefaultItems.Count) matching scan items."

    foreach ($defaultStrategy in @('backup', 'replace', 'skip')) {
    $defaultReferenceTarget = Join-Path $fixture "default-$defaultStrategy-reference-target"
    $defaultNativeTarget = Join-Path $fixture "default-$defaultStrategy-native-target"
    Copy-Item -LiteralPath $target -Destination $defaultReferenceTarget -Recurse
    Copy-Item -LiteralPath $target -Destination $defaultNativeTarget -Recurse
    $referenceDefaultResultJson = & dotnet $referenceExe --execute $defaultStrategy $defaultProfile $source $defaultReferenceTarget
    if ($LASTEXITCODE -ne 0) { throw "Original .NET default-profile $defaultStrategy migration failed" }
    $nativeDefaultResultJson = & $nativeMigrationExe $defaultStrategy $defaultProfile $source $defaultNativeTarget
    if ($LASTEXITCODE -ne 0) { throw "C++ default-profile $defaultStrategy migration failed" }
    $referenceDefaultResult = $referenceDefaultResultJson | ConvertFrom-Json
    $nativeDefaultResult = $nativeDefaultResultJson | ConvertFrom-Json
    foreach ($field in @('success', 'skipped', 'failed', 'copied', 'overwritten', 'backedUp', 'filesFailed')) {
        if ($referenceDefaultResult.$field -ne $nativeDefaultResult.$field) {
            throw "Default-profile migration summary '$field' differs:`n.NET: $referenceDefaultResultJson`nC++: $nativeDefaultResultJson"
        }
    }
    $referenceDefaultResults = @($referenceDefaultResult.items)
    $nativeDefaultResults = @($nativeDefaultResult.items)
    if ($referenceDefaultResults.Count -ne $nativeDefaultResults.Count) {
        throw 'Default-profile migration item count differs'
    }
    for ($index = 0; $index -lt $referenceDefaultResults.Count; $index++) {
        foreach ($field in @('name', 'target', 'status', 'copied', 'overwritten', 'backedUp')) {
            if ($referenceDefaultResults[$index].$field -cne $nativeDefaultResults[$index].$field) {
                throw "Default-profile result $index field '$field' differs:`n.NET: $referenceDefaultResultJson`nC++: $nativeDefaultResultJson"
            }
        }
    }
    $referenceDefaultFiles = @(Get-MigratedFiles $defaultReferenceTarget)
    $nativeDefaultFiles = @(Get-MigratedFiles $defaultNativeTarget)
    if ($referenceDefaultFiles.Count -ne $nativeDefaultFiles.Count) {
        throw "Default-profile target file count differs: .NET=$($referenceDefaultFiles.Count), C++=$($nativeDefaultFiles.Count)"
    }
    for ($index = 0; $index -lt $referenceDefaultFiles.Count; $index++) {
        if ($referenceDefaultFiles[$index] -cne $nativeDefaultFiles[$index]) {
            throw "Default-profile target file differs: .NET='$($referenceDefaultFiles[$index])', C++='$($nativeDefaultFiles[$index])'"
        }
    }
    $expectedLargeHash = if ($defaultStrategy -eq 'skip') { $largeTargetHash } else { $largeSourceHash }
    foreach ($largeOutput in @((Join-Path $defaultReferenceTarget 'schematics\large.litematic'),
                                (Join-Path $defaultNativeTarget 'schematics\large.litematic'))) {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $largeOutput).Hash -ne $expectedLargeHash) {
            throw "Default-profile $defaultStrategy did not apply the selected policy to the large schematic"
        }
    }
    $referenceDefaultServers = & $nativeMigrationExe --nbt-json (Join-Path $defaultReferenceTarget 'servers.dat')
    $nativeDefaultServers = & $nativeMigrationExe --nbt-json (Join-Path $defaultNativeTarget 'servers.dat')
    if ($referenceDefaultServers -cne $nativeDefaultServers) {
        throw "Default-profile server list differs:`n.NET: $referenceDefaultServers`nC++: $nativeDefaultServers"
    }
    $referenceDefaultBackups = @(Get-BackupFiles $defaultReferenceTarget)
    $nativeDefaultBackups = @(Get-BackupFiles $defaultNativeTarget)
    if ($defaultStrategy -eq 'backup' -and
        -not @($nativeDefaultBackups | Where-Object { $_ -eq "schematics/large.litematic|$largeTargetHash" }).Count) {
        throw 'Default-profile backup did not preserve the previous large schematic'
    }
    if ($referenceDefaultBackups.Count -ne $nativeDefaultBackups.Count) {
        throw "Default-profile backup count differs: .NET=$($referenceDefaultBackups.Count), C++=$($nativeDefaultBackups.Count)"
    }
    for ($index = 0; $index -lt $referenceDefaultBackups.Count; $index++) {
        if ($referenceDefaultBackups[$index] -cne $nativeDefaultBackups[$index]) {
            throw "Default-profile backup differs: .NET='$($referenceDefaultBackups[$index])', C++='$($nativeDefaultBackups[$index])'"
        }
    }
    $referenceDefaultReport = Get-NormalizedReport $defaultReferenceTarget
    $nativeDefaultReport = Get-NormalizedReport $defaultNativeTarget
    if ($referenceDefaultReport -cne $nativeDefaultReport) {
        throw "Default-profile report differs:`n.NET:`n$referenceDefaultReport`nC++:`n$nativeDefaultReport"
    }
    Write-Output "Default profile $defaultStrategy migration: $($referenceDefaultResults.Count) item results, $($referenceDefaultFiles.Count) ordinary files, server NBT, $($referenceDefaultBackups.Count) backups and report agree."
    }
}
finally {
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
    $allowedPrefix = [System.IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
    if (-not $resolvedFixture.StartsWith($allowedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not [System.IO.Path]::GetFileName($resolvedFixture).StartsWith('scan-parity-', [System.StringComparison]::Ordinal)) {
        throw "Refusing to remove unexpected test path: $resolvedFixture"
    }
    if (Test-Path -LiteralPath $resolvedFixture) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
