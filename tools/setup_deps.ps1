<#
.SYNOPSIS
  haori-server / Gaia のビルドに必要なサードパーティ依存を取得して展開する。

.DESCRIPTION
  Gaia は以下の3つを要求する(バージョンは Gaia README の「テスト済み」に合わせる):
    - Eigen  3.4.0        (ヘッダオンリー)
    - oneTBB 2021.12.0    (Windows プリビルド)
    - Embree 3.13.1       (Windows プリビルド。★4系は非互換なので使わないこと)

  vcpkg を使わず公式プリビルドを直接展開する。理由は docs/decisions.md の D-002 を参照。
  展開先 external/thirdparty/ は .gitignore 済みなので、このスクリプトが唯一の入手手段になる。

.PARAMETER Force
  既に展開済みでも再ダウンロード・再展開する。
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # Invoke-WebRequest の進捗バーは遅いので抑止

$RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ThirdParty = Join-Path $RepoRoot 'external\thirdparty'
$Downloads  = Join-Path $RepoRoot 'external\downloads'

New-Item -ItemType Directory -Force -Path $ThirdParty, $Downloads | Out-Null

# name / url / 展開後に必ず存在するはずのディレクトリ名
$Deps = @(
    @{
        Name    = 'eigen-3.4.0'
        Url     = 'https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip'
        Archive = 'eigen-3.4.0.zip'
        RootDir = 'eigen-3.4.0'
    },
    @{
        Name    = 'oneapi-tbb-2021.12.0'
        Url     = 'https://github.com/uxlfoundation/oneTBB/releases/download/v2021.12.0/oneapi-tbb-2021.12.0-win.zip'
        Archive = 'oneapi-tbb-2021.12.0-win.zip'
        RootDir = 'oneapi-tbb-2021.12.0'
    },
    @{
        Name    = 'embree-3.13.1'
        Url     = 'https://github.com/RenderKit/embree/releases/download/v3.13.1/embree-3.13.1.x64.vc14.windows.zip'
        Archive = 'embree-3.13.1.x64.vc14.windows.zip'
        RootDir = 'embree-3.13.1.x64.vc14.windows'
    },

    # --- ここから下は haori-server 本体 (Gaia とは無関係) ---------------------
    # CMake の FetchContent に取りに行かせない理由は docs/decisions.md D-006 を参照
    # (PATH の MinGW 版 CMake は CA 証明書を持たず TLS 検証に失敗する)。
    @{
        Name    = 'cpp-httplib-0.18.3'
        Url     = 'https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.zip'
        Archive = 'cpp-httplib-0.18.3.zip'
        RootDir = 'cpp-httplib-0.18.3'
    },
    @{
        Name    = 'nlohmann-json-3.11.3'
        Url     = 'https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip'
        Archive = 'nlohmann-json-3.11.3.zip'
        RootDir = 'json-3.11.3'
    },
    @{
        Name    = 'spdlog-1.14.1'
        Url     = 'https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip'
        Archive = 'spdlog-1.14.1.zip'
        RootDir = 'spdlog-1.14.1'
    }
)

Write-Host '=== haori-server 依存セットアップ ==='
Write-Host "展開先: $ThirdParty"

foreach ($dep in $Deps) {
    $target = Join-Path $ThirdParty $dep.RootDir

    if ((Test-Path -LiteralPath $target) -and -not $Force) {
        Write-Host "  [skip]     $($dep.Name)  (既に存在)"
        continue
    }

    $archive = Join-Path $Downloads $dep.Archive
    if ((-not (Test-Path -LiteralPath $archive)) -or $Force) {
        Write-Host "  [download] $($dep.Name)"
        Invoke-WebRequest -Uri $dep.Url -OutFile $archive -UseBasicParsing -TimeoutSec 600
    }
    else {
        Write-Host "  [cached]   $($dep.Archive)"
    }

    Write-Host "  [expand]   $($dep.Name)"
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    Expand-Archive -LiteralPath $archive -DestinationPath $ThirdParty -Force

    if (-not (Test-Path -LiteralPath $target)) {
        throw "展開後に $target が見つからない。アーカイブ構成が変わった可能性がある: $($dep.Url)"
    }
}

# ---- Eigen は「ソース配布」なので install を1回通して config を生成する -----
# 配布 zip に入っているのは cmake/Eigen3Config.cmake.in (テンプレート) だけで、
# find_package(Eigen3 CONFIG) が探す Eigen3Config.cmake は install 時に生成される。
# ヘッダオンリーなのでコンパイルは走らず、数秒で終わる。
$eigenSrc    = Join-Path $ThirdParty 'eigen-3.4.0'
$eigenBuild  = Join-Path $ThirdParty '_eigen-build'
$eigenPrefix = Join-Path $ThirdParty 'eigen3'

