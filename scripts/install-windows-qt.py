"""Install the pinned official Qt base SDK in the project, checking Qt's hashes.

Run with an isolated Python containing requests and py7zr (aqt's dependencies).
Qt 6.11 uses per-architecture repositories unsupported by aqt 3.3's resolver.
"""
from pathlib import Path
import hashlib
import requests
import py7zr

root = Path(__file__).resolve().parents[1] / '.build-tools' / 'Qt'
base = ('https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/'
        'qt6_6111/qt6_6111_msvc2022_64/qt.qt6.6111.win64_msvc2022_64/')
suffix = '-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
root.mkdir(parents=True, exist_ok=True)
sdk = root / '6.11.1' / 'msvc2022_64'
sdk.mkdir(parents=True, exist_ok=True)
hashes = {
    'qtbase': '6f554628540ab947d48294e208cc3caae7f023d2',
    'qttools': 'f8f33d511bbf38b85aa701425b41455b3512b36d',
    'qtsvg': '353d70f58e5cd55d4217e3ec806a4b5dad4d320b',
    'qttranslations': '0ed00fbd45f905f27ad18f35c0e5410857846209',
}
for component in ('qtbase', 'qttools', 'qtsvg', 'qttranslations'):
    name = '6.11.1-0-202605090529' + component + suffix
    checksum = requests.get(base + name + '.sha1', timeout=60)
    checksum.raise_for_status()
    expected = checksum.text.strip().split()[0].lower()
    if expected != hashes[component]:
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
print('Qt SDK:', qt_bin.parent, flush=True)
