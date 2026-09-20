param([switch]$Clean, [switch]$Package)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
Push-Location $project
try {
    . "$PSScriptRoot/windows-dev-environment.ps1"
    $qt = Join-Path $project '.build-tools/Qt/6.11.1/msvc2022_64'
    if (-not (Test-Path "$qt/lib/cmake/Qt6/Qt6Config.cmake")) {
        throw 'Qt 6.11.1 is missing. Run scripts/install-windows-qt.py with requests and py7zr.'
    }
    if (-not (Test-Path '.build-tools/zlib/install/include/zlib.h')) {
        throw 'zlib 1.3.1 is missing. Follow the Windows backend build instructions.'
    }
    $env:Path = "$((Resolve-Path -LiteralPath (Join-Path $qt 'bin')).Path);$env:Path"
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $logs = Join-Path $project ".agent/logs/windows-$stamp"
    New-Item -ItemType Directory -Path $logs | Out-Null
    cmake --preset windows-qt-release *> "$logs/configure.log"
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $logs/configure.log" }
    $buildArgs = @('--build', '--preset', 'windows-qt-release', '-j', '6')
    if ($Clean) { $buildArgs += '--clean-first' }
    & cmake @buildArgs *> "$logs/build.log"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $logs/build.log" }
    ctest --preset windows-qt-release *> "$logs/ctest.log"
    Get-Content "$logs/ctest.log" -Tail 5
    if ($LASTEXITCODE -ne 0) { throw "Tests failed: $logs/ctest.log" }
    if ($Package) {
        # Every development package is a new immutable directory.
        $destination = Join-Path $project ".build-tools/packages/SeriousShot-windows-x64_$stamp"
        if (Test-Path -LiteralPath $destination) { throw 'Package destination already exists.' }
        New-Item -ItemType Directory -Path $destination | Out-Null
        Copy-Item -LiteralPath 'build-windows-qt/SeriousShot.exe' -Destination $destination
        Copy-Item -LiteralPath 'build-windows-qt/assets' -Destination $destination -Recurse
        & "$qt/bin/windeployqt.exe" --release --compiler-runtime --no-translations --no-opengl-sw "$destination/SeriousShot.exe" *> "$logs/deploy.log"
        if ($LASTEXITCODE -ne 0) { throw "Deploy failed: $logs/deploy.log" }
        $runtime = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath $runtime) {
            Get-ChildItem -LiteralPath $runtime -Filter '*.dll' | Copy-Item -Destination $destination
        }
        # Keep the codec notices and local assets with the development build.
        $licenses = New-Item -ItemType Directory -Path "$destination/licenses"
        Copy-Item -LiteralPath 'THIRD_PARTY_NOTICES.md' -Destination $licenses.FullName
        Copy-Item -LiteralPath 'assets/fonts/OFL.txt' -Destination $licenses.FullName
        Copy-Item -LiteralPath 'assets/color_profiles/CC0-1.0.txt' -Destination $licenses.FullName
        Copy-Item -LiteralPath 'build-windows-qt/_deps/ultrahdr-src/LICENSE' -Destination "$licenses/libultrahdr-LICENSE.txt"
        Copy-Item -LiteralPath 'build-windows-qt/_deps/ultrahdr-src/adobe-hdr-gain-map-license/NOTICE' -Destination "$licenses/Adobe-NOTICE.txt"
        Copy-Item -LiteralPath 'build-windows-qt/_deps/ultrahdr-src/third_party/turbojpeg/LICENSE.md' -Destination "$licenses/libjpeg-turbo-LICENSE.md"
        Copy-Item -LiteralPath '.build-tools/zlib/src/LICENSE' -Destination "$licenses/zlib-LICENSE.txt"
        Copy-Item -LiteralPath "$qt/sbom" -Destination $licenses.FullName -Recurse
        "Local development build. Release license/source-distribution review remains pending. Qt 6.11.1 is dynamically linked; https://www.qt.io/licensing/open-source-lgpl-obligations" |
            Set-Content -LiteralPath "$destination/DEVELOPMENT_BUILD.txt" -Encoding utf8
        Get-FileHash -LiteralPath "$destination/SeriousShot.exe" -Algorithm SHA256 | Format-List | Out-String |
            Set-Content -LiteralPath "$destination/SHA256.txt"
        Write-Output "Package=$destination"
    }
    Write-Output "Logs=$logs"
} finally { Pop-Location }
