<#
.SYNOPSIS
  Gaia の VBDCloth スタンドアロン (GAIA_VBDCloth.exe) をビルドする。M0 の検証用。

.DESCRIPTION
  指示書 §8 の M0 は VBDDynamics を挙げているが、あちらはテトラメッシュ(ソリッド)用。
  布に必要なのは VBDCloth 側なのでこちらを M0 の対象にする(docs/decisions.md D-001)。

  前提: tools\setup_deps.ps1 を実行済みであること。
#>
[CmdletBinding()]
param(
    [string]$CudaArchitectures = '120',   # Blackwell (RTX 5070 Ti)
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Gui,                          # polyscope ビューアも一緒に建てる
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$EnvFile  = Join-Path $RepoRoot 'external\deps.env.ps1'
if (-not (Test-Path -LiteralPath $EnvFile)) {
    throw "$EnvFile が無い。先に tools\setup_deps.ps1 を実行すること。"
}
. $EnvFile

$SrcDir   = Join-Path $RepoRoot 'external\Gaia\Simulator\VBDCloth'
$GaiaDir  = Join-Path $RepoRoot 'external\Gaia\Simulator\CMake'
$BuildDir = Join-Path $RepoRoot 'build\gaia-vbdcloth'

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "[clean] $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

Write-Host '=== GAIA_VBDCloth ビルド ==='
Write-Host "  source : $SrcDir"
Write-Host "  build  : $BuildDir"
Write-Host "  arch   : sm_$CudaArchitectures"
Write-Host "  config : $Config"
Write-Host "  gui    : $($Gui.IsPresent)"

# ジェネレータは必ず明示する。この開発機の PATH には MinGW GCC + ninja があり、
# 放置すると CMake が GCC を選んで CUDA/MSVC 前提の Gaia が壊れる (D-003)。
$cmakeArgs = @(
    '-S', $SrcDir,
    '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DGAIA_DIR=$GaiaDir",
    "-DEigen3_DIR=$env:Eigen3_DIR",
    "-DTBB_DIR=$env:TBB_DIR",
    "-Dembree_DIR=$env:embree_DIR",
    "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures",
    "-DBUILD_GUI=$(if ($Gui) { 'ON' } else { 'OFF' })",
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
)

Write-Host "`n--- configure ---"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure に失敗 (exit $LASTEXITCODE)" }

Write-Host "`n--- build ---"
& cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build に失敗 (exit $LASTEXITCODE)" }

$exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter 'GAIA_VBDCloth.exe' -ErrorAction SilentlyContinue |
       Select-Object -First 1
if ($exe) {
    Write-Host "`nビルド成功: $($exe.FullName)"
} else {
    throw 'ビルドは通ったが GAIA_VBDCloth.exe が見つからない'
}
