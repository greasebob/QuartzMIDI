# Installs the audio-to-MIDI converter's dependencies into this folder, for
# both source builds and releases.
#
#   powershell -ExecutionPolicy Bypass -File tools\mp3-to-midi\setup.ps1 [-Engine cuda13|cuda12|directml]
#
# The app runs this from the Convert popup and parses its step:/done:/error:
# lines (same protocol as convert.py); setup.cmd runs it manually. Everything is
# fetched from the upstream publishers, and any system Python is ignored.
# Creates:
#   python\   python.org embeddable Python 3.12 with the packages pinned in
#             requirements.txt, and one engine's, from PyPI
#   model\    Transkun's model as ONNX, from Pianoscribe's publisher
#   ffmpeg\   ffmpeg.exe and ffprobe.exe from gyan.dev's FFmpeg build on GitHub
#             releases (gyan.dev's own site keeps only the latest version)
#   deno\     deno.exe from Deno's GitHub release
#   engine.json  the engine that passed its check and the ones this PC can run,
#             {"engine": "cuda13", "engines": ["cuda13", "cuda12", "directml"]}
# Python, pip, the model, FFmpeg and Deno must match the SHA-256 in pins.psd1; a
# mismatch is deleted. pip verifies every package against requirements*.txt,
# whose hashes tools\pin-hashes.py writes. Rerun to resume after an
# interruption.
#
# The engine is the ONNX Runtime the model runs on. Without -Engine the best one
# for the card is picked: CUDA 13 on an NVIDIA card of compute capability 7.5 or
# more with driver 580 or later, CUDA 12 on an older NVIDIA card with driver 528
# or later, DirectML otherwise. Each is checked by running the model on the
# card, and one that fails gives way to the next, ending with DirectML, which
# also runs on the CPU. -Engine tries that one first, unless the card the
# driver names cannot run it.

param([ValidateSet('auto', 'cuda13', 'cuda12', 'directml')] [string] $Engine = 'auto')

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

$engineNames = @{ cuda13 = 'CUDA 13'; cuda12 = 'CUDA 12'; directml = 'DirectML' }

# The engines this PC can run, best first, from the first NVIDIA card's compute
# capability and driver; DirectML alone without one.
function Get-Engines {
    $smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
    if (-not (Test-Path -LiteralPath $smi)) {
        $found = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
        if (-not $found) { return @('directml') }
        $smi = $found.Source
    }
    try { $card = @(& $smi --query-gpu=name,compute_cap,driver_version --format=csv,noheader 2>$null) | Select-Object -First 1 } catch { $card = $null }
    if ("$card" -notmatch '^(.+?),\s*(\d+)\.(\d+),\s*(\d+)') {
        Write-Host 'The NVIDIA driver did not name the card; using DirectML.'
        return @('directml')
    }
    $capability = [int]$Matches[2] * 10 + [int]$Matches[3]; $driver = [int]$Matches[4]
    Write-Host "$($Matches[1]), compute capability $($capability / 10), driver $driver"
    $script:cardNamed = $true
    $engines = @()
    if ($capability -ge 75 -and $driver -ge 580) { $engines += 'cuda13' }
    # onnxruntime-gpu 1.26's Windows build holds code from compute capability 6.1.
    if ($capability -ge 61 -and $driver -ge 528) { $engines += 'cuda12' }
    return $engines + 'directml'
}

# The package names a requirements file pins.
function Get-PinnedNames([string] $File) {
    Get-Content -LiteralPath (Join-Path $here $File) | ForEach-Object { if ($_ -match '^([A-Za-z0-9_.-]+)==') { $Matches[1] } }
}

