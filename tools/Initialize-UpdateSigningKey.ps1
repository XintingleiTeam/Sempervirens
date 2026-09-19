[CmdletBinding()]
param(
    [string]$KeyPath = '',
    [string]$HeaderPath = '',
    [string]$ActionsSecretPath = ''
)
$ErrorActionPreference = 'Stop'
if (-not $KeyPath) { $KeyPath = Join-Path $PSScriptRoot '..\.private\update-signing-key.bin' }
if (-not $HeaderPath) { $HeaderPath = Join-Path $PSScriptRoot '..\src\update_public_key.hpp' }
if (-not $ActionsSecretPath) { $ActionsSecretPath = Join-Path $PSScriptRoot '..\.private\github-actions-key.txt' }
if (Test-Path -LiteralPath $KeyPath) { throw 'A signing key already exists. Refusing to replace it.' }
Add-Type -AssemblyName System.Security
$key = [Security.Cryptography.ECDsaCng]::new(256)
$key.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
try {
    $parameters = $key.ExportParameters($true)
    $json = @{ X=[Convert]::ToBase64String($parameters.Q.X); Y=[Convert]::ToBase64String($parameters.Q.Y); D=[Convert]::ToBase64String($parameters.D) } | ConvertTo-Json -Compress
    $plain = [Text.Encoding]::UTF8.GetBytes($json)
    $entropy = [Text.Encoding]::UTF8.GetBytes('Sempervirens update signing key v1')
    $protected = [Security.Cryptography.ProtectedData]::Protect($plain,$entropy,[Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($KeyPath))) | Out-Null
    [IO.File]::WriteAllBytes([IO.Path]::GetFullPath($KeyPath),$protected)
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($ActionsSecretPath),
        [Convert]::ToBase64String($plain),[Text.UTF8Encoding]::new($false))
    function Format-Bytes([byte[]]$bytes) { ($bytes | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', ' }
    $header = @"
#pragma once
#include <array>
#include <cstdint>

namespace sempervirens::update {
// Public half of the offline release key. The DPAPI-protected private half is
// excluded from source and never copied to the update server.
inline constexpr std::array<std::uint8_t, 32> signing_public_x{{$(Format-Bytes $parameters.Q.X)}};
inline constexpr std::array<std::uint8_t, 32> signing_public_y{{$(Format-Bytes $parameters.Q.Y)}};
} // namespace sempervirens::update
"@
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($HeaderPath),$header,[Text.UTF8Encoding]::new($false))
    Write-Host "Created protected signing key: $([IO.Path]::GetFullPath($KeyPath))"
    Write-Host "Updated embedded public key: $([IO.Path]::GetFullPath($HeaderPath))"
    Write-Host "GitHub Actions secret value was written to: $([IO.Path]::GetFullPath($ActionsSecretPath))"
    Write-Host 'Store it as SEMPERVIRENS_UPDATE_SIGNING_KEY_BASE64, then remove the plaintext export.'
} finally { $key.Dispose() }
