<#
.SYNOPSIS
  haori-server 本体をビルドする。

.DESCRIPTION
  cpp-httplib / nlohmann-json / spdlog は FetchContent で取得する(初回のみネットワークを使う)。
  M2 時点では Gaia にリンクしていないので、setup_deps.ps1 は不要。
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [string]$CudaArchitectures = '120',   # Blackwell (RTX 5070 Ti)
    [switch]$NoGaia,                       # Gaia を外して codec/jobs だけ建てる
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $RepoRoot 'build\server'

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "[clean] $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$cmakeArgs = @(
    '-S', $RepoRoot,
    '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64'
)

if ($NoGaia) {
    $cmakeArgs += '-DHAORI_WITH_GAIA=OFF'
} else {
    # Gaia は Eigen / TBB / Embree を find_package する。setup_deps.ps1 が書いたパスを渡す。
    $EnvFile = Join-Path $RepoRoot 'external\deps.env.ps1'
    if (-not (Test-Path -LiteralPath $EnvFile)) {
        throw "$EnvFile が無い。先に tools\setup_deps.ps1 を実行すること。"
    }
    . $EnvFile

    $cmakeArgs += @(
        '-DHAORI_WITH_GAIA=ON',
        "-DEigen3_DIR=$env:Eigen3_DIR",
        "-DTBB_DIR=$env:TBB_DIR",
        "-Dembree_DIR=$env:embree_DIR",
        "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures",
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
    )
}

Write-Host '=== haori-server ビルド ==='
Write-Host "  gaia   : $(-not $NoGaia)"
if (-not $NoGaia) { Write-Host "  arch   : sm_$CudaArchitectures" }
Write-Host "  config : $Config"

# ジェネレータの明示は必須。PATH の MinGW GCC に持っていかれる (docs/decisions.md D-003)。
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure に失敗 (exit $LASTEXITCODE)" }

& cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build に失敗 (exit $LASTEXITCODE)" }

$exe = Join-Path $BuildDir "$Config\haori-server.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "haori-server.exe が見つからない" }
Write-Host "`nビルド成功: $exe"

if (-not $SkipTests) {
    Write-Host "`n=== ユニットテスト ==="
    & (Join-Path $BuildDir "$Config\haori_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "ユニットテストが失敗した (exit $LASTEXITCODE)" }
}
