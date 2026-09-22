# Installs the audio-to-MIDI converter's dependencies into this folder, for
# both source builds and releases.
#
#   powershell -ExecutionPolicy Bypass -File tools\mp3-to-midi\setup.ps1
#
# The app runs this from the Convert popup and parses its step:/done:/error:
# lines (same protocol as convert.py); setup.cmd runs it manually. Everything is
# fetched from the upstream publishers, and any system Python is ignored.
# Creates:
#   python\   python.org embeddable Python 3.12 with the packages pinned in
#             requirements.txt (from PyPI and PyTorch's index)
#   ffmpeg\   ffmpeg.exe and ffprobe.exe from gyan.dev's FFmpeg build on GitHub
#             releases (gyan.dev's own site keeps only the latest version)
#   deno\     deno.exe from Deno's GitHub release
# Python, pip, FFmpeg and Deno must match the SHA-256 in pins.psd1; a mismatch
# is deleted. pip verifies every package against requirements.txt, whose hashes
# tools\pin-hashes.py writes. About 1.2 GB; rerun to resume after an interruption.
#
# -Nvidia installs a CUDA build of PyTorch instead of the CPU build (about
# 2 GB versus 120 MB; NVIDIA GPUs only): CUDA 13.0 for a card of compute
# capability 7.5 or more on a driver of 580 or later, which the RTX 50 series
# needs, else CUDA 12.6. nvidia-smi names the card. Rerun with or without it to
# switch; only PyTorch is downloaded again.

param([switch] $Nvidia)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$downloads = Join-Path $here 'downloads'
# While this marker exists the app treats the converter as not installed.
$partial = Join-Path $here 'setup.partial'

function Get-Pinned([hashtable] $Pin) {
    $name = if ($Pin.Name) { $Pin.Name } else { Split-Path -Leaf $Pin.Url }
    $file = Join-Path $downloads $name
    if (-not (Test-Path -LiteralPath $file)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $client = New-Object System.Net.WebClient
        # Report the file name and the innermost network error.
        try { $client.DownloadFile($Pin.Url, "$file.partial") }
        catch { throw "$name could not be downloaded: $($_.Exception.GetBaseException().Message)" }
        finally { $client.Dispose() }
        Move-Item -LiteralPath "$file.partial" -Destination $file -Force
    }
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($actual -ne $Pin.Sha256) {
        Remove-Item -LiteralPath $file -Force
        throw "$name does not match its pinned SHA256 (got $actual). It was deleted; if a second run still differs, the publisher changed the file."
    }
    return $file
}

function Expand-Entry([string] $Zip, [string] $EntryPattern, [string] $Destination) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entry = $archive.Entries | Where-Object { $_.FullName -like $EntryPattern } | Select-Object -First 1
        if (-not $entry) { throw "$EntryPattern is not in $Zip." }
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        # Extract to a temporary name so an interrupted run leaves no partial exe.
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$Destination.partial", $true)
        Move-Item -LiteralPath "$Destination.partial" -Destination $Destination -Force
    } finally { $archive.Dispose() }
}

# The CUDA requirements file for the first NVIDIA card, from its compute
# capability and driver version. Throws when there is none or it cannot be used.
function Get-CudaRequirements {
    $smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
    if (-not (Test-Path -LiteralPath $smi)) {
        $found = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
        if (-not $found) { throw 'No NVIDIA card was found.' }
        $smi = $found.Source
    }
    $cards = @(& $smi --query-gpu=name,compute_cap,driver_version --format=csv,noheader)
    $card = $cards | Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or "$card" -notmatch '^(.+?),\s*(\d+)\.(\d+),\s*(\d+)') {
        throw 'The NVIDIA driver did not name the card. Update the driver, then install again.'
    }
    $name = $Matches[1]; $capability = [int]$Matches[2] * 10 + [int]$Matches[3]; $driver = [int]$Matches[4]
    Write-Host "$name, compute capability $($capability / 10), driver $driver"
    if ($capability -ge 75 -and $driver -ge 580) { return 'requirements-cu130.txt' }
    if ($capability -le 90) { return 'requirements-cu126.txt' }
    throw "The $name needs NVIDIA driver 580 or later. Update the driver, then install again."
}

