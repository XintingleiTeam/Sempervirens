param([string]$Path = (Join-Path $PSScriptRoot 'build\test\Sempervirens.exe'))

$ErrorActionPreference = 'Stop'
$file = (Resolve-Path -LiteralPath $Path).Path
$bytes = [System.IO.File]::ReadAllBytes($file)

function Read-U16([int]$offset) {
    if ($offset -lt 0 -or $offset + 2 -gt $bytes.Length) { throw 'PE header is truncated.' }
    return [System.BitConverter]::ToUInt16($bytes, $offset)
}
function Read-U32([int]$offset) {
    if ($offset -lt 0 -or $offset + 4 -gt $bytes.Length) { throw 'PE header is truncated.' }
    return [System.BitConverter]::ToUInt32($bytes, $offset)
}
function Read-U64([int]$offset) {
    if ($offset -lt 0 -or $offset + 8 -gt $bytes.Length) { throw 'PE header is truncated.' }
    return [System.BitConverter]::ToUInt64($bytes, $offset)
}
function Read-Ascii([int]$offset) {
    $end = $offset
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0 -and $end - $offset -lt 260) { $end++ }
    if ($end -ge $bytes.Length -or $end - $offset -ge 260) { throw 'PE import name is malformed.' }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $offset, $end - $offset)
}

if ($bytes.Length -lt 128 -or (Read-U16 0) -ne 0x5A4D) { throw 'This is not an MZ executable.' }
$pe = [int](Read-U32 0x3C)
if ((Read-U32 $pe) -ne 0x00004550) { throw 'The PE signature is missing.' }
$sectionsCount = Read-U16 ($pe + 6)
$optionalSize = Read-U16 ($pe + 20)
$optional = $pe + 24
$magic = Read-U16 $optional
if ($magic -ne 0x20B) { throw 'Expected a 64-bit native Windows executable.' }
$subsystem = Read-U16 ($optional + 68)
if ($subsystem -ne 2) { throw "Expected Windows GUI subsystem; found $subsystem." }
$directoryBase = $optional + 0x70
$importRva = Read-U32 ($directoryBase + 8)
$resourceRva = Read-U32 ($directoryBase + 2 * 8)
$clrRva = Read-U32 ($directoryBase + 14 * 8)
if ($clrRva -ne 0) { throw 'The executable contains a .NET CLR directory.' }
if ($importRva -eq 0) { throw 'The executable has no import directory.' }

$sections = @()
$sectionBase = $optional + $optionalSize
for ($index = 0; $index -lt $sectionsCount; $index++) {
    $offset = $sectionBase + $index * 40
    $sections += [pscustomobject]@{
        VirtualSize = Read-U32 ($offset + 8)
        VirtualAddress = Read-U32 ($offset + 12)
        RawSize = Read-U32 ($offset + 16)
        RawAddress = Read-U32 ($offset + 20)
    }
}
function Rva-ToOffset([uint32]$rva) {
    foreach ($section in $sections) {
        $extent = [Math]::Max($section.VirtualSize, $section.RawSize)
        if ($rva -ge $section.VirtualAddress -and $rva -lt $section.VirtualAddress + $extent) {
            $offset = [int]($section.RawAddress + $rva - $section.VirtualAddress)
            if ($offset -ge 0 -and $offset -lt $bytes.Length) { return $offset }
        }
    }
    throw "PE RVA $rva does not map to a file section."
}

