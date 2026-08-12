<#
.SYNOPSIS
  external/Gaia に haori-server 用のパッチを適用する。

.DESCRIPTION
  指示書 §9 に従い、Gaia 本体の修正は fork せずパッチとして管理する。
  submodule を clone しなおした直後に一度実行すればよい。適用済みなら何もしない。

  各パッチの意図は docs/gaia-survey.md「ビルドに必要だった修正」を参照。
#>
[CmdletBinding()]
param(
    [switch]$Revert,
    [switch]$Check     # 適用状況を表示するだけ
)

$ErrorActionPreference = 'Stop'

$RepoRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$GaiaDir   = Join-Path $RepoRoot 'external\Gaia'
$PatchDir  = Join-Path $RepoRoot 'patches'

if (-not (Test-Path -LiteralPath (Join-Path $GaiaDir '.git'))) {
    throw "external/Gaia が無い。先に git submodule update --init --recursive を実行すること。"
}

$patches = Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' | Sort-Object Name
if (-not $patches) { throw "patches/ に .patch が無い" }

foreach ($patch in $patches) {
    # --check で適用可否を判定してから実際に当てる(冪等にするため)
    & git -C $GaiaDir apply --check --reverse $patch.FullName 2>$null
    $alreadyApplied = ($LASTEXITCODE -eq 0)

    if ($Check) {
        "{0,-34} {1}" -f $patch.Name, $(if ($alreadyApplied) { '適用済み' } else { '未適用' })
        continue
    }

    if ($Revert) {
        if ($alreadyApplied) {
            & git -C $GaiaDir apply --reverse $patch.FullName
            if ($LASTEXITCODE -ne 0) { throw "revert 失敗: $($patch.Name)" }
            "  [revert]  $($patch.Name)"
        } else {
            "  [skip]    $($patch.Name) (未適用)"
        }
        continue
    }

    if ($alreadyApplied) {
        "  [skip]    $($patch.Name) (適用済み)"
        continue
    }

    & git -C $GaiaDir apply --check $patch.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "パッチが当たらない: $($patch.Name)。Gaia 側が更新された可能性があるので patches/ を作り直すこと。"
    }
    & git -C $GaiaDir apply $patch.FullName
    if ($LASTEXITCODE -ne 0) { throw "apply 失敗: $($patch.Name)" }
    "  [apply]   $($patch.Name)"
}
