# Fetches the unchanged official PawnIO files the installer ships into installer\drittanbieter and checks their hashes.
$ErrorActionPreference = 'Stop'
$t = Join-Path $PSScriptRoot 'drittanbieter'
New-Item -ItemType Directory -Force $t | Out-Null
$files = @(
    @{ url = 'https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe'; name = 'PawnIO_setup.exe'
       sha = '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032' },
    @{ url = 'https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip'; name = 'modules.zip'
       sha = '43608CB89BC84247FEF1368A139013F7D043E17DB6D6C8DFC9B46BF0905A81F4' }
)
foreach ($f in $files) {
    $p = Join-Path $t $f.name
    if (!(Test-Path $p) -or (Get-FileHash $p).Hash -ne $f.sha) { Invoke-WebRequest $f.url -OutFile $p -UseBasicParsing }
    if ((Get-FileHash $p).Hash -ne $f.sha) { throw "Falscher Hash: $($f.name)" }
}
Expand-Archive (Join-Path $t 'modules.zip') (Join-Path $t 'modules') -Force
