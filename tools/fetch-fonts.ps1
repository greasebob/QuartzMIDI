# Fetches the fonts the sheets add-on carries from their publishers on GitHub.
# The font files are never committed, so every build, from source or for a
# release, gets them here.
#
#   powershell -ExecutionPolicy Bypass -File tools\fetch-fonts.ps1
#   powershell -ExecutionPolicy Bypass -File tools\fetch-fonts.ps1 -Pin
#
# addons\sheets\fonts.psd1 names each file at a pinned commit with its size, its
# git blob SHA-1 and its SHA-256, and the folder under third_party\fonts it goes
# in. Both sheets projects run this before their resources compile: a file
# already there that matches its SHA-256 is left alone, and a missing or changed
# one is fetched and checked before it is written. -Verbose names the files left
# alone.
# -Pin is for a new pin: it fetches every file, checks its size and that its git
# blob SHA-1 is the one the publisher's commit lists, writes the files only once
# all of them match, and prints each SHA-256 for fonts.psd1.
# The files are only hashed and stored here.

[CmdletBinding()]
param([switch] $Pin)
$ErrorActionPreference = 'Stop'
# The repository is the folder above this script's, wherever it is run from.
$repo = Split-Path -Parent $PSScriptRoot

# Downloads into memory, so nothing reaches the disk before it is checked.
function Get-Bytes([string] $Name, [string] $Url) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $client = New-Object System.Net.WebClient
    # Report the file name, its URL and the innermost network error.
    try { return , $client.DownloadData($Url) }
    catch { throw "$Name could not be downloaded from ${Url}: $($_.Exception.GetBaseException().Message)" }
    finally { $client.Dispose() }
}

# git's name for a file's contents: the SHA-1 of "blob {size}", a NUL and the bytes.
function Get-GitSha1([byte[]] $Bytes) {
    $header = [Text.Encoding]::ASCII.GetBytes("blob $($Bytes.Length)`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    try {
        [void] $sha1.TransformBlock($header, 0, $header.Length, $null, 0)
        [void] $sha1.TransformFinalBlock($Bytes, 0, $Bytes.Length)
        return -join ($sha1.Hash | ForEach-Object { $_.ToString('x2') })
    } finally { $sha1.Dispose() }
}

# Upper case, as Get-FileHash writes it.
function Get-Sha256([byte[]] $Bytes) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { return -join ($sha256.ComputeHash($Bytes) | ForEach-Object { $_.ToString('X2') }) }
    finally { $sha256.Dispose() }
}

# Writes beside the target and renames it into place, so a font in the folder is
# always whole.
function Write-Font([string] $Target, [byte[]] $Bytes) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Target) -Force | Out-Null
    [IO.File]::WriteAllBytes("$Target.partial", $Bytes)
    Move-Item -LiteralPath "$Target.partial" -Destination $Target -Force
}

try {
    $pins = Import-PowerShellDataFile -LiteralPath (Join-Path $repo 'addons\sheets\fonts.psd1')
    $fonts = [IO.Path]::GetFullPath((Join-Path $repo 'third_party\fonts')) + '\'
    $files = foreach ($family in $pins.Families) {
        foreach ($file in $family.Files) {
            # Both families have an OFL.txt, so a file is named with its folder.
            $leaf = ($file.Path -split '[\\/]')[-1]
            $target = [IO.Path]::GetFullPath((Join-Path (Join-Path $repo $family.Folder) $leaf))
            # A pin only ever writes a plain file name in a folder under third_party\fonts,
            # so a changed pin cannot write over a script the build runs.
            if ($leaf -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or -not $target.StartsWith($fonts, [StringComparison]::OrdinalIgnoreCase)) {
                throw "addons\sheets\fonts.psd1 puts $($file.Path) in $($family.Folder), which is not a plain file name in a folder under third_party\fonts."
            }
            [pscustomobject] @{
                Pin = $file; Name = "$($family.Folder)\$leaf"; Url = $family.Base + $file.Path
                Target = $target; Bytes = $null
            }
        }
    }

    # Both sheets projects run this, at the same time under msbuild /m. One run at
    # a time, so neither writes over or renames a file the other is writing.
    $mutex = New-Object System.Threading.Mutex -ArgumentList $false, 'QuartzMIDI-fetch-fonts'
    # A run that ended without releasing it leaves it abandoned, and then it is ours.
    try { [void] $mutex.WaitOne() } catch [System.Threading.AbandonedMutexException] { }
    try {
        if ($Pin) {
            foreach ($file in $files) {
                $file.Bytes = Get-Bytes $file.Name $file.Url
                if ($file.Bytes.Length -ne $file.Pin.Size) {
                    throw "$($file.Name) from $($file.Url) is $($file.Bytes.Length) bytes, not the $($file.Pin.Size) of its pin. Nothing was written."
                }
                $gitSha1 = Get-GitSha1 $file.Bytes
                if ($gitSha1 -ne $file.Pin.GitSha1) {
                    throw "$($file.Name) from $($file.Url) has the git SHA-1 $gitSha1, not the $($file.Pin.GitSha1) of its pin. Nothing was written."
                }
            }
            foreach ($file in $files) {
                Write-Font $file.Target $file.Bytes
                Write-Host "$($file.Name) $(Get-Sha256 $file.Bytes)"
            }
        } else {
            foreach ($file in $files) {
                if (-not $file.Pin.Sha256) { throw "$($file.Name) has no SHA-256 in addons\sheets\fonts.psd1; tools\fetch-fonts.ps1 -Pin prints it." }
            }
            foreach ($file in $files) {
                if ((Test-Path -LiteralPath $file.Target) -and (Get-FileHash -LiteralPath $file.Target -Algorithm SHA256).Hash -eq $file.Pin.Sha256) {
                    Write-Verbose "$($file.Name) is in place."
                    continue
                }
                $bytes = Get-Bytes $file.Name $file.Url
                if ($bytes.Length -ne $file.Pin.Size -or (Get-Sha256 $bytes) -ne $file.Pin.Sha256) {
                    throw "$($file.Name) from the publisher does not match its pin."
                }
                Write-Font $file.Target $bytes
                Write-Host "Fetched $($file.Name)"
            }
        }
    } finally { $mutex.ReleaseMutex() }
} catch {
    # One line naming what failed, which msbuild shows as the build's error.
    Write-Host "error: $(($_.Exception.Message -replace '\s+', ' ').Trim())"
    exit 1
}
