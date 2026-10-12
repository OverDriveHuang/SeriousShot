#!/usr/bin/env python3
"""Generate a complete managed inventory after deployment/signing. Never signs a catalog."""
import argparse, hashlib, json, pathlib, re, plistlib
p=argparse.ArgumentParser();p.add_argument('root',type=pathlib.Path);p.add_argument('--version',required=True);p.add_argument('--platform',choices=['windows','macos'],required=True);p.add_argument('--architecture',choices=['x64','arm64'],required=True);p.add_argument('--entrypoint',required=True);p.add_argument('--output',type=pathlib.Path)
a=p.parse_args();root=a.root.resolve(strict=True)
if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',a.version):raise SystemExit('strict three integer version required')
if a.platform=='windows' and not (root/'SeriousShotUpdater.exe').is_file():raise SystemExit('deploy independent updater before inventory')
if a.platform=='macos':
    info=plistlib.loads((root/'Contents/Info.plist').read_bytes())
    if info.get('CFBundleShortVersionString')!=a.version:raise SystemExit('bundle version differs from package')
files=[];seen=set();symlinks=[]
for path in sorted(root.rglob('*')):
    if path.is_symlink():
        if a.platform!='macos' or not path.resolve(strict=True).is_relative_to(root):raise SystemExit(f'unsafe managed symlink: {path}')
        symlinks.append({'path':path.relative_to(root).as_posix(),'target':str(path.readlink())});continue
    if not path.is_file():continue
    relative=path.relative_to(root).as_posix()
    if relative=='package_manifest.json':continue
    if relative in ['settings.ini','.DS_Store'] or relative.startswith(('.seriousshot-','logs/')):raise SystemExit(f'private/runtime state in package: {relative}')
    if relative.casefold() in seen:raise SystemExit('case-colliding managed files')
    seen.add(relative.casefold());h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    files.append({'path':relative,'sha256':h.hexdigest(),'size':path.stat().st_size})
if a.entrypoint not in {f['path'] for f in files}:raise SystemExit('entrypoint missing')
m={'product':'SeriousShot','version':a.version,'platform':a.platform,'architecture':a.architecture,'installer_protocol':1,'entrypoint':a.entrypoint,'unpacked_size':sum(f['size'] for f in files),'files':files}
if symlinks:m['symlinks']=symlinks
if a.platform=='macos':m['bundle_id']=info['CFBundleIdentifier'];m['app_name']=root.name
output=a.output or (root.parent/(root.stem+'.package_manifest.json') if a.platform=='macos' else root/'package_manifest.json')
if a.platform=='macos' and output.resolve().is_relative_to(root):raise SystemExit('Mac manifest must stay outside the signed App (DMG staging root)')
output.write_text(json.dumps(m,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print(f'managed {len(files)} files, {m["unpacked_size"]} bytes: {output.resolve()}')
