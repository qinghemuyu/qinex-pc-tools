# 从已测试的默认桌面构建生成带许可证的本地运行包。
param([string]$BuildDirectory = 'build/all-desktop', [switch]$Release)
$ErrorActionPreference = 'Stop'
$releaseTag = ''
$releaseRevision = ''
if ($Release) {
    $changes = & git -C $PSScriptRoot status --porcelain
    if ($LASTEXITCODE -ne 0 -or $changes) { throw '发布打包要求先提交全部源码修改，工作区必须干净。' }
    $releaseTag = & git -C $PSScriptRoot describe --tags --exact-match HEAD
    if ($LASTEXITCODE -ne 0) { throw '发布打包要求 HEAD 已有版本标签。' }
    $releaseRevision = & git -C $PSScriptRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw '无法读取发布提交。' }
}
& (Join-Path $PSScriptRoot 'build.ps1') -Test -BuildDirectory $BuildDirectory
if ($Release) {
    $currentRevision = & git -C $PSScriptRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $currentRevision -ne $releaseRevision) { throw '构建期间源码提交发生变化，停止发布打包。' }
    $changes = & git -C $PSScriptRoot status --porcelain
    if ($LASTEXITCODE -ne 0 -or $changes) { throw '构建期间源码发生变化，停止发布打包。' }
}
$buildRoot = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $PSScriptRoot $BuildDirectory }
$stageRoot = Join-Path $PSScriptRoot ('build/package/' + [guid]::NewGuid().ToString('N'))
$outputRoot = Join-Path $PSScriptRoot 'dist/desktop'
New-Item -ItemType Directory -Path $stageRoot, $outputRoot -Force | Out-Null
foreach ($project in @('qinex-link', 'qinex-pointer')) {
    $name = if ($project -eq 'qinex-link') { 'QInEX-Link' } else { 'QInEX-Pointer' }
    $stage = Join-Path $stageRoot $name
    New-Item -ItemType Directory -Path $stage | Out-Null
    $files = @((Join-Path $buildRoot "$project/Release/$name.exe"))
    foreach ($common in @('LICENSE.txt', 'NOTICE.txt', 'THIRD_PARTY_NOTICES.md')) { $files += Join-Path $PSScriptRoot $common }
    if ($project -eq 'qinex-link') {
        $files += Join-Path $PSScriptRoot 'qinex-link/THIRD_PARTY_NOTICES.txt'
        $files += Join-Path $PSScriptRoot 'qinex-link/后台性能说明.txt'
        $files += Join-Path $PSScriptRoot 'qinex-pointer/使用说明.txt'
        $files += Join-Path $PSScriptRoot 'qinex-link/合并版使用.txt'
    } else { $files += Join-Path $PSScriptRoot 'qinex-pointer/使用说明.txt' }
    foreach ($file in $files) { Copy-Item -LiteralPath $file -Destination $stage }
    $revision = & git -C $PSScriptRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw '无法读取构建源码版本。' }
    $changes = & git -C $PSScriptRoot status --porcelain
    if ($LASTEXITCODE -ne 0) { throw '无法读取工作区状态。' }
    $sourceState = if ($changes) { '包含未提交的本地修改；不是该提交的原样构建。' } else { '源码工作区干净。' }
    $buildKind = if ($Release) { "源码标签：$releaseTag" } else { '本地开发包（非发布构建）。' }
    @("源码基线：$revision", "源码状态：$sourceState", $buildKind, '构建组合：Windows x64 / Release / 桌面版（Android 工具关闭）', '未经真实 P4 与采集设备验收。') | Set-Content -LiteralPath (Join-Path $stage 'BUILD-INFO.txt') -Encoding UTF8
    $archive = Join-Path $outputRoot "$name-windows-x64.zip"
    $temporary = Join-Path $stageRoot "$name.zip"
    Compress-Archive -LiteralPath $stage -DestinationPath $temporary
    if (Test-Path -LiteralPath $archive) { Move-Item -LiteralPath $archive -Destination "$archive.previous" -Force }
    Move-Item -LiteralPath $temporary -Destination $archive
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
}
$checksums = foreach ($name in @('QInEX-Link-windows-x64.zip', 'QInEX-Pointer-windows-x64.zip')) {
    $hash = Get-FileHash -LiteralPath (Join-Path $outputRoot $name) -Algorithm SHA256
    "$($hash.Hash.ToLowerInvariant())  $name"
}
$checksums | Set-Content -LiteralPath (Join-Path $outputRoot 'SHA256SUMS.txt') -Encoding ASCII
