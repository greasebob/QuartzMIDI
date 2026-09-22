# Signs an add-on folder for engine/AddonHost.hpp: writes addon.json (including
# the DLL's SHA-256) and addon.sig (ECDSA P-256 over addon.json's bytes).
#
#   tools\sign-addon.ps1 -Folder build\release\QuartzMIDI\addons\sheets -Name 'Sheets' -Dll sheets.dll -Key <key file>
#   tools\sign-addon.ps1 -NewKey -Key <key file>      creates a key; never overwrites one
#   tools\sign-addon.ps1 -PublicKey -Key <key file>   prints the public key as a C++ declaration
#   tools\sign-addon.ps1 -Header <file> -Key <key file>   writes that declaration to a header if it differs
#
# Builds pass -MakeKey, which creates build\addon-key\key.bin if missing, so each
# local build signs its add-ons with its own key and its app trusts that key.
# Key files must never be committed. -For and -Ends add a licensee name and an
# expiry date (YYYY-MM-DD) to the signed manifest.

param(
    [string] $Folder, [string] $Name, [string] $Dll, [Parameter(Mandatory)] [string] $Key,
    [string] $For = '', [string] $Ends = '', [int] $Abi = 1,
    [string] $Header, [switch] $NewKey, [switch] $PublicKey, [switch] $MakeKey
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Core

function Get-PublicBlob([System.Security.Cryptography.CngKey] $cngKey) {
    $blob = $cngKey.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    'inline constexpr addon::PublicKey kOwnerKey{' + (($blob | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', ') + '};'
}

if ($MakeKey -and -not (Test-Path -LiteralPath $Key)) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Key) -Force | Out-Null
    $NewKey = $true
} elseif ($MakeKey) { $NewKey = $false }
if ($NewKey) {
    if (Test-Path -LiteralPath $Key) { throw "$Key is already there. A new key would refuse every add-on signed with it." }
    $parameters = New-Object System.Security.Cryptography.CngKeyCreationParameters
    $parameters.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
    # PowerShell passes $null to a string parameter as "", which would create a named key persisted in the user's store.
    $made = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $parameters)
    [IO.File]::WriteAllBytes($Key, $made.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
    if (-not $MakeKey) { Get-PublicBlob $made; return }
}

$cngKey = [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Key)),
    [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
if ($PublicKey) { Get-PublicBlob $cngKey; return }
if ($Header) {
    # Write only on change so the header's timestamp does not trigger a rebuild.
    $line = "#pragma once`r`n" + (Get-PublicBlob $cngKey) + "`r`n"
    if (-not (Test-Path -LiteralPath $Header) -or [IO.File]::ReadAllText($Header) -ne $line) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $Header) -Force | Out-Null
        [IO.File]::WriteAllText($Header, $line, (New-Object System.Text.UTF8Encoding $false))
    }
    return
}

if (-not $Folder -or -not $Name -or -not $Dll) { throw 'Signing needs -Folder, -Name and -Dll.' }
$Folder = (Resolve-Path -LiteralPath $Folder).Path
if ($Dll -ne (Split-Path -Leaf $Dll)) { throw '-Dll is a file name inside the folder, not a path.' }
if ($Ends -and $Ends -notmatch '^\d{4}-\d{2}-\d{2}$') { throw '-Ends is YYYY-MM-DD.' }
$dllPath = Join-Path $Folder $Dll
$manifest = [ordered]@{
    id = Split-Path -Leaf $Folder; name = $Name; dll = $Dll
    sha256 = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
    owner = $For; ends = $Ends; abi = $Abi
} | ConvertTo-Json
$bytes = (New-Object System.Text.UTF8Encoding $false).GetBytes($manifest)
[IO.File]::WriteAllBytes((Join-Path $Folder 'addon.json'), $bytes)
$ecdsa = New-Object System.Security.Cryptography.ECDsaCng $cngKey
$ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
[IO.File]::WriteAllBytes((Join-Path $Folder 'addon.sig'), $ecdsa.SignData($bytes))
Write-Host "Signed $Folder"
