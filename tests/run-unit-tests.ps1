# Builds and runs the unit tests (tests\UnitTests). Exit code 0 when all tests pass.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tests\run-unit-tests.ps1 [-Configuration Release] [-Platform ARM64]
param(
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found. Install Visual Studio with the C++ workload.' }

$project = Join-Path $PSScriptRoot 'UnitTests\UnitTests.vcxproj'
& $msbuild $project /nologo /v:minimal /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) { throw 'Building the unit tests failed.' }

& (Join-Path $PSScriptRoot "UnitTests\$Platform\$Configuration\UnitTests.exe")
exit $LASTEXITCODE