if ((-not (Test-Path -LiteralPath $eigenPrefix)) -or $Force) {
    Write-Host '  [install]  eigen-3.4.0 (ヘッダオンリー)'
    if (Test-Path -LiteralPath $eigenBuild) { Remove-Item -LiteralPath $eigenBuild -Recurse -Force }

    # ジェネレータを明示する理由(重要):
    # この開発機の PATH には MinGW-w64 GCC と ninja が入っており、CMake は放っておくと
    # Ninja + GCC を選んでしまう。Gaia は CUDA + MSVC 前提なので必ず VS ジェネレータを指定する。
    # 詳細は docs/decisions.md の D-003 を参照。
    $genArgs = @('-G', 'Visual Studio 17 2022', '-A', 'x64')

    & cmake -S $eigenSrc -B $eigenBuild @genArgs `
        "-DCMAKE_INSTALL_PREFIX=$eigenPrefix" `
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
        '-DBUILD_TESTING=OFF' 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Eigen の cmake configure に失敗した' }

    & cmake --install $eigenBuild 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Eigen の cmake --install に失敗した' }

    Remove-Item -LiteralPath $eigenBuild -Recurse -Force
}
else {
    Write-Host '  [skip]     eigen3 install (既に存在)'
}

# ---- Gaia の find_package が必要とするパスを解決する ----------------------
# Gaia の Simulator/CMake/GAIA-config.cmake は
#   find_package(Eigen3 REQUIRED) / find_package(embree 3.0 REQUIRED) / find_package(TBB REQUIRED)
# を呼ぶので、それぞれの *-config.cmake があるディレクトリを指す必要がある。

$eigenDir = Join-Path $ThirdParty 'eigen-3.4.0\cmake'
$tbbDir   = Join-Path $ThirdParty 'oneapi-tbb-2021.12.0\lib\cmake\tbb'
$embDir   = Join-Path $ThirdParty 'embree-3.13.1.x64.vc14.windows\lib\cmake\embree-3.13.1'

# 実際の config ファイル位置を実測で確認する(構成が版によって違うため決め打ちしない)
function Find-ConfigDir {
    param([string]$Base, [string[]]$Patterns)
    foreach ($p in $Patterns) {
        $hit = Get-ChildItem -LiteralPath $Base -Recurse -File -Filter $p -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.Directory.FullName }
    }
    return $null
}

$eigenDir = Find-ConfigDir -Base $eigenPrefix `
                           -Patterns @('Eigen3Config.cmake', 'eigen3-config.cmake')
$tbbDir   = Find-ConfigDir -Base (Join-Path $ThirdParty 'oneapi-tbb-2021.12.0') `
                           -Patterns @('TBBConfig.cmake', 'tbb-config.cmake')
$embDir   = Find-ConfigDir -Base (Join-Path $ThirdParty 'embree-3.13.1.x64.vc14.windows') `
                           -Patterns @('embree-config.cmake', 'embreeConfig.cmake')

Write-Host ''
Write-Host '=== 検出した CMake config パス ==='
Write-Host "  Eigen3_DIR : $eigenDir"
Write-Host "  TBB_DIR    : $tbbDir"
Write-Host "  embree_DIR : $embDir"

foreach ($pair in @(@('Eigen3_DIR', $eigenDir), @('TBB_DIR', $tbbDir), @('embree_DIR', $embDir))) {
    if (-not $pair[1]) { throw "$($pair[0]) を解決できなかった。展開結果を確認すること。" }
}

# 後続のビルドスクリプトが読む形で書き出す(環境変数を汚さない)
$envFile = Join-Path $RepoRoot 'external\deps.env.ps1'
@"
# tools/setup_deps.ps1 が自動生成。手で編集しないこと。
`$env:Eigen3_DIR = '$eigenDir'
`$env:TBB_DIR    = '$tbbDir'
`$env:embree_DIR = '$embDir'
`$env:PATH       = '$(Split-Path -Parent (Join-Path $ThirdParty 'embree-3.13.1.x64.vc14.windows\bin\embree3.dll'))' + ';' + `$env:PATH
"@ | Set-Content -LiteralPath $envFile -Encoding UTF8

Write-Host ''
Write-Host "依存セットアップ完了。ビルド前に次を読み込むこと:  . .\external\deps.env.ps1"
