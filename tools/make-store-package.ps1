# Builds the Microsoft Store upload: winui\AppPackages\DockDebug_<version>_x64_arm64.msixupload.
#
# Builds a Release MSIX for x64 and for ARM64, bundles them with makeappx into a .msixbundle, and zips
# the bundle together with both .appxsym symbol packages (for crash reports in Partner Center) into a
# .msixupload. The packages are unsigned; the Store signs them during certification.
#
# The version comes from winui\Package.appxmanifest (Identity/@Version). Every Store submission needs a
# higher version than the last one, and the fourth number must stay 0.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools\make-store-package.ps1
$ErrorActionPreference = 'Stop'

$winui = Join-Path $PSScriptRoot '..\winui' | Resolve-Path
$project = Join-Path $winui 'DockDebug.vcxproj'
$outDir = Join-Path $winui 'AppPackages'
$platforms = @('x64', 'ARM64')

[xml]$manifest = Get-Content (Join-Path $winui 'Package.appxmanifest')
$version = $manifest.Package.Identity.Version

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found. Install Visual Studio with the C++ workload.' }

$makeappx = Get-ChildItem (Join-Path $winui 'packages') -Recurse -Filter makeappx.exe |
    Where-Object { $_.Directory.Name -eq 'x64' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) { throw 'makeappx.exe not found. Restore NuGet packages first (see README).' }

$staging = Join-Path $outDir "staging_$version"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force (Join-Path $staging 'bundle') | Out-Null

foreach ($platform in $platforms) {
    Write-Host "Building $platform ..."
    # The per-platform packages and sideload test folders MSBuild writes are not needed; keep them in staging.
    & $msbuild $project /nologo /v:minimal /p:Configuration=Release /p:Platform=$platform `
        /p:UapAppxPackageBuildMode=StoreUpload "/p:AppxPackageDir=$staging\msbuild/"
    if ($LASTEXITCODE -ne 0) { throw "Build for $platform failed." }

    $upload = Join-Path $winui "$platform\Release\DockDebug\Upload\DockDebug_${version}_$platform"
    Copy-Item (Join-Path $upload "DockDebug_${version}_$platform.msix") (Join-Path $staging 'bundle')
    Copy-Item (Join-Path $upload "DockDebug_${version}_$platform.appxsym") $staging
}

$name = "DockDebug_${version}_x64_arm64"
$bundle = Join-Path $staging "$name.msixbundle"
& $makeappx bundle /d (Join-Path $staging 'bundle') /p $bundle /bv $version /o
if ($LASTEXITCODE -ne 0) { throw 'makeappx bundle failed.' }

# A .msixupload is a zip of the bundle and the symbol packages.
$upload = Join-Path $outDir "$name.msixupload"
if (Test-Path $upload) { Remove-Item -Force $upload }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($upload, 'Create')
try {
    foreach ($file in @($bundle) + @(Get-ChildItem $staging -Filter *.appxsym | ForEach-Object FullName)) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file, (Split-Path $file -Leaf)) | Out-Null
    }
} finally {
    $zip.Dispose()
}
Remove-Item -Recurse -Force $staging

Write-Host "Store upload: $upload"