try {
    # Before setup.partial: a card that cannot be used leaves the installed converter as it was.
    if ($Nvidia) { $cudaRequirements = Get-CudaRequirements }
    $pins = Import-PowerShellDataFile -LiteralPath (Join-Path $here 'pins.psd1')
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    Set-Content -LiteralPath $partial -Encoding Ascii -Value 'setup.ps1 has not finished; run it again.'

    # A private Python, because the pinned wheels are for CPython 3.12.
    Write-Host 'step: Downloading Python (1 of 5)'
    $pythonHome = Join-Path $here 'python'
    $python = Join-Path $pythonHome 'python.exe'
    $pth = Join-Path $pythonHome 'python312._pth'
    if (-not (Test-Path -LiteralPath $python)) {
        $embedZip = Get-Pinned $pins.Python
        if (Test-Path -LiteralPath $pythonHome) { Remove-Item -LiteralPath $pythonHome -Recurse -Force }
        # Extract to a temporary folder so an interrupted run leaves no partial install.
        $unpacking = "$pythonHome.partial"
        if (Test-Path -LiteralPath $unpacking) { Remove-Item -LiteralPath $unpacking -Recurse -Force }
        [System.IO.Compression.ZipFile]::ExtractToDirectory($embedZip, $unpacking)
        Move-Item -LiteralPath $unpacking -Destination $pythonHome
    }
    # Releases bundle the Visual C++ runtime DLLs torch needs in runtime\;
    # source builds rely on the installed runtime.
    $runtime = Join-Path $here 'runtime'
    if (Test-Path -LiteralPath $runtime) { Copy-Item -Path (Join-Path $runtime '*.dll') -Destination $pythonHome -Force }

    # Install exactly requirements.txt: --no-deps, wheels only except proxy_tools
    # (pure Python, sdist only). The embeddable Python has no pip, so pip runs
    # from its wheel. The ._pth file is written afterwards because while it
    # exists Python ignores the path pip uses to build proxy_tools.
    Write-Host 'step: Downloading the packages (2 of 5)'
    $pip = Join-Path (Get-Pinned $pins.Pip) 'pip'
    if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth -Force }
    $saved = @{ PYTHONHOME = $env:PYTHONHOME; PYTHONPATH = $env:PYTHONPATH; PYTHONNOUSERSITE = $env:PYTHONNOUSERSITE }
    try {
        $env:PYTHONHOME = $null; $env:PYTHONPATH = $null; $env:PYTHONNOUSERSITE = '1'
        # pip's "Downloading <wheel> (<size>)" lines become step: progress; the
        # rest goes to the log. --isolated ignores machine pip config. The pinned
        # setuptools is installed first so proxy_tools builds with it
        # (--no-build-isolation) instead of an unpinned latest setuptools.
        $requirements = Get-Content -LiteralPath (Join-Path $here 'requirements.txt')
        if ($Nvidia) {
            # Replace the +cpu torch/torchaudio pins and their index with the
            # card's CUDA file.
            $kept = @(); $dropping = $false
            foreach ($line in $requirements) {
                if ($line -match '^(torch|torchaudio)==' -or $line -match '^--extra-index-url') { $dropping = $line -notmatch '^--'; continue }
                if ($dropping -and $line -match '^\s+--hash=') { continue }
                $dropping = $false; $kept += $line
            }
            $requirements = $kept + (Get-Content -LiteralPath (Join-Path $here $cudaRequirements))
        }
        $usedRequirements = Join-Path $downloads 'requirements-used.txt'
        Set-Content -LiteralPath $usedRequirements -Encoding Ascii -Value $requirements
        $buildLines = @(); $taking = $false
        foreach ($line in $requirements) {
            if ($line -match '^setuptools==') { $taking = $true; $buildLines += $line }
            elseif ($taking -and $line -match '^\s+--hash=') { $buildLines += $line }
            else { $taking = $false }
        }
        if ($buildLines.Count -lt 2) { throw 'requirements.txt pins no setuptools with its hash.' }
        $buildRequirements = Join-Path $downloads 'build-requirements.txt'
        Set-Content -LiteralPath $buildRequirements -Encoding Ascii -Value $buildLines
        & $python $pip install --isolated --require-hashes --no-deps --only-binary=:all: --no-compile `
            --requirement $buildRequirements --disable-pip-version-check --progress-bar off --no-warn-script-location |
            ForEach-Object { Write-Host "$_" }
        if ($LASTEXITCODE -ne 0) { throw 'Installing the converter packages failed.' }
        & $python $pip install --isolated --require-hashes --no-deps --only-binary=:all: --no-binary=proxy_tools --no-build-isolation --no-compile `
            --requirement $usedRequirements --disable-pip-version-check --progress-bar off --no-warn-script-location |
            ForEach-Object {
                if ("$_" -match '^\s*Downloading (?:\S*/)?([^/\s]+?)-\d\S* \((.+)\)') { Write-Host "step: Downloading $($Matches[1]), $($Matches[2]) (2 of 5)" }
                elseif ("$_" -match '^Installing collected packages') { Write-Host 'step: Installing the packages (2 of 5)' }
                else { Write-Host "$_" }
            }
        if ($LASTEXITCODE -ne 0) { throw 'Installing the converter packages failed.' }
    } finally {
        $env:PYTHONHOME = $saved.PYTHONHOME; $env:PYTHONPATH = $saved.PYTHONPATH; $env:PYTHONNOUSERSITE = $saved.PYTHONNOUSERSITE
    }
    # Fix the import path so no other Python installation can leak in.
    Set-Content -LiteralPath $pth -Encoding Ascii -Value @('python312.zip', '.', 'Lib\site-packages', 'import site')

    Write-Host 'step: Downloading FFmpeg (3 of 5)'
    if (-not ((Test-Path -LiteralPath (Join-Path $here 'ffmpeg\ffmpeg.exe')) -and (Test-Path -LiteralPath (Join-Path $here 'ffmpeg\ffprobe.exe')))) {
        $ffmpegZip = Get-Pinned $pins.Ffmpeg
        Expand-Entry $ffmpegZip '*/bin/ffmpeg.exe' (Join-Path $here 'ffmpeg\ffmpeg.exe')
        Expand-Entry $ffmpegZip '*/bin/ffprobe.exe' (Join-Path $here 'ffmpeg\ffprobe.exe')
    }
    Write-Host 'step: Downloading Deno (4 of 5)'
    if (-not (Test-Path -LiteralPath (Join-Path $here 'deno\deno.exe'))) {
        Expand-Entry (Get-Pinned $pins.Deno) 'deno.exe' (Join-Path $here 'deno\deno.exe')
    }

    # Smoke test: every import used by convert.py and signin.py.
    Write-Host 'step: Checking the converter (5 of 5)'
    & $python -c 'import torch, transkun.transcribe, yt_dlp, yt_dlp_ejs, webview; print(torch.__version__)'
    if ($LASTEXITCODE -ne 0) {
        throw 'The converter does not import. The log says which package; a DLL load failure in torch means the Visual C++ redistributable is missing.'
    }
    # A CUDA build that imports can still have no code for the card; run one
    # operation on it so that shows here, not in the first conversion.
    if ($Nvidia) {
        & $python -c 'import torch; torch.ones(1, device=''cuda'').add_(1); torch.cuda.synchronize(); print(torch.cuda.get_device_name(0))'
        if ($LASTEXITCODE -ne 0) { throw 'PyTorch cannot run on this NVIDIA card. The log says why.' }
    }

    # Delete the ~300 MB of downloaded archives.
    Remove-Item -LiteralPath $downloads -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $partial -Force
    Write-Host 'done: The converter is installed.'
} catch {
    # Single error: line for the app; setup.partial remains so a rerun resumes.
    Write-Host "error: $($_.Exception.Message)"
    exit 1
}
