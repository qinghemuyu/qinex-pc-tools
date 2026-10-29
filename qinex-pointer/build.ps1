param([switch]$Test, [switch]$AndroidTools, [string]$AdbDirectory = '', [string]$AndroidBuildTools = '', [string]$AndroidJdk = '')
& (Join-Path $PSScriptRoot '../build.ps1') -Project Pointer @PSBoundParameters
