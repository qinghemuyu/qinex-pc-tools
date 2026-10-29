# 从已测试的默认桌面构建生成带许可证的本地运行包。
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Test
$buildRoot = Join-Path $PSScriptRoot 'build/all-desktop'
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
    } else { $files += Join-Path $PSScriptRoot 'qinex-pointer/使用说明.txt' }
    foreach ($file in $files) { Copy-Item -LiteralPath $file -Destination $stage }
    $archive = Join-Path $outputRoot "$name-windows-x64.zip"
    $temporary = Join-Path $stageRoot "$name.zip"
    Compress-Archive -LiteralPath $stage -DestinationPath $temporary
    if (Test-Path -LiteralPath $archive) { Move-Item -LiteralPath $archive -Destination "$archive.previous" -Force }
    Move-Item -LiteralPath $temporary -Destination $archive
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
}
