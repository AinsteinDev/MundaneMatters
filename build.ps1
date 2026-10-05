# Builds MundaneMatters.dll (RelWithDebInfo) and stages it into data\SKSE\Plugins.
# Usage: .\build.ps1            (configure if needed + build)
#        .\build.ps1 -Clean     (wipe build\ first)
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
	-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with C++ tools not found' }

$env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg'

# Keep every cache and temp file off C: (it is nearly full): downloads, registry clones,
# binary cache and the compiler's temp files all go under Projects\_cache on this drive.
$cache = Join-Path (Split-Path $root -Parent) '_cache'
foreach ($d in 'downloads', 'registries', 'binary', 'temp') { New-Item -ItemType Directory -Force (Join-Path $cache $d) | Out-Null }
$env:VCPKG_DOWNLOADS = Join-Path $cache 'downloads'
$env:X_VCPKG_REGISTRIES_CACHE = Join-Path $cache 'registries'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $cache 'binary'
$env:TEMP = Join-Path $cache 'temp'
$env:TMP = $env:TEMP
# cmake/ninja installed with: python -m pip install --user cmake ninja
$pyScripts = Join-Path $env:APPDATA 'Python\Python310\Scripts'
if (Test-Path $pyScripts) { $env:PATH = "$pyScripts;$env:PATH" }

if ($Clean -and (Test-Path "$root\build")) { Remove-Item -Recurse -Force "$root\build" }

# vcvars64.bat calls vswhere from PATH.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# Native stderr must not be fatal under Windows PowerShell 5.1, so merge it inside cmd.
$ErrorActionPreference = 'Continue'
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && cmake --preset release 2>&1 && cmake --build --preset release 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "Built: $root\data\SKSE\Plugins\MundaneMatters.dll"
