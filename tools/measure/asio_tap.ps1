# Puts the ASIO tap in front of a driver for this Windows user, or takes it away.
#   powershell tools\measure\asio_tap.ps1 on [-Device topping] [-Audible]
#   powershell tools\measure\asio_tap.ps1 off [-Device topping]
#   powershell tools\measure\asio_tap.ps1 status [-Device topping]
# Nothing is installed: the override is one key under HKCU, and removing it restores the real driver.
# Muted by default, so the host plays normally and the DAC gets silence. Leaving it on mutes ASIO.
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('on', 'off', 'status')][string]$Action,
    [string]$Device = 'topping',
    [switch]$Audible
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$dll = Join-Path $repo 'build\tap\asio_tap.dll'

$driver = Get-ChildItem 'HKLM:\SOFTWARE\ASIO' | Where-Object { $_.PSChildName -like "*$Device*" } | Select-Object -First 1
if (-not $driver) { throw "no ASIO driver matches '$Device'" }
$clsid = (Get-ItemProperty $driver.PSPath).CLSID
$key = "HKCU:\Software\Classes\CLSID\$clsid"
$server = "$key\InprocServer32"
$current = if (Test-Path -LiteralPath $server) { (Get-ItemProperty -LiteralPath $server).'(default)' }

switch ($Action) {
    'on' {
        if (-not (Test-Path $dll)) { throw 'build the tap first: tools\measure\asio_tap.bat' }
        if ((Test-Path -LiteralPath $key) -and $current -ne $dll) { throw "$key already exists and is not the tap" }
        Set-Content (Join-Path (Split-Path $dll) 'asio_tap.ini') "[tap]`r`nmute=$(if ($Audible) { 0 } else { 1 })"
        New-Item -Path $server -Force | Out-Null
        Set-ItemProperty -LiteralPath $server -Name '(default)' -Value $dll
        Set-ItemProperty -LiteralPath $server -Name 'ThreadingModel' -Value 'Apartment'
        "tap ON for $($driver.PSChildName), $(if ($Audible) { 'audible' } else { 'muted' })"
    }
    'off' {
        if (-not (Test-Path -LiteralPath $key)) { "tap already off for $($driver.PSChildName)"; break }
        if ($current -ne $dll) { throw "$key is not the tap, leaving it alone" }
        Remove-Item -LiteralPath $key -Recurse
        "tap OFF for $($driver.PSChildName)"
    }
    'status' {
        if ($current) { "tap ON for $($driver.PSChildName): $current" } else { "tap off for $($driver.PSChildName)" }
    }
}
