[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CurrentApp,
    [string]$PreviousApp,
    [string]$PreviousVersion,
    [string]$Version = '0.1.0.0',
    [string]$Channel = 'stable',
    [string[]]$AssetUrlRoots = @(
        'https://github.com/XintingleiTeam/sempervirens/releases/download/v{version}',
        'https://xintinglei.cn/api/sempervirens/update/releases/{version}'
    ),
    [string]$OutputRoot = '',
    [string]$KeyPath = '',
    [string]$KeyMaterialBase64 = $env:SEMPERVIRENS_UPDATE_SIGNING_KEY_BASE64,
    [string]$PythonPath = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutputRoot) { $OutputRoot = Join-Path $PSScriptRoot '..\artifacts\update-site' }
if (-not $KeyPath) { $KeyPath = Join-Path $PSScriptRoot '..\.private\update-signing-key.bin' }
if (-not $KeyMaterialBase64 -and -not (Test-Path -LiteralPath $KeyPath -PathType Leaf)) { throw 'Initialize the offline update signing key first.' }
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'The public version must use four numeric components.' }
if ($AssetUrlRoots.Count -lt 2) { throw 'Both GitHub and domestic update mirrors are required.' }
$resolvedAssetRoots = @($AssetUrlRoots | ForEach-Object { $_.Replace('{version}', $Version).TrimEnd('/') })
$current = (Resolve-Path -LiteralPath $CurrentApp).Path
$release = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('releases\' + $Version)
$channels = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) 'channels'
[IO.Directory]::CreateDirectory($release) | Out-Null
[IO.Directory]::CreateDirectory($channels) | Out-Null
$full = Join-Path $release 'SempervirensApp.exe'
Copy-Item -LiteralPath $current -Destination $full -Force
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$packages = @()
if ($PreviousApp) {
    if ($PreviousVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'PreviousVersion is required and must use four numeric components.' }
    $patchName = "$PreviousVersion-to-$Version.svdelta"
    $patch = Join-Path $release $patchName
    if (-not $PythonPath) {
        $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
        if (-not $pythonCommand) { throw 'Python 3 was not found. Pass -PythonPath explicitly.' }
        $PythonPath = $pythonCommand.Source
    }
    & $PythonPath (Join-Path $PSScriptRoot 'Create-Delta.py') (Resolve-Path -LiteralPath $PreviousApp).Path $current $patch
    if ($LASTEXITCODE -ne 0) { throw 'Delta generation failed.' }
    $deltaUrls = @($resolvedAssetRoots | ForEach-Object { "$_/$patchName" })
    $packages += [ordered]@{ kind='delta'; fromVersion=$PreviousVersion; fromSha256=(Hash $PreviousApp); urls=$deltaUrls; size=(Get-Item $patch).Length; sha256=(Hash $patch) }
}
$fullUrls = @($resolvedAssetRoots | ForEach-Object { "$_/SempervirensApp.exe" })
$packages += [ordered]@{ kind='full'; urls=$fullUrls; size=(Get-Item $full).Length; sha256=(Hash $full) }
$manifestObject = [ordered]@{
    schema=1; product='sempervirens-windows-x64'; channel=$Channel; version=$Version
    serial=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds(); minimumUpdaterProtocol=1
    publishedUtc=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    notes=[ordered]@{ 'zh-CN'="Sempervirens $Version 更新"; en="Sempervirens $Version update" }
    outputSize=(Get-Item $full).Length; outputSha256=(Hash $full); packages=$packages
}
$manifestPath = Join-Path $channels ($Channel + '.json')
$manifest = $manifestObject | ConvertTo-Json -Depth 8 -Compress
[IO.File]::WriteAllText($manifestPath,$manifest,[Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.Security
if ($KeyMaterialBase64) {
    $plain = [Convert]::FromBase64String($KeyMaterialBase64)
} else {
    $protected = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $KeyPath).Path)
    $entropy = [Text.Encoding]::UTF8.GetBytes('Sempervirens update signing key v1')
    $plain = [Security.Cryptography.ProtectedData]::Unprotect($protected,$entropy,[Security.Cryptography.DataProtectionScope]::CurrentUser)
}
$material = [Text.Encoding]::UTF8.GetString($plain) | ConvertFrom-Json
$x = [Convert]::FromBase64String($material.X)
$y = [Convert]::FromBase64String($material.Y)
$d = [Convert]::FromBase64String($material.D)
if ($x.Length -ne 32 -or $y.Length -ne 32 -or $d.Length -ne 32) { throw 'The P-256 signing key is malformed.' }
$blob = [byte[]]::new(104)
[BitConverter]::GetBytes([uint32]0x32534345).CopyTo($blob,0) # BCRYPT_ECDSA_PRIVATE_P256_MAGIC
[BitConverter]::GetBytes([uint32]32).CopyTo($blob,4)
$x.CopyTo($blob,8); $y.CopyTo($blob,40); $d.CopyTo($blob,72)
$cngKey = [Security.Cryptography.CngKey]::Import($blob,[Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$key = [Security.Cryptography.ECDsaCng]::new($cngKey)
$key.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
try { $signature = $key.SignData([IO.File]::ReadAllBytes($manifestPath),[Security.Cryptography.HashAlgorithmName]::SHA256) }
finally { $key.Dispose(); $cngKey.Dispose() }
if ($signature.Length -ne 64) { throw 'Expected an IEEE P1363 ECDSA signature.' }
[IO.File]::WriteAllBytes(($manifestPath + '.sig'),$signature)
Write-Host "Published signed $Channel channel under $([IO.Path]::GetFullPath($OutputRoot))"
