# Usage: .\update-hub.ps1 -PluginsDir "C:\...\UnrealTournament\UnrealTournament\Plugins"
param([Parameter(Mandatory)][string]$PluginsDir)
$ErrorActionPreference = 'Stop'
$manifestUrl = 'https://github.com/jmortley/netcodeplus-launcher/releases/download/updates-latest/manifest.json'

$tmp = Join-Path $env:TEMP ("ncp-update-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $m   = (Invoke-WebRequest -Uri $manifestUrl -UseBasicParsing).Content | ConvertFrom-Json
    $p   = $m.channels.stable.plugin
    Write-Host "Latest NetcodePlus build: $($p.version)"

    $zip = Join-Path $tmp 'ncp.zip'
    Invoke-WebRequest -Uri $p.url -OutFile $zip -UseBasicParsing
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $p.sha256.ToLower()) { throw "SHA-256 mismatch: expected $($p.sha256), got $got" }

    $dest = Join-Path $PluginsDir 'NetcodePlus'
    if (Test-Path $dest) { Rename-Item $dest "NetcodePlus.bak.$(Get-Date -Format yyyyMMddHHmmss)" }
    New-Item -ItemType Directory -Path $dest | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Write-Host "Installed build $($p.version) -> $dest . Restart the server."
}
finally { Remove-Item $tmp -Recurse -Force }
