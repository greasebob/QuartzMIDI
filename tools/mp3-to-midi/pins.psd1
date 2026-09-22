# Third-party downloads for the converter, each from its publisher at a pinned
# version and SHA256. Read by setup.ps1 and tools\make-release.ps1. To bump a
# version, change the URL and the hash together; a hash mismatch aborts setup.
@{
    # Embeddable Python, and the pip wheel setup.ps1 runs it with; no system
    # Python is needed.
    Python      = @{ Url = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
                     Sha256 = '4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3' }
    Pip         = @{ Url = 'https://files.pythonhosted.org/packages/f3/6e/1736e5b4ae2b778ef2f81c47d797de9f891d4d8acb047a24ca37a60294dd/pip-26.2.1-py3-none-any.whl'
                     Sha256 = '71138ADF1F4CA900CDB7D289C21B7494329F2332B6D85F0E1C42108C0384ED3E' }
    Ffmpeg     = @{ Url = 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.1/ffmpeg-9.0.1-essentials_build.zip'
                     Sha256 = 'FEC81AE03971D9DD4BE3EBE02E263BD2EC1D789483F931BDBA5F5715E65DA2E9' }
    Deno        = @{ Url = 'https://github.com/denoland/deno/releases/download/v2.9.6/deno-x86_64-pc-windows-msvc.zip'
                     Sha256 = '15E5300B0BA3C3695A7621D90160A746EC9E710228CEE639AFA9D580F6E3CD11' }
    DenoLicence = @{ Url = 'https://raw.githubusercontent.com/denoland/deno/v2.9.6/LICENSE.md'
                     Sha256 = 'F62497FFFECC0852960C8D3E6934B9DB86D16396E9B604072E923892CAE3A588'
                     Name = 'deno-2.9.6-LICENSE.md' }
}
