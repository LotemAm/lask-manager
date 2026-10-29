# Builds a release and packs the installer-free zip: dist\LaskManager-<version>-x64.zip holding
# lask.exe and README.md. Copy the exe anywhere and run it; nothing to install.
#
#   .\scripts\package.ps1              # Visual Studio generator
#   .\scripts\package.ps1 -Preset ninja  # from a VS developer prompt
param([ValidateSet("vs", "ninja")][string]$Preset = "vs")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset "$Preset-release"
    if ($LASTEXITCODE) { throw "build failed" }

    $version = (Select-String -Path CMakeLists.txt -Pattern '^\s*VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
    $exe = Join-Path $root "build\$Preset\Release\lask.exe"

    # The number that matters, measured on the machine that packs it.
    $bench = (& $exe --bench | Out-String).Trim()
    Write-Host "startup: $bench"

    $name = "LaskManager-$version-x64"
    $stage = Join-Path $root "dist\$name"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $exe, (Join-Path $root "README.md") $stage

    $zip = Join-Path $root "dist\$name.zip"
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    Write-Host "wrote $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB, sha256 $hash)"
} finally {
    Pop-Location
}
