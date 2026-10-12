#!/usr/bin/env python3
"""Narrow isolated ad-hoc bundle switch POC; never enables automatic install."""
import argparse, hashlib, json, pathlib, plistlib, subprocess, os, shutil, datetime
p=argparse.ArgumentParser();p.add_argument('a',type=pathlib.Path);p.add_argument('b',type=pathlib.Path);p.add_argument('c',type=pathlib.Path);p.add_argument('--work',type=pathlib.Path,required=True);p.add_argument('--evidence',type=pathlib.Path,required=True);a=p.parse_args()
work=a.work.resolve();work.mkdir(parents=True,exist_ok=False);target=work/'SeriousShot.app'
def run(args):
 r=subprocess.run([str(x) for x in args],capture_output=True,text=True)
 return {'code':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
def inspect(bundle):
 info=plistlib.loads((bundle/'Contents/Info.plist').read_bytes());exe=bundle/'Contents/MacOS/SeriousShot'
 return {'version':info['CFBundleShortVersionString'],'bundle_id':info['CFBundleIdentifier'],'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'signature':run(['codesign','-d','--verbose=4',bundle]),'strict_verify':run(['codesign','--verify','--deep','--strict',bundle]),'gatekeeper':run(['spctl','--assess','--type','execute','--verbose=4',bundle]),'quarantine':run(['xattr','-p','com.apple.quarantine',bundle])}
report={'time_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'os':run(['sw_vers']),'scope':'isolated current-signature bundle disk switch; no daily App/settings/hotkey or TCC changes','transitions':[]}
run(['ditto',a.a.resolve(),target]);report['A']=inspect(target)
for label,source in [('B',a.b),('C',a.c)]:
 stage=work/f'{label}-stage.app';backup=work/f'{label}-backup.app';run(['ditto',source.resolve(),stage]);before=inspect(target);prepared=inspect(stage)
 if before['strict_verify']['code'] or prepared['strict_verify']['code']:raise SystemExit('signature validation failed; retained POC files')
 journal=work/f'{label}-transaction.json';journal.write_text(json.dumps({'phase':'withdraw','target':str(target),'stage':str(stage),'backup':str(backup)}))
 os.rename(target,backup)
 # Exercise interrupted publication: roll back while the new bundle is still staged.
 os.rename(backup,target);rollback=inspect(target)
 if rollback['binary_sha256']!=before['binary_sha256']:raise SystemExit('rollback mismatch')
 os.rename(target,backup);os.rename(stage,target);after=inspect(target)
 record={'step':label,'before':before,'prepared':prepared,'rollback':rollback,'after':after,'same_target':str(target),'bundle_switch_completed':True,'launch_attempted':False}
 # Gatekeeper rejection is a negative capability result, not permission to bypass it.
 record['automatic_gate_passed']=False
 report['transitions'].append(record);journal.write_text(json.dumps({'phase':'complete','target':str(target),'backup':str(backup)}))
report['result']='ManualDmg remains default: Gatekeeper/ad-hoc launch and TCC persistence gate not met'
a.evidence.parent.mkdir(parents=True,exist_ok=True);a.evidence.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(a.evidence.resolve())
