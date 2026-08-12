<#
.SYNOPSIS
  haori-server を既定設定 (gaia / 127.0.0.1:8787) で起動する。

.DESCRIPTION
  Embree/TBB の DLL パスを通してからサーバーを起動するだけの薄いラッパー。
  deps.env.ps1 のドットソースを忘れると起動時に DLL が見つからず落ちる。
  設定を変えたい場合は build\server\Release\haori-server.exe を直接叩くこと
  (--host / --port / --engine / --log)。
#>
$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot

$EnvFile = Join-Path $RepoRoot 'external\deps.env.ps1'
if (-not (Test-Path -LiteralPath $EnvFile)) {
    throw "$EnvFile が無い。先に tools\setup_deps.ps1 を実行すること。"
}
. $EnvFile

$Exe = Join-Path $RepoRoot 'build\server\Release\haori-server.exe'
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "$Exe が無い。先に tools\build_server.ps1 を実行すること。"
}

Write-Host '=== haori-server 起動 ==='
Write-Host "  exe : $Exe"
Write-Host '  url : http://127.0.0.1:8787'
Write-Host '  停止: Ctrl+C'
Write-Host ''

& $Exe --host 127.0.0.1 --port 8787 --engine gaia --log info
exit $LASTEXITCODE
