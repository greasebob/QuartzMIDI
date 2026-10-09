# The fonts the sheets add-on carries, each file from its publisher on GitHub at
# a pinned commit. Read by tools\fetch-fonts.ps1, which both sheets projects run
# before their resources compile. Size and GitSha1 are GitHub's own listing of
# the commit; -Pin checks them and prints the Sha256 every other run checks. To
# bump a family, change Commit, Base and every file's pins together.
@{
    Families = @(
        @{
            Name    = 'Atkinson Hyperlegible Next'
            Face    = 23
            Commit  = '5d633f80fc654ef5fffa7cfc257528685158dcef'
            Base    = 'https://raw.githubusercontent.com/googlefonts/atkinson-hyperlegible-next/5d633f80fc654ef5fffa7cfc257528685158dcef/'
            Folder  = 'third_party\fonts\atkinson-hyperlegible-next'
            Licence = 'Atkinson-Hyperlegible-Next-LICENSE.txt'
            Files   = @(
                @{ Path = 'fonts/ttf/AtkinsonHyperlegibleNext-Regular.ttf';    Style = 'Regular';    Size = 65068; GitSha1 = 'd0982cdce554149011bec58b21ac4a1bf8ee9ff5'; Sha256 = '88ED5C31A71584C7772963B02D04BEF1EB7E3D2E9C8B9CB204339B1F82CF432C' }
                @{ Path = 'fonts/ttf/AtkinsonHyperlegibleNext-Bold.ttf';       Style = 'Bold';       Size = 66792; GitSha1 = '668a2d166581035f7f836a700a6cc3f26881ec68'; Sha256 = '994414047DF66BB4998D01C1CB1EEB4A2DDC4622D1AA56BBB8ADBECA7645B041' }
                @{ Path = 'fonts/ttf/AtkinsonHyperlegibleNext-Italic.ttf';     Style = 'Italic';     Size = 69812; GitSha1 = '6d09ee04da6cdd33e507fee27c48bdcff133051d'; Sha256 = 'B7F8F03CEB28EBADB2DE2332B2E869A00E8DBFB5C8E00FDBB06385770C8227E7' }
                @{ Path = 'fonts/ttf/AtkinsonHyperlegibleNext-BoldItalic.ttf'; Style = 'BoldItalic'; Size = 71456; GitSha1 = 'f5863f958424311d6d8b0d17fe1ccb4f3782d1dd'; Sha256 = 'C2D3C596B54D1565CA20D383CDFB127B50B18B500B85A774FD07D4D976CD018B' }
                @{ Path = 'OFL.txt';                                           Style = 'Licence';    Size = 4431;  GitSha1 = '88955733b80000358ef3e5521f3369b276e066a9'; Sha256 = 'ACA6A428580965D2297D1B718042DD427C2A9443ECE3B0D02D758E161E0C4030' }
            )
        }
        @{
            Name    = 'Inclusive Sans'
            Face    = 24
            Commit  = '38b8ed1dd67bd25ef4180140492810cf050ef501'
            Base    = 'https://raw.githubusercontent.com/LivKing/Inclusive-Sans/38b8ed1dd67bd25ef4180140492810cf050ef501/'
            Folder  = 'third_party\fonts\inclusive-sans'
            Licence = 'Inclusive-Sans-LICENSE.txt'
            Files   = @(
                @{ Path = 'fonts/ttf/InclusiveSans-Regular.ttf';    Style = 'Regular';    Size = 74616; GitSha1 = '2a36c67c393f3c8ec712cfd0b4679c415b104a9d'; Sha256 = '89DA91C8547A7EE508A59D67184C8E3CCFDE91DA0F262489BC7520D8287668A8' }
                @{ Path = 'fonts/ttf/InclusiveSans-Bold.ttf';       Style = 'Bold';       Size = 73644; GitSha1 = 'aed2cbc59d553d79d97a3118d3e49b1b36754e8c'; Sha256 = '8B8B4997F3BBE83B88225FE8714569016003007CB486831E39785A34AFD5C90C' }
                @{ Path = 'fonts/ttf/InclusiveSans-Italic.ttf';     Style = 'Italic';     Size = 76412; GitSha1 = '7eb0375876ff19f455be909290d29c9928fa1f47'; Sha256 = 'C76BB4B1FD2DC7D3650CEFA83BF88BEE32B36920393E4CF9C2BF1412CCDE8121' }
                @{ Path = 'fonts/ttf/InclusiveSans-BoldItalic.ttf'; Style = 'BoldItalic'; Size = 75468; GitSha1 = '5edd753d9da3447f4a88c1a8bfcd8c8385b8e352'; Sha256 = '1B4E05DC7EE62BE001FAE1DAA7CF1EB259F011963B88F893ED8E4E4A3C31591F' }
                @{ Path = 'OFL.txt';                                Style = 'Licence';    Size = 4398;  GitSha1 = 'ca5dd5d917d5d1ddafeca3575bd46da710f0d3ab'; Sha256 = '0690E0819628A20C5EE68388A7BAE621E5C3E53BE57276169DECD00111D8F2CF' }
            )
        }
    )
}
