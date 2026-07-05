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
    # Backups must live OUTSIDE Plugins\: UE4 scans everything under Plugins\
    # for .uplugin files — a leftover NetcodePlus.bak.* inside it can shadow
    # the real plugin and the server silently runs the OLD DLL.
    $backupRoot = Join-Path (Split-Path $PluginsDir -Parent) 'PluginBackups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

    # Sweep legacy in-Plugins backups left by older versions of this script.
    Get-ChildItem -Path $PluginsDir -Directory -Filter 'NetcodePlus.bak.*' -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "Relocated legacy backup: $($_.Name)"; Move-Item $_.FullName $backupRoot }

    if (Test-Path $dest) { Move-Item $dest (Join-Path $backupRoot "NetcodePlus.bak.$(Get-Date -Format yyyyMMddHHmmss)") }
    New-Item -ItemType Directory -Path $dest | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    # Keep the two newest backups, prune the rest.
    Get-ChildItem -Path $backupRoot -Directory -Filter 'NetcodePlus.bak.*' |
        Sort-Object Name -Descending | Select-Object -Skip 2 | Remove-Item -Recurse -Force

    Write-Host "Installed build $($p.version) -> $dest . Restart the server."
}
finally { Remove-Item $tmp -Recurse -Force }