$imports = [System.Collections.Generic.List[string]]::new()
$importSymbols = [System.Collections.Generic.List[string]]::new()
$descriptor = Rva-ToOffset $importRva
for ($index = 0; $index -lt 256; $index++) {
    $offset = $descriptor + $index * 20
    $nameRva = Read-U32 ($offset + 12)
    if ($nameRva -eq 0) { break }
    $library = Read-Ascii (Rva-ToOffset $nameRva)
    $imports.Add($library)
    $lookupRva = Read-U32 $offset
    if ($lookupRva -eq 0) { $lookupRva = Read-U32 ($offset + 16) }
    if ($lookupRva -eq 0) { continue }
    $lookup = Rva-ToOffset $lookupRva
    for ($symbolIndex = 0; $symbolIndex -lt 8192; $symbolIndex++) {
        $entry = Read-U64 ($lookup + $symbolIndex * 8)
        if ($entry -eq 0) { break }
        if (($entry -shr 63) -ne 0) { continue }
        if ($entry -gt [uint32]::MaxValue) { throw 'PE import name RVA is out of range.' }
        $symbol = Read-Ascii ((Rva-ToOffset ([uint32]$entry)) + 2)
        $importSymbols.Add("$library!$symbol")
    }
}
if ($imports.Count -eq 0) { throw 'No imported Windows libraries were found.' }

if ($resourceRva -eq 0) { throw 'The executable has no resource directory.' }
$resourceRoot = Rva-ToOffset $resourceRva
$resourceNamedCount = Read-U16 ($resourceRoot + 12)
$resourceIdCount = Read-U16 ($resourceRoot + 14)
$resourceTypes = @()
for ($index = 0; $index -lt $resourceNamedCount + $resourceIdCount; $index++) {
    $name = Read-U32 ($resourceRoot + 16 + $index * 8)
    if (($name -band 0x80000000) -eq 0) { $resourceTypes += [int]($name -band 0xFFFF) }
}
if ($resourceTypes -notcontains 3 -or $resourceTypes -notcontains 14) {
    throw 'The executable does not contain both icon image and icon group resources.'
}
$version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($file)
if ($version.ProductName -ne 'Sempervirens' -or
    $version.OriginalFilename -ne 'Sempervirens.exe' -or
    $version.FileDescription -ne 'Sempervirens') {
    throw 'The executable identity metadata is incomplete or incorrect.'
}
$nonSystem = @($imports | Where-Object {
    $_ -notmatch '^(KERNEL32|USER32|GDI32|D2D1|DWRITE|DWMAPI|OLE32|OLEAUT32|OLEACC|SHELL32|WINDOWSCODECS|ADVAPI32|COMDLG32|SHLWAPI|COMCTL32|IMM32|MSVCRT|UCRTBASE|NTDLL|VERSION|RPCRT4|SETUPAPI|CRYPT32|BCRYPT|WINHTTP|WS2_32|api-ms-win-[a-z0-9-]+|ext-ms-win-[a-z0-9-]+)\.dll$'
})
if ($nonSystem.Count -gt 0) { throw "Unexpected non-system runtime imports: $($nonSystem -join ', ')" }
$lateUser32Imports = @(
    'USER32.dll!AdjustWindowRectExForDpi',
    'USER32.dll!GetDpiForWindow',
    'USER32.dll!SetProcessDpiAwarenessContext'
)
$foundLateImports = @($lateUser32Imports | Where-Object { $importSymbols -contains $_ })
if ($foundLateImports.Count -gt 0) {
    throw "The Windows 10 base-compatible executable directly imports later DPI APIs: $($foundLateImports -join ', ')"
}
$binaryText = [System.Text.Encoding]::UTF8.GetString($bytes)
if (-not $binaryText.Contains('<longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>')) {
    throw 'The executable manifest does not enable Windows long-path awareness.'
}
if (-not $binaryText.Contains('<requestedExecutionLevel level="asInvoker" uiAccess="false"')) {
    throw 'The executable manifest does not declare standard-user execution.'
}
Write-Output "Native x64 Windows GUI verified: $file"
Write-Output "Windows libraries: $($imports -join ', ')"
Write-Output 'Windows 10 base compatibility verified: later DPI APIs are resolved dynamically.'
Write-Output 'Embedded manifest verified: standard-user execution and long-path awareness are enabled.'
Write-Output 'Application identity verified: product metadata and dedicated icon resources are embedded.'