# Runs the private Python's pip and returns its exit code. pip's "Downloading
# <wheel> (<size>)" lines become step: progress for $Step ("2 of 6"); the rest
# goes to the log. --isolated ignores machine pip config. The ._pth file is
# taken away while pip runs, because while it exists Python ignores the path
# pip uses to build proxy_tools, and written back after.
function Invoke-Pip([string] $Step, [string] $What, [string[]] $Arguments) {
    if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth -Force }
    $saved = @{ PYTHONHOME = $env:PYTHONHOME; PYTHONPATH = $env:PYTHONPATH; PYTHONNOUSERSITE = $env:PYTHONNOUSERSITE }
    try {
        $env:PYTHONHOME = $null; $env:PYTHONPATH = $null; $env:PYTHONNOUSERSITE = '1'
        # pip uninstall takes no --progress-bar.
        $quiet = if ($Arguments[0] -eq 'install') { @('--progress-bar', 'off') } else { @() }
        & $python $pip @Arguments --isolated --disable-pip-version-check @quiet |
            ForEach-Object {
                if ("$_" -match '^\s*Downloading (?:\S*/)?([^/\s]+?)-\d\S* \((.+)\)') { Write-Host "step: Downloading $($Matches[1]), $($Matches[2]) ($Step)" }
                elseif ("$_" -match '^Installing collected packages') { Write-Host "step: Installing $What ($Step)" }
                else { Write-Host "$_" }
            }
        return $LASTEXITCODE
    } finally {
        $env:PYTHONHOME = $saved.PYTHONHOME; $env:PYTHONPATH = $saved.PYTHONPATH; $env:PYTHONNOUSERSITE = $saved.PYTHONNOUSERSITE
        # Fix the import path so no other Python installation can leak in.
        Set-Content -LiteralPath $pth -Encoding Ascii -Value @('python312.zip', '.', 'Lib\site-packages', 'import site')
    }
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
    # from its wheel.
    Write-Host 'step: Downloading the packages (2 of 6)'
    $pip = Join-Path (Get-Pinned $pins.Pip) 'pip'
    # The pinned setuptools is installed first so proxy_tools builds with it
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
    if ((Invoke-Pip '2 of 6' 'the packages' @('install', '--require-hashes', '--no-deps', '--only-binary=:all:', '--no-compile', '--no-warn-script-location',
                                          '--requirement', $buildRequirements)) -ne 0) { throw 'Installing the converter packages failed.' }
    if ((Invoke-Pip '2 of 6' 'the packages' @('install', '--require-hashes', '--no-deps', '--only-binary=:all:', '--no-binary=proxy_tools', '--no-build-isolation',
                                          '--no-compile', '--no-warn-script-location', '--requirement', $usedRequirements)) -ne 0) {
        throw 'Installing the converter packages failed.'
    }

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

    # The engine. Each one tried replaces the other engines' packages, then the
    # check imports everything convert.py and signin.py use and runs the model
    # once where conversions will run it. A CUDA engine passes only when CUDA
    # ran it; DirectML passes on the CPU too, being the last.
    # -Engine goes first, unless the card the driver named cannot run it.
    $cardNamed = $false
    $engines = @(Get-Engines)
    $order = $engines
    if ($Engine -ne 'auto' -and ($engines -contains $Engine -or -not $cardNamed)) {
        $order = @($Engine) + @($engines | Where-Object { $_ -ne $Engine })
    } elseif ($Engine -ne 'auto') {
        Write-Host "This card cannot run $($engineNames[$Engine])."
    }
    $check = "import sys; sys.path.insert(0, sys.argv[1]); import transcribe, yt_dlp, yt_dlp_ejs, webview; " +
             "model = transcribe.Model('auto', 1); model.check(); print('engine: ' + (model.engine or 'CPU'))"
    $installed = $null; $ran = $null
    foreach ($candidate in $order) {
        $name = $engineNames[$candidate]
        $file = "requirements-$candidate.txt"
        $keep = @(Get-PinnedNames $file)
        $others = @($engineNames.Keys | Where-Object { $_ -ne $candidate } | ForEach-Object { Get-PinnedNames "requirements-$_.txt" } |
                    Where-Object { $keep -notcontains $_ } | Sort-Object -Unique)
        Write-Host "step: Installing $name (6 of 6)"
        if ((Invoke-Pip '6 of 6' $name (@('uninstall', '--yes') + $others)) -ne 0) { throw 'Removing the other engines failed.' }
        # pip leaves the CUDA libraries' empty folders behind.
        $nvidia = Join-Path $pythonHome 'Lib\site-packages\nvidia'
        if ((Test-Path -LiteralPath $nvidia) -and -not (Get-ChildItem -LiteralPath $nvidia -Recurse -File)) {
            Remove-Item -LiteralPath $nvidia -Recurse -Force
        }
        # No cache: a CUDA engine's 1.3 to 1.8 GB would stay in pip's cache too.
        if ((Invoke-Pip '6 of 6' $name @('install', '--require-hashes', '--no-deps', '--only-binary=:all:', '--no-compile', '--no-cache-dir',
                                         '--no-warn-script-location', '--requirement', (Join-Path $here $file))) -ne 0) {
            throw "Installing $name failed."
        }
        Write-Host "step: Checking $name (6 of 6)"
        $report = @(& $python -c $check $here)
        $exit = $LASTEXITCODE
        $report | ForEach-Object { Write-Host "$_" }
        $ran = "$($report | Where-Object { "$_" -like 'engine: *' } | Select-Object -Last 1)" -replace '^engine: ', ''
        if ($exit -eq 0 -and ($candidate -eq 'directml' -or $ran -eq 'CUDA')) { $installed = $candidate; break }
        Write-Host "$name did not run the model$(if ($ran) { " (it ran on $ran)" })."
    }
    if (-not $installed) {
        throw 'The converter does not import. The log says which package; a DLL load failure in onnxruntime means the Visual C++ redistributable is missing.'
    }
    Set-Content -LiteralPath (Join-Path $here 'engine.json') -Encoding Ascii -Value (ConvertTo-Json -Compress -InputObject ([ordered]@{ engine = $installed; engines = @($engines) }))

    # Delete the downloaded archives.
    Remove-Item -LiteralPath $downloads -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $partial -Force
    # The PyTorch converter's note that its CUDA build had no code for the card.
    Remove-Item -LiteralPath (Join-Path $here 'gpu-unsupported') -Force -ErrorAction SilentlyContinue
    Write-Host "done: The converter runs on $(if ($ran -eq 'CPU') { 'the CPU' } else { $engineNames[$installed] })."
} catch {
    # Single error: line for the app; setup.partial remains so a rerun resumes.
    Write-Host "error: $($_.Exception.Message)"
    exit 1
}
