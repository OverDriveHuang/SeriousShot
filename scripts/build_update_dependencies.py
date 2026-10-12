#!/usr/bin/env python3
"""Build pinned upstream libsodium locally; no system install or private keys."""
import os, hashlib, pathlib, subprocess, sys, tarfile, urllib.request, shutil
root=pathlib.Path(__file__).resolve().parents[1]
cache=root/'.build-tools'; cache.mkdir(exist_ok=True)
version='1.0.22'; digest='adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349'
archive=cache/f'libsodium-{version}.tar.gz'
if not archive.exists(): urllib.request.urlretrieve(f'https://download.libsodium.org/libsodium/releases/{archive.name}',archive)
if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest: raise SystemExit('libsodium hash mismatch')
source=cache/f'libsodium-{version}'
if not source.exists():
    with tarfile.open(archive) as t:
        for member in t.getmembers():
            if not (cache / member.name).resolve().is_relative_to(cache.resolve()) or member.issym() or member.islnk(): raise SystemExit('unsafe dependency archive')
        t.extractall(cache)
install=cache/'libsodium-install'; (install/'lib').mkdir(parents=True,exist_ok=True)
if sys.platform=='win32':
    # Upstream ReleaseLIB defaults /MT; match Qt's /MD without changing sodium code.
    crtprops=source/'builds/msvc/properties/ReleaseLIB.props'
    crtprops.write_text(crtprops.read_text().replace('<RuntimeLibrary>MultiThreaded</RuntimeLibrary>','<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>'))
    project=source/'builds/msvc/vs2022/libsodium/libsodium.vcxproj'
    subprocess.run(['msbuild',str(project),'/p:Configuration=ReleaseLIB','/p:Platform=x64',f'/p:OutDir={install / "lib"}\\','/p:TargetName=libsodium','/m'],check=True)
    shutil.copytree(source/'src/libsodium/include',install/'include',dirs_exist_ok=True)
else:
    buildenv=os.environ.copy()
    if sys.platform=='darwin':
        buildenv['MACOSX_DEPLOYMENT_TARGET']='15.5'
        buildenv['CFLAGS']='-O2 -mmacosx-version-min=15.5'
    subprocess.run(['make','clean'],cwd=source,check=False,env=buildenv)
    subprocess.run(['./configure','--disable-shared','--enable-static','--with-pic',f'--prefix={install}'],cwd=source,check=True,env=buildenv)
    subprocess.run(['make','-j4'],cwd=source,check=True,env=buildenv)
    subprocess.run(['make','install'],cwd=source,check=True,env=buildenv)
print(f'libsodium {version} source SHA-256 {digest}; install: {install}')

archive_version='3.8.9'; archive_digest='888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde'
a=cache/f'libarchive-{archive_version}.tar.xz'
if not a.exists(): urllib.request.urlretrieve(f'https://www.libarchive.org/downloads/{a.name}',a)
if hashlib.sha256(a.read_bytes()).hexdigest()!=archive_digest: raise SystemExit('libarchive hash mismatch')
if not (cache/f'libarchive-{archive_version}').exists():
    with tarfile.open(a) as t:
        for member in t.getmembers():
            if not (cache / member.name).resolve().is_relative_to(cache.resolve()) or member.issym() or member.islnk(): raise SystemExit('unsafe dependency archive')
        t.extractall(cache)
print(f'libarchive {archive_version} source SHA-256 {archive_digest}')
