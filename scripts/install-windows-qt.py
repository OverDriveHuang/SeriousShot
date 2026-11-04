"""Install the pinned official Qt SDK in the project, checking Qt's hashes.

Run with an isolated Python containing requests and py7zr (aqt's dependencies).
Qt 6.11 uses per-architecture repositories unsupported by aqt 3.3's resolver.
"""
from pathlib import Path
import hashlib
import subprocess
import requests
import py7zr

root = Path(__file__).resolve().parents[1] / '.build-tools' / 'Qt'
base = ('https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/'
        'qt6_6112/qt6_6112_msvc2022_64/qt.qt6.6112.win64_msvc2022_64/')
suffix = '-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
root.mkdir(parents=True, exist_ok=True)
sdk = root / '6.11.2' / 'msvc2022_64'
sdk.mkdir(parents=True, exist_ok=True)
hashes = {
    'qtbase': '1fca9483183d426c1a3a984f1382382fa51f48de',
    'qtdeclarative': 'f3ac5149eb76cb0279ba84e126734cf520214519',
    'qttools': '0a94da47fb97103660b5bfec7361ae51b05f7323',
    'qtsvg': 'f458d7f238795d61a3e53fa5211cf2d6b1aa08ef',
    'qttranslations': '2826dd1fcb9489d243c25a812f3edd7319ca2e45',
}
for component, pinned_hash in hashes.items():
    name = '6.11.2-0-202608131017' + component + suffix
    checksum = requests.get(base + name + '.sha1', timeout=60)
    checksum.raise_for_status()
    expected = checksum.text.strip().split()[0].lower()
    if expected != pinned_hash:
        raise RuntimeError('Official checksum differs from pinned SDK: ' + name)
    if len(expected) != 40 or any(c not in '0123456789abcdef' for c in expected):
        raise RuntimeError('Invalid official checksum: ' + name)
    archive = root / name
    if not archive.exists():
        response = requests.get(base + name, stream=True, timeout=60)
        response.raise_for_status()
        with archive.open('wb') as output:
            for data in response.iter_content(1024 * 1024):
                output.write(data)
    digest = hashlib.sha1(archive.read_bytes()).hexdigest()
    if digest != expected:
        raise RuntimeError('Qt archive hash mismatch: ' + name)
    print(name, 'sha1=' + digest, 'verified', flush=True)
    with py7zr.SevenZipFile(archive) as package:
        package.extractall(sdk)
qt_bin = sdk / 'bin'
(qt_bin / 'qt.conf').write_text('[Paths]\nPrefix=..\n', encoding='utf-8')
for tool in ('lupdate.exe', 'lrelease.exe'):
    subprocess.run([str(qt_bin / tool), '-version'], check=True)
for locale in ('en', 'zh_CN'):
    catalog = sdk / 'translations' / f'qtbase_{locale}.qm'
    if not catalog.is_file():
        raise RuntimeError(f'Missing Qt standard translation: {catalog}')
print('Qt SDK:', qt_bin.parent, flush=True)
