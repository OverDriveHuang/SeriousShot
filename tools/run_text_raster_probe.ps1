$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
Push-Location $project
try {
    . ./scripts/windows-dev-environment.ps1
    $qt = (Resolve-Path '.build-tools/Qt/6.11.1/msvc2022_64').Path
    $stamp = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([DateTime]::UtcNow, 'China Standard Time').ToString('yyyy-MM-dd_HH-mm-ss')
    $probe = "build/text-raster-probe/$stamp"
    $output = "build/text-raster-probe-results/$stamp"
    New-Item -ItemType Directory -Force $probe,$output | Out-Null
    & cl /nologo /std:c++20 /EHsc /MD /utf-8 /Zc:__cplusplus /DQT_FREETYPE_LIB /Isrc /I"$qt/include" /I"$qt/include/QtCore" /I"$qt/include/QtGui" /I"$qt/include/QtWidgets" /I"$qt/include/QtFreetype" tools/text_raster_probe.cpp src/platform/freetype/freetype_text_rasterizer_port.cpp /Fo"$probe/" /Fe"$probe/probe.exe" /link /LIBPATH:"$qt/lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6BundledFreetype.lib Qt6BundledLibpng.lib user32.lib gdi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed.' }
    $env:Path="$qt/bin;$env:Path"
    & "$probe/probe.exe" $output | Tee-Object "$output/probe.txt"
    if ($LASTEXITCODE -ne 0) { throw 'Probe failed.' }
    Write-Output "Evidence=$project/$output"
} finally { Pop-Location }
