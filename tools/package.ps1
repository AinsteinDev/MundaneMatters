# Builds the release zip: dist\MundaneMatters-v<version>.zip, laid out to install with MO2/Vortex or
# extract into Data. Run .\build.ps1 first. The .pdb (debug symbols) is left out.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern '^\s*VERSION\s+([\d.]+)').Matches[0].Groups[1].Value
$data = Join-Path $root 'data'
if (-not (Test-Path "$data\SKSE\Plugins\MundaneMatters.dll")) { throw 'MundaneMatters.dll missing - run build.ps1 first' }

$stage = Join-Path $root 'dist\stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
robocopy $data $stage /E /XF *.pdb meta.ini /NJH /NJS /NDL /NFL /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

$zip = Join-Path $root "dist\MundaneMatters-v$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# .NET writes forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes).
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
Remove-Item -Recurse -Force $stage
Write-Host "Packaged: $zip"
exit 0
