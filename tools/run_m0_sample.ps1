<#
.SYNOPSIS
  M0 の実行検証: Gaia 同梱の布サンプル (S03_MultiLayerClothOnCollider) を短いフレーム数で走らせる。

.DESCRIPTION
  Gaia のサンプル設定には作者環境の絶対パス (D:\Code\Graphics\Gaia\...) が
  そのまま埋まっており、また numFrames が 3000 と検証には長すぎる。
  このスクリプトは設定を build\m0_sample\ に複製して補正してから実行する。
  元のサンプルは書き換えない。

  前提: tools\build_gaia_cloth.ps1 でビルド済みであること。
#>
[CmdletBinding()]
param(
    [int]$NumFrames = 3,
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $RepoRoot 'external\deps.env.ps1')

$GaiaRoot = Join-Path $RepoRoot 'external\Gaia'
$Exe      = Join-Path $RepoRoot "build\gaia-vbdcloth\$Config\GAIA_VBDCloth.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "$Exe が無い。先に tools\build_gaia_cloth.ps1 を実行すること。"
}

$SampleName = 'C70_CR0.3_Cs100000.0_bd10.0_step20_iters15_init2_accel0.0_damp2e-06_l5_fix_VBD_run2'
$SampleDir  = Join-Path $GaiaRoot "Simulator\VBDCloth\ParameterGen\Parameters\S03_MultiLayerClothOnCollider\$SampleName"
$WorkDir    = Join-Path $RepoRoot 'build\m0_sample'
$OutDir     = Join-Path $WorkDir 'out'

New-Item -ItemType Directory -Force -Path $WorkDir, $OutDir | Out-Null

# --- Models.json: 作者環境の絶対パスを ${REPO_ROOT} に直す ---------------
$models = Get-Content -LiteralPath (Join-Path $SampleDir 'Models.json') -Raw
$models = $models -replace [regex]::Escape('D:\\Code\\Graphics\\Gaia'), '${REPO_ROOT}'
$modelsPath = Join-Path $WorkDir 'Models.json'
Set-Content -LiteralPath $modelsPath -Value $models -Encoding UTF8

# --- Parameters.json: フレーム数を縮め、ビューアを無効化 -----------------
# BUILD_GUI=OFF でビルドしているので enableViewer が true のままだと
# GUINoCompliationError() が走ってプロセスが異常終了する (0xC0000409)。
$params = Get-Content -LiteralPath (Join-Path $SampleDir 'Parameters.json') -Raw | ConvertFrom-Json
$params.PhysicsParams.numFrames = $NumFrames
$params.ViewerParams.enableViewer = $false
$paramsPath = Join-Path $WorkDir 'Parameters.json'
$params | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $paramsPath -Encoding UTF8

Write-Host '=== M0 実行検証 ==='
Write-Host "  exe    : $Exe"
Write-Host "  frames : $NumFrames (元サンプルは 3000)"
Write-Host "  out    : $OutDir"
Write-Host ''

& $Exe $modelsPath $paramsPath $OutDir -R $GaiaRoot
$code = $LASTEXITCODE

Write-Host ''
Write-Host "終了コード: $code"

$produced = Get-ChildItem -LiteralPath $OutDir -Recurse -File -Include '*.ply', '*.obj' -ErrorAction SilentlyContinue
if ($produced) {
    Write-Host "出力メッシュ: $($produced.Count) 件"
    $produced | Select-Object -First 5 | ForEach-Object {
        Write-Host ("  {0}  ({1:N0} bytes)" -f $_.Name, $_.Length)
    }
} else {
    Write-Warning "出力メッシュが生成されていない。$OutDir を確認すること。"
}
