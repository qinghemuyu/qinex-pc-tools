param([switch]$Test)
& (Join-Path $PSScriptRoot '../build.ps1') -Project Link @PSBoundParameters
