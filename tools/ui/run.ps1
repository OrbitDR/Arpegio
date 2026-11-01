# Runs the checks of the real application with digital silence. Nothing is audible, but the DAC changes format.
#   powershell tools\ui\run.ps1 -MusicFolder "D:\Música" [-Device topping] [-Data FOLDER]
# The music folder is only catalogued, and only into the temporary data folder.
param(
    [Parameter(Mandatory)][string]$MusicFolder,
    [string]$Device = 'topping',
    [string]$Data = "$env:TEMP\arpegio-ui-check",
    [string]$Build = 'release'
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
Set-Location $repo

Remove-Item $Data -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Data\native", "$Data\outputs", "$Data\media", "$Data\volume" | Out-Null
$silence = "$Data\silence"
python tools\ui\make_silence.py $silence

$exe = (Resolve-Path "build\$Build\arpegio.exe").Path
$env:SILENCE_FOLDER = (Resolve-Path $silence).Path
$env:MUSIC_FOLDER = $MusicFolder
$env:ARPEGIO_DEVICE = $Device
$env:ARPEGIO_EXE = $exe
$env:ARPEGIO_DATA = "$Data\outputs"
$shots = "$Data\shots"

node tools\ui\check.mjs --exe $exe --data "$Data\native" --out $shots --steps tools\ui\steps_native.mjs
if ($LASTEXITCODE) { exit 1 }
$window = (Get-Content "$Data\native\settings.json" -Raw | ConvertFrom-Json).window
if (-not $window) { 'FAIL the window place was not saved'; exit 1 }
"ok   window place saved: $($window.width) x $($window.height) at $($window.x), $($window.y)"
foreach ($run in @(
    @('native', 'steps_sections.mjs'),
    @('outputs', 'steps_outputs.mjs'),
    @('outputs', 'steps_interaction.mjs'),
    @('outputs', 'steps_clicks.mjs'),
    @('media', 'steps_media.mjs'),
    @('volume', 'steps_volume.mjs'),
    @('outputs', 'steps_cpu.mjs')
)) {
    node tools\ui\check.mjs --exe $exe --data "$Data\$($run[0])" --out $shots --steps "tools\ui\$($run[1])"
    if ($LASTEXITCODE) { exit 1 }
}
