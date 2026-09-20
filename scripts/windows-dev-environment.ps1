# Dot-source this script to activate the existing MSVC/SDK/CMake installation.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$seriousShotVsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$seriousShotVsPath = & $seriousShotVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $seriousShotVsPath) { throw 'Visual Studio Build Tools with MSVC x64 is required.' }
$seriousShotDevCmd = Join-Path $seriousShotVsPath 'Common7\Tools\VsDevCmd.bat'
$seriousShotDevCommand = "`"$seriousShotDevCmd`" -no_logo -arch=x64 -host_arch=x64 && set"
& $env:ComSpec /d /c $seriousShotDevCommand | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if ($LASTEXITCODE -ne 0) { throw 'VS developer environment failed.' }
$seriousShotCMakeRoot = Join-Path $seriousShotVsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:Path = "$(Join-Path $seriousShotCMakeRoot 'CMake\bin');$(Join-Path $seriousShotCMakeRoot 'Ninja');$env:Path"
$env:VSLANG = '1033'
