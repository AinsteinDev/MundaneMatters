# Mirrors data\ into the MO2 mod folder "MundaneMatters". Enable the mod once in MO2's left pane.
# Usage: .\deploy.ps1   (run .\build.ps1 first)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'data'
$dst = $env:MM_DEPLOY_DIR  # your MO2 mod folder (...\mods\MundaneMatters)

if (-not (Test-Path "$src\SKSE\Plugins\MundaneMatters.dll")) { throw 'MundaneMatters.dll missing - run build.ps1 first' }

# /MIR keeps the MO2 folder an exact copy of data\ (files removed here are removed there).
# meta.ini is MO2's own file, so it is excluded from mirroring and never deleted.
robocopy $src $dst /MIR /XF meta.ini /NJH /NJS /NDL /NP | Out-Host
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

Write-Host "Deployed to $dst"
exit 0  # robocopy codes 1-7 mean success
