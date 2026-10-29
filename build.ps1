param(
    [ValidateSet('All', 'Link', 'Pointer')][string]$Project = 'All',
    [switch]$AndroidTools,
    [switch]$Test,
    [string]$AdbDirectory = '',
    [string]$AndroidBuildTools = '',
    [string]$AndroidJdk = '',
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
$mode = if ($AndroidTools) { 'android' } else { 'desktop' }
$buildPath = Join-Path $PSScriptRoot "build/$($Project.ToLowerInvariant())-$mode"
if ($BuildDirectory) {
    $buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $PSScriptRoot $BuildDirectory }
}
$configureArgs = @('-S', $PSScriptRoot, '-B', $buildPath, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DQINEX_BUILD_LINK=$($Project -ne 'Pointer')", "-DQINEX_BUILD_POINTER=$($Project -ne 'Link')",
    "-DQINEX_WITH_ANDROID_TOOLS=$($AndroidTools.IsPresent)", '-DBUILD_TESTING=ON')
foreach ($entry in @(@('QINEX_ADB_DIR', $AdbDirectory), @('QINEX_ANDROID_BUILD_TOOLS', $AndroidBuildTools), @('QINEX_ANDROID_JDK', $AndroidJdk))) {
    if ($entry[1]) { $configureArgs += "-D$($entry[0])=$($entry[1])" }
}
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
# 不复用 MSBuild 节点；关闭 FileTracker，避免部分 VS 环境中的编译进程挂起。
& cmake --build $buildPath --config Release --parallel 4 -- /p:TrackFileAccess=false /nr:false
if ($LASTEXITCODE -ne 0) { throw '编译失败。若程序正在运行，请先自行退出。' }
if ($Test) {
    & ctest --test-dir $buildPath -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '测试未通过。' }
}
Write-Host "构建完成：$buildPath" -ForegroundColor Green
