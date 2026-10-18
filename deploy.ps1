# Mirrors data\ into the MO2 mod folder "MundaneMatters". Enable the mod once in MO2's left pane.
# Usage: .\deploy.ps1   (run .\build.ps1 first)
# The MO2 mod folder comes from $env:MM_DEPLOY_DIR, or the first line of deploy.local (not committed).
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'data'
$dst = $env:MM_DEPLOY_DIR
$local = Join-Path $PSScriptRoot 'deploy.local'
if (-not $dst -and (Test-Path $local)) { $dst = (Get-Content $local -TotalCount 1).Trim() }
if (-not $dst) { throw 'Set MM_DEPLOY_DIR, or put your MO2 mod folder (...\mods\MundaneMatters) in deploy.local' }

if (-not (Test-Path "$src\SKSE\Plugins\MundaneMatters.dll")) { throw 'MundaneMatters.dll missing - run build.ps1 first' }

# /MIR keeps the MO2 folder an exact copy of data\ (files removed here are removed there).
# meta.ini is MO2's own file, so it is excluded from mirroring and never deleted. Debug symbols (.pdb)
# stay in the project for crash reports; the game never needs them.
robocopy $src $dst /MIR /XF meta.ini *.pdb /R:2 /W:2 /NJH /NJS /NDL /NP | Out-Host  # /R:2: a running game locks the DLL; fail fast
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

Write-Host "Deployed to $dst"
exit 0  # robocopy codes 1-7 mean success
