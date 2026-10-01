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
#             requirements.txt, from PyPI
#   model\    Transkun's model as ONNX, from Pianoscribe's publisher
#   ffmpeg\   ffmpeg.exe and ffprobe.exe from gyan.dev's FFmpeg build on GitHub
#             releases (gyan.dev's own site keeps only the latest version)
#   deno\     deno.exe from Deno's GitHub release
# Python, pip, the model, FFmpeg and Deno must match the SHA-256 in pins.psd1; a
# mismatch is deleted. pip verifies every package against requirements.txt,
# whose hashes tools\pin-hashes.py writes. Rerun to resume after an
# interruption.

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

try {
    $pins = Import-PowerShellDataFile -LiteralPath (Join-Path $here 'pins.psd1')
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    Set-Content -LiteralPath $partial -Encoding Ascii -Value 'setup.ps1 has not finished; run it again.'

    # A private Python, because the pinned wheels are for CPython 3.12.
    Write-Host 'step: Downloading Python (1 of 6)'
    $pythonHome = Join-Path $here 'python'
    $python = Join-Path $pythonHome 'python.exe'
    $pth = Join-Path $pythonHome 'python312._pth'
    # A converter from before ONNX Runtime ran Transkun on PyTorch; its 1 to 3 GB
    # of packages go with the Python they were installed into.
    $oldTorch = Join-Path $pythonHome 'Lib\site-packages\torch'
    if (-not (Test-Path -LiteralPath $python) -or (Test-Path -LiteralPath $oldTorch)) {
        $embedZip = Get-Pinned $pins.Python
        if (Test-Path -LiteralPath $pythonHome) { Remove-Item -LiteralPath $pythonHome -Recurse -Force }
        # Extract to a temporary folder so an interrupted run leaves no partial install.
        $unpacking = "$pythonHome.partial"
        if (Test-Path -LiteralPath $unpacking) { Remove-Item -LiteralPath $unpacking -Recurse -Force }
        [System.IO.Compression.ZipFile]::ExtractToDirectory($embedZip, $unpacking)
        Move-Item -LiteralPath $unpacking -Destination $pythonHome
    }
    # Releases bundle the Visual C++ runtime DLLs onnxruntime needs in runtime\;
    # source builds rely on the installed runtime.
    $runtime = Join-Path $here 'runtime'
    if (Test-Path -LiteralPath $runtime) { Copy-Item -Path (Join-Path $runtime '*.dll') -Destination $pythonHome -Force }

    # Install exactly requirements.txt: --no-deps, wheels only except proxy_tools
    # (pure Python, sdist only). The embeddable Python has no pip, so pip runs
    # from its wheel. The ._pth file is written afterwards because while it
    # exists Python ignores the path pip uses to build proxy_tools.
    Write-Host 'step: Downloading the packages (2 of 6)'
    $pip = Join-Path (Get-Pinned $pins.Pip) 'pip'
    if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth -Force }
    $saved = @{ PYTHONHOME = $env:PYTHONHOME; PYTHONPATH = $env:PYTHONPATH; PYTHONNOUSERSITE = $env:PYTHONNOUSERSITE }
    try {
        $env:PYTHONHOME = $null; $env:PYTHONPATH = $null; $env:PYTHONNOUSERSITE = '1'
        # pip's "Downloading <wheel> (<size>)" lines become step: progress; the
        # rest goes to the log. --isolated ignores machine pip config. The pinned
        # setuptools is installed first so proxy_tools builds with it
        # (--no-build-isolation) instead of an unpinned latest setuptools.
        $usedRequirements = Join-Path $here 'requirements.txt'
        $requirements = Get-Content -LiteralPath $usedRequirements
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
                if ("$_" -match '^\s*Downloading (?:\S*/)?([^/\s]+?)-\d\S* \((.+)\)') { Write-Host "step: Downloading $($Matches[1]), $($Matches[2]) (2 of 6)" }
                elseif ("$_" -match '^Installing collected packages') { Write-Host 'step: Installing the packages (2 of 6)' }
                else { Write-Host "$_" }
            }
        if ($LASTEXITCODE -ne 0) { throw 'Installing the converter packages failed.' }
    } finally {
        $env:PYTHONHOME = $saved.PYTHONHOME; $env:PYTHONPATH = $saved.PYTHONPATH; $env:PYTHONNOUSERSITE = $saved.PYTHONNOUSERSITE
    }
    # Fix the import path so no other Python installation can leak in.
    Set-Content -LiteralPath $pth -Encoding Ascii -Value @('python312.zip', '.', 'Lib\site-packages', 'import site')

    Write-Host 'step: Downloading the transcription model (3 of 6)'
    $model = Join-Path $here 'model'
    $parts = 'scorer.onnx', 'attributes.onnx', 'frontend.npz', 'manifest.json', 'LICENSE.txt'
    if (@($parts | Where-Object { -not (Test-Path -LiteralPath (Join-Path $model $_)) }).Count) {
        $modelZip = Get-Pinned $pins.Model
        foreach ($part in $parts) { Expand-Entry $modelZip $part (Join-Path $model $part) }
    }

    Write-Host 'step: Downloading FFmpeg (4 of 6)'
    if (-not ((Test-Path -LiteralPath (Join-Path $here 'ffmpeg\ffmpeg.exe')) -and (Test-Path -LiteralPath (Join-Path $here 'ffmpeg\ffprobe.exe')))) {
        $ffmpegZip = Get-Pinned $pins.Ffmpeg
        Expand-Entry $ffmpegZip '*/bin/ffmpeg.exe' (Join-Path $here 'ffmpeg\ffmpeg.exe')
        Expand-Entry $ffmpegZip '*/bin/ffprobe.exe' (Join-Path $here 'ffmpeg\ffprobe.exe')
    }
    Write-Host 'step: Downloading Deno (5 of 6)'
    if (-not (Test-Path -LiteralPath (Join-Path $here 'deno\deno.exe'))) {
        Expand-Entry (Get-Pinned $pins.Deno) 'deno.exe' (Join-Path $here 'deno\deno.exe')
    }

    # Smoke test: every import used by convert.py and signin.py.
    # Smoke test: every import used by convert.py and signin.py, and the model
    # loaded where conversions will run it.
    Write-Host 'step: Checking the converter (6 of 6)'
    & $python -c 'import sys; sys.path.insert(0, sys.argv[1]); import transcribe, yt_dlp, yt_dlp_ejs, webview; print(''GPU'' if transcribe.Model(''auto'', 1).gpu else ''CPU'')' $here
    if ($LASTEXITCODE -ne 0) {
        throw 'The converter does not import. The log says which package; a DLL load failure in onnxruntime means the Visual C++ redistributable is missing.'
    }

    # Delete the downloaded archives.
    Remove-Item -LiteralPath $downloads -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $partial -Force
    # The PyTorch converter's note that its CUDA build had no code for the card.
    Remove-Item -LiteralPath (Join-Path $here 'gpu-unsupported') -Force -ErrorAction SilentlyContinue
    Write-Host 'done: The converter is installed.'
} catch {
    # Single error: line for the app; setup.partial remains so a rerun resumes.
    Write-Host "error: $($_.Exception.Message)"
    exit 1
}
