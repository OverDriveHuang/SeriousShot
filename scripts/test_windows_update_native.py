#!/usr/bin/env python3
"""Opt-in Windows helper transactions against disposable deployed fixtures.

Requires a testing-trust build with native install explicitly enabled. Arguments
are paths, never key bytes. The controller never touches normal app/settings.
"""
import argparse, base64, ctypes, hashlib, json, os, pathlib, shutil, subprocess, time, zipfile
p=argparse.ArgumentParser()
p.add_argument('--deployment',type=pathlib.Path,required=True)
p.add_argument('--fixture',type=pathlib.Path,required=True)
p.add_argument('--helper',type=pathlib.Path,help='newly rebuilt helper for a diagnostic fixture')
p.add_argument('--only-normal',action='store_true',help='explicit diagnostic subset, not the native acceptance matrix')
p.add_argument('--only-forward',type=int,help='explicit single forward crash diagnostic subset')
p.add_argument('--only-startup',action='store_true',help='explicit startup recovery diagnostic subset')
p.add_argument('--only-checkpoints',action='store_true',help='checkpoint count and actual mutation interruption subset')
p.add_argument('--only-long',action='store_true',help='single full-deployment long-path transaction')
p.add_argument('--full-deployment',action='store_true',help='retain all deployed assets/licenses in managed fixtures')
p.add_argument('--locked-library',type=pathlib.Path,required=True)
p.add_argument('--catalog-tool',type=pathlib.Path,required=True)
p.add_argument('--secret-key',type=pathlib.Path,required=True)
p.add_argument('--key-id',required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--workspace',type=pathlib.Path,help='new disposable short-path fixture directory')
a=p.parse_args()
a.output=a.output.resolve(); a.output.mkdir(parents=True,exist_ok=False)
a.workspace=(a.workspace or a.output/'transactions').resolve();a.workspace.mkdir(parents=True,exist_ok=False)
def write(path,value):
    path.parent.mkdir(parents=True,exist_ok=True); path.write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf8')
def read(path):
    # Observers must share DELETE as well as READ/WRITE: a Python stdio handle
    # can otherwise prevent the production QSaveFile atomic rename it observes.
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateFileW.restype=ctypes.c_void_p
    kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
    handle=kernel.CreateFileW(str(path),0x80000000,7,None,3,0,None)
    if handle==ctypes.c_void_p(-1).value:
        code=ctypes.get_last_error()
        if code in (2,3): raise FileNotFoundError(code,str(path))
        if code in (5,32): raise PermissionError(code,str(path))
        raise OSError(code,str(path))
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    try:
        size=ctypes.c_longlong()
        kernel.GetFileSizeEx.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_longlong)]
        if not kernel.GetFileSizeEx(handle,ctypes.byref(size)): raise ctypes.WinError(ctypes.get_last_error())
        assert 0 <= size.value <= 1048576, 'bounded observer JSON'
        buffer=ctypes.create_string_buffer(size.value)
        count=ctypes.c_uint32()
        kernel.ReadFile.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32),ctypes.c_void_p]
        if not kernel.ReadFile(handle,buffer,size.value,ctypes.byref(count),None): raise ctypes.WinError(ctypes.get_last_error())
        assert count.value==size.value, 'short JSON read'
        return json.loads(buffer.raw.decode('utf8'))
    finally: kernel.CloseHandle(handle)
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def await_condition(fn,seconds=30):
    limit=time.monotonic()+seconds
    while time.monotonic()<limit:
        try:
            if fn(): return
        except (FileNotFoundError,PermissionError):
            # Native atomic replacement/durable flush can briefly deny an
            # observer handle. Retain the same deadline; corrupt JSON fails.
            pass
        time.sleep(.05)
    raise AssertionError('bounded wait expired')
def wait_exit(pid,creation,timeout=45000):
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.restype=ctypes.c_void_p
    h=kernel.OpenProcess(0x101000,False,int(pid))
    if h:
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        try:
            assert creation, 'waiting requires a captured creation identity'
            times=[(ctypes.c_uint32*2)() for _ in range(4)]
            kernel.GetProcessTimes.argtypes=[ctypes.c_void_p]+[ctypes.c_void_p]*4
            assert kernel.GetProcessTimes(h,*[ctypes.byref(t) for t in times])
            actual=str((times[0][1]<<32)|times[0][0])
            # A PID from a durable old slot can now belong to another process.
            # Keep this same handle for identity and wait, avoiding another race.
            if actual!=str(creation): return None
            kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32]
            result=kernel.WaitForSingleObject(h,timeout)
            assert result==0, f'owned process wait failed: pid={pid} creation={creation} wait={result}'
            code=ctypes.c_uint32()
            kernel.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint32)]
            assert kernel.GetExitCodeProcess(h,ctypes.byref(code))
            return code.value
        finally: kernel.CloseHandle(h)
env=os.environ.copy()
# Remove SDK/Qt PATH dependencies: deployed runtimes must stand alone.
env['PATH']=os.path.join(os.environ['SystemRoot'],'System32')
env.pop('QT_PLUGIN_PATH',None); env.pop('QT_QPA_PLATFORM_PLUGIN_PATH',None)
old=a.workspace/'seed-old'; new=a.workspace/'seed-new'
old.mkdir()
if a.full_deployment: shutil.copytree(a.deployment,old,dirs_exist_ok=True)
# Minimal complete QCore fixture runtime, with real Qt/CRT deployment from
# windeployqt. Final application upgrade is a separate A/B acceptance gate.
for f in a.deployment.iterdir():
    if f.is_file() and (f.suffix.lower()=='.dll' or f.name=='SeriousShotUpdater.exe'):
        shutil.copyfile(f,old/f.name)
shutil.copyfile(a.fixture,old/'SeriousShot.exe')
if a.helper: shutil.copyfile(a.helper,old/'SeriousShotUpdater.exe')
shutil.copyfile(a.locked_library,old/'LockedDependency.dll')
for path in [old/'package_manifest.json',old/'SHA256.txt',old/'DEVELOPMENT_BUILD.txt']:
    if path.exists(): path.unlink()
def manifest(root,version):
    files=[dict(path=f.relative_to(root).as_posix(),sha256=digest(f),size=f.stat().st_size) for f in sorted(root.rglob('*')) if f.is_file() and f.name!='package_manifest.json']
    m=dict(product='SeriousShot',version=version,platform='windows',architecture='x64',installer_protocol=1,entrypoint='SeriousShot.exe',unpacked_size=sum(f['size'] for f in files),files=files)
    write(root/'package_manifest.json',m);return m
(old/'old-only.txt').write_text('old',encoding='utf8')
(old/'changed.txt').write_text('old',encoding='utf8')
oldm=manifest(old,'0.1.4')
shutil.copytree(old,new);(new/'old-only.txt').unlink();(new/'changed.txt').write_text('new',encoding='utf8');(new/'new-only.txt').write_text('new',encoding='utf8')
newm=manifest(new,'0.1.5')
write(a.output/'old-manifest.json',oldm);write(a.output/'new-manifest.json',newm)
archive=a.output/'fixture.zip'
with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
    for f in sorted(new.rglob('*')):
        if f.is_file(): z.write(f,f.relative_to(new).as_posix())
target=dict(version='0.1.5',minimum_os='10.0.0',platform='windows',architecture='x64',asset_url='https://github.com/OverDriveHuang/SeriousShot/releases/download/v0.1.5/fixture.zip',asset_name='fixture.zip',sha256=digest(archive),asset_size=archive.stat().st_size,release_id=1,asset_id=1,release_tag='v0.1.5',draft=False,prerelease=False,installer_protocol=1)
payload=a.output/'payload.json'; envelope=a.output/'envelope.json'
write(payload,dict(product='SeriousShot',channel='testing',revision=1,published_at='2026-10-13T00:00:00Z',targets=[target]))
subprocess.run([str(a.catalog_tool.resolve()),'sign',str(payload),str(a.secret_key.resolve()),a.key_id,str(envelope)],check=True)
results=[]; owned_processes=[]; owned_handles=[]; active_case='setup'
def verify(root,m):
    for f in m['files']: assert (root/f['path']).is_file() and digest(root/f['path'])==f['sha256'], f['path']
    assert read(root/'package_manifest.json')==m
def run_case(name,fail=None,reject=False,hold=False,lock=False,no_receipt=False,loaded=False,recover_fail=None,terminal_fail=None,crash_terminal=False,startup=False,startup_refuse=False,mutation_fail=None,recover_mutation_fail=None,legacy_journal=False,checkpoint_fail=None):
    global active_case
    active_case=name
    root=a.workspace/name; install=root/'中文 安装'
    if a.only_long:
        assert a.full_deployment
        install=root/('中文 安装'+'x'*(189-len(str(install))))
    shutil.copytree(old,install)
    (install/'user-file.txt').write_text('untouched',encoding='utf8')
    slot=root/'slot'; cwd=root/'different cwd';cwd.mkdir()
    childenv=env.copy()
    childenv['SERIOUSSHOT_UPDATE_TRACE']='1'
    if fail: childenv['SERIOUSSHOT_UPDATE_FAIL_AFTER']=str(fail)
    if mutation_fail: childenv['SERIOUSSHOT_UPDATE_FAIL_MUTATION']=str(mutation_fail)
    if no_receipt: childenv['SERIOUSSHOT_TEST_NO_RECEIPT']='1'
    if terminal_fail: childenv['SERIOUSSHOT_UPDATE_FAIL_TERMINAL']=terminal_fail
    if checkpoint_fail: childenv['SERIOUSSHOT_UPDATE_FAIL_CHECKPOINT']=checkpoint_fail
    if crash_terminal: childenv['SERIOUSSHOT_UPDATE_CRASH_AFTER_TERMINAL']='1'
    holder=None;handle=None
    if hold:
        holder=subprocess.Popen([str(install/'SeriousShot.exe'),'--hold',str(root/'release')],cwd=cwd,env=env)
        owned_processes.append(holder)
        await_condition(lambda:(root/'release.ready').exists())
    if loaded:
        holder=subprocess.Popen([str(old/'SeriousShot.exe'),'--hold-library',str(install/'LockedDependency.dll'),str(root/'release')],cwd=cwd,env=env)
        owned_processes.append(holder)
        await_condition(lambda:(root/'release.ready').exists())
        ready=read(root/'release.ready')
        assert ready['mapped_library'].casefold()==str(install/'LockedDependency.dll').replace('\\','/').casefold()
    if lock:
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.restype=ctypes.c_void_p
        kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
        locked_file='Qt6Core.dll' if lock=='dll' else 'changed.txt'
        handle=kernel.CreateFileW(str(install/locked_file),0x80000000,1,None,3,0,None)
        assert handle not in (None,ctypes.c_void_p(-1).value)
        owned_handles.append(handle)
    launch=str(install/'SeriousShot.exe')
    if a.only_long: launch=launch.lower()
    parent=subprocess.Popen([launch,'--install',str(envelope),str(archive),str(slot),'reject' if reject else 'accept'],cwd=cwd,env=childenv)
    owned_processes.append(parent)
    assert parent.wait(timeout=45)==0
    await_condition(lambda:len(list((slot/'helpers').glob('*/transaction.json')))==1)
    journal=next((slot/'helpers').glob('*/transaction.json'))
    if hold:
        time.sleep(.5);verify(install,oldm)
        (root/'release').touch(); assert holder.wait(timeout=10)==0
    ready=read(journal.parent/'ready.json')
    # Process completion is the synchronization barrier. Do not continuously
    # poll the production journal during its atomic renames.
    helper_exit=wait_exit(ready['pid'],ready['creation'],95000)
    if lock:
        kernel.CloseHandle.argtypes=[ctypes.c_void_p];kernel.CloseHandle(handle);owned_handles.remove(handle);handle=None
    if startup:
        committed=crash_terminal and read(journal)['phase']=='complete'
        if (journal.parent/'started.json').exists():
            receipt=read(journal.parent/'started.json'); (install/'stop').touch(); wait_exit(receipt['pid'],receipt['creation'])
        expected=newm if committed else oldm
        recoveryenv=env.copy();recoveryenv['SERIOUSSHOT_STARTUP_TEST_SLOT']=str(slot)
        for refused in ([True,False] if startup_refuse else [False]):
            caller_env=recoveryenv.copy()
            if refused: caller_env['SERIOUSSHOT_UPDATE_STARTUP_REFUSE']='1'
            for marker in ('startup-caller.json','release-startup'):
                if (slot/marker).exists(): (slot/marker).unlink()
            caller=subprocess.Popen([str(install/'SeriousShot.exe'),'--startup',str(slot)],cwd=cwd,env=caller_env)
            owned_processes.append(caller)
            await_condition(lambda:(slot/'startup-caller.json').exists(),15)
            marker=read(slot/'startup-caller.json');assert marker['armed']==(not refused)
            assert int(marker['pid'])==caller.pid and caller.poll() is None
            recovery_ready=read(journal.parent/'recovery-ready.json')
            # The helper is armed but cannot move any file before normal caller exit.
            time.sleep(.3);verify(install,newm if committed or fail else oldm)
            assert not (slot/'restarted.json').exists()
            (slot/'release-startup').touch();assert caller.wait(timeout=10)==0
            assert wait_exit(recovery_ready['pid'],recovery_ready['creation'],65000) in (None,0)
            if refused:
                verify(install,newm);assert read(slot/'slot.json')['phase']=='installing'
                assert not (slot/'restarted.json').exists()
            else:
                await_condition(lambda:(slot/'restarted.json').exists(),15)
                restarted=read(slot/'restarted.json')
                assert restarted['exe'].casefold()==str(install/'SeriousShot.exe').replace('\\','/').casefold()
                assert int(restarted['pid'])!=caller.pid and restarted['creation']
                verify(install,expected)
                final=read(slot/'slot.json');assert final['phase']==('empty' if committed else 'ready')
                assert not final.get('transaction') and not final.get('nonce')
                (slot/'stop-restarted').touch();assert wait_exit(restarted['pid'],restarted['creation']) in (None,0)
        assert read(journal)['phase']==('complete' if committed else 'rolled_back')
    elif reject:
        assert read(journal).get('phase')=='cancelled'
        verify(install,oldm)
    elif terminal_fail or crash_terminal:
        assert helper_exit in (None,20 if terminal_fail else 92)
        pending=read(slot/'slot.json')
        assert pending['phase']=='installing' and (slot/'payload.zip').exists()
        subprocess.run([str(install/'SeriousShot.exe'),'--cleanup',str(slot)],env=env,cwd=cwd,check=True,timeout=15)
        assert journal.exists() and journal.parent.exists(), 'cleanup destroyed pending recovery evidence'
        if (journal.parent/'started.json').exists():
            receipt=read(journal.parent/'started.json'); (install/'stop').touch(); wait_exit(receipt['pid'],receipt['creation'])
        committed=crash_terminal and read(journal)['phase']=='complete'
        expected=newm if committed else oldm
        for repeat in range(2):
            subprocess.run([str(journal.parent/'SeriousShotUpdater.exe'),'--recover',str(journal)],cwd=cwd,env=env,check=True,timeout=60)
            time.sleep(.1);verify(install,expected)
        final=read(slot/'slot.json')
        assert final['phase']==('empty' if committed else 'ready')
        assert not final.get('transaction') and not final.get('nonce')
        assert (not (slot/'payload.zip').exists()) if committed else (slot/'payload.zip').exists()
    elif fail or mutation_fail:
        assert helper_exit in (None,91), 'mutation/operation failpoint was not reached'
        assert read(journal).get('intent') is not None
        if legacy_journal:
            # Protocol-1 helpers formerly persisted an operation intent before
            # each move. Recovery must ignore even a stale operation label.
            write(journal,dict(read(journal),intent='publish_file',file='changed.txt'))
        # Wait for actual injected helper exit, not simply its last intent.
        st=read(slot/'slot.json'); pid=int(st['helper_pid'])
        recoveryenv=env.copy()
        recoveryenv['SERIOUSSHOT_UPDATE_TRACE']='1'
        if recover_fail or recover_mutation_fail:
            interrupted=recoveryenv.copy()
            if recover_fail: interrupted['SERIOUSSHOT_UPDATE_FAIL_AFTER']=str(recover_fail)
            if recover_mutation_fail: interrupted['SERIOUSSHOT_UPDATE_FAIL_MUTATION']=str(recover_mutation_fail)
            interrupted_result=subprocess.run([str(journal.parent/'SeriousShotUpdater.exe'),'--recover',str(journal)],cwd=cwd,env=interrupted,timeout=60)
            assert interrupted_result.returncode==91, 'recovery failpoint was not reached'
        for repeat in range(2):
            subprocess.run([str(journal.parent/'SeriousShotUpdater.exe'),'--recover',str(journal)],cwd=cwd,env=recoveryenv,check=True,timeout=60)
            # Recovery starts the restored fixture with no args, which exits.
            time.sleep(.1)
            verify(install,oldm)
        assert read(journal)['phase']=='rolled_back'
    else:
        assert read(journal).get('phase') in ('complete','rolled_back')
        if loaded:
            (root/'release').touch(); assert holder.wait(timeout=10)==0
        # Windows can allow renaming a mapped image. This real LoadLibrary
        # case records the native outcome; deny-delete handles above exercise
        # the required locked-file rollback deterministically.
        rolled=bool(lock or no_receipt or checkpoint_fail or (loaded and read(journal)['phase']=='rolled_back'))
        if lock:
            failure=read(journal)
            assert failure.get('failure_reason')=='replace_move' and failure.get('failure_file')==locked_file, 'unrelated failure masked as deny-delete lock'
            assert failure.get('failure_context',{}).get('move_attempt')=='3', 'locked move did not exhaust its bounded retry budget'
        if no_receipt:
            assert read(journal).get('failure_reason')=='startup_receipt_timeout'
        if checkpoint_fail:
            failure=read(journal)
            assert failure.get('failure_reason')=='injected_checkpoint'
            assert failure.get('failure_intent')==('withdraw_exe' if checkpoint_fail=='replacing' else 'launch')
            assert not (journal.parent/'started.json').exists(), 'candidate launched after failed checkpoint'
        if loaded and rolled:
            failure=read(journal)
            assert failure.get('failure_reason')=='replace_move' and failure.get('failure_file')=='LockedDependency.dll', 'unrelated failure masked as mapped DLL behavior'
        expected=oldm if rolled else newm
        verify(install,expected)
        assert read(journal)['phase']==('rolled_back' if rolled else 'complete')
        (install/'stop').touch()
        if read(journal)['phase']=='complete':
            receipt=read(journal.parent/'started.json')
            assert receipt['exe'].casefold()==str(install/'SeriousShot.exe').replace('\\','/').casefold()
            assert receipt['version']=='0.1.5'
            wait_exit(receipt['pid'],receipt['creation'])
    assert (install/'user-file.txt').read_text(encoding='utf8')=='untouched'
    runs=[part.strip().splitlines() for part in (journal.parent/'test-boundaries.txt').read_text().split('run\n')[1:]]
    checkpoints=[event.split()[1] for event in runs[0] if event.startswith('checkpoint ')]
    if not (fail or mutation_fail or reject or terminal_fail or crash_terminal or startup):
        expected_checkpoints=[] if checkpoint_fail=='replacing' else ['replacing']
        if no_receipt or not rolled: expected_checkpoints.append('awaiting_receipt')
        expected_checkpoints.append('rolled_back' if rolled else 'complete')
        assert checkpoints==expected_checkpoints, checkpoints
        if checkpoint_fail=='replacing':
            assert not any(event.startswith('mutation ') for event in runs[0]), 'application files moved before durable descriptor'
    results.append(dict(case=name,passed=True,phase=read(journal)['phase'],intent=read(journal).get('intent'),helper_exit_observed=helper_exit,helper_journal_checkpoints=checkpoints,mutation_runs=runs,managed_files=len(newm['files'])))
    write(a.output/name/'final-state.json',dict(journal=read(journal),slot=read(slot/'slot.json')))
    state=read(slot/'slot.json')
    if state.get('helper_pid'): wait_exit(state['helper_pid'],state['helper_creation'])
    subprocess.run([str(install/'SeriousShot.exe'),'--cleanup',str(slot)],env=env,cwd=cwd,check=True,timeout=15)
    assert not journal.parent.exists()
    assert not list(install.glob('.seriousshot-*'))
    assert not list((slot/'cleanup').glob('*.json'))
    results[-1]['managed_runtime_cleaned']=True
    write(a.output/'results.json',results)
    print(name,'PASS',flush=True)
    return results[-1]
def mutation_cases(boundaries,normal):
    # Select actual native operations from a successful run, independently of
    # the legacy diagnostic-operation failpoint numbering.
    def representatives(events):
        selected={}
        for event in events:
            fields=event.split()
            if fields[0]=='mutation':
                key=(fields[2],fields[3])
                # Prefer the dependency whose old/new hashes differ, so a
                # repeated rollback proves restored old bytes are retained.
                if key not in selected or fields[4:]==['changed.txt']: selected[key]=int(fields[1])
        return selected
    for (operation,action),number in representatives(normal['mutation_runs'][0]).items():
        run_case(f'mutation-{action}-{operation}',mutation_fail=number)
    rollback=run_case('mutation-rollback-baseline',fail=boundaries,legacy_journal=True)
    for (operation,action),number in representatives(rollback['mutation_runs'][1]).items():
        run_case(f'mutation-{action}-{operation}',fail=boundaries,recover_mutation_fail=number)
def file_count_case(normal):
    global oldm,newm,archive,envelope
    # Add real managed files, rebuild/sign the fixture, and exercise the same
    # helper. Journal write count must stay constant as payload count grows.
    for n in range(24):
        relative=pathlib.Path('checkpoint-count')/f'file-{n:02}.txt'
        for seed in (old,new):
            (seed/relative).parent.mkdir(exist_ok=True)
            (seed/relative).write_text(f'fixture {n}',encoding='utf8')
    oldm=manifest(old,'0.1.4');newm=manifest(new,'0.1.5')
    archive=a.output/'fixture-more-files.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for f in sorted(new.rglob('*')):
            if f.is_file(): z.write(f,f.relative_to(new).as_posix())
    larger_target=dict(target,sha256=digest(archive),asset_size=archive.stat().st_size)
    larger_payload=a.output/'payload-more-files.json';envelope=a.output/'envelope-more-files.json'
    write(larger_payload,dict(product='SeriousShot',channel='testing',revision=1,published_at='2026-10-13T00:00:00Z',targets=[larger_target]))
    subprocess.run([str(a.catalog_tool.resolve()),'sign',str(larger_payload),str(a.secret_key.resolve()),a.key_id,str(envelope)],check=True)
    larger=run_case('normal-more-files')
    assert larger['managed_files']==normal['managed_files']+24
    assert larger['helper_journal_checkpoints']==normal['helper_journal_checkpoints']
def checkpoint_failure_cases():
    run_case('checkpoint-replacing-write-fails',checkpoint_fail='replacing')
    run_case('checkpoint-awaiting-receipt-write-fails',checkpoint_fail='awaiting_receipt')
    run_case('terminal-complete-write-fails',terminal_fail='complete')
    run_case('terminal-rollback-write-fails',lock=True,terminal_fail='rolled_back')
    run_case('terminal-complete-before-slot-crash',crash_terminal=True)
    run_case('terminal-rollback-before-slot-crash',lock=True,crash_terminal=True)
try:
    boundaries=1+len(oldm['files'])-1+1+len(newm['files'])-1+1+1+1
    if a.only_long:
        run_case('long-full-deployment')
        print('single full-deployment long-path transaction PASS',flush=True)
        raise SystemExit(0)
    if a.only_startup:
        run_case('startup-rollback',fail=boundaries,startup=True)
        run_case('startup-refused-then-retry',fail=boundaries,startup=True,startup_refuse=True)
        run_case('startup-committed-terminal',crash_terminal=True,startup=True)
        run_case('startup-rolled-terminal',lock=True,crash_terminal=True,startup=True)
        print('explicit diagnostic subset: startup recovery',flush=True)
        raise SystemExit(0)
    if a.only_forward:
        run_case(f'crash-{a.only_forward:03}',fail=a.only_forward)
        print('explicit diagnostic subset: forward crash',a.only_forward,flush=True)
        raise SystemExit(0)
    normal=run_case('normal')
    if a.only_normal:
        print('explicit diagnostic subset: normal only',flush=True)
        raise SystemExit(0)
    if a.only_checkpoints:
        run_case('file-lock',lock=True)
        checkpoint_failure_cases()
        mutation_cases(boundaries,normal)
        file_count_case(normal)
        print('explicit diagnostic subset: phase checkpoints and actual mutations',flush=True)
        raise SystemExit(0)
    run_case('quit-refused',reject=True)
    run_case('other-instance',hold=True)
    run_case('file-lock',lock=True)
    run_case('dll-file-lock',lock='dll')
    run_case('mapped-dll-other-path-process',loaded=True)
    run_case('missing-startup-receipt',no_receipt=True)
    checkpoint_failure_cases()
    run_case('startup-rollback',fail=boundaries,startup=True)
    run_case('startup-refused-then-retry',fail=boundaries,startup=True,startup_refuse=True)
    run_case('startup-committed-terminal',crash_terminal=True,startup=True)
    run_case('startup-rolled-terminal',lock=True,crash_terminal=True,startup=True)
    # Every legacy operation boundary of the forward transaction (including
    # final EXE publication and launch). Recovery must be idempotent.
    boundaries=1+len(oldm['files'])-1+1+len(newm['files'])-1+1+1+1
    for n in range(1,boundaries+1): run_case(f'crash-{n:03}',fail=n)
    # Start rollback with the new EXE already published, then crash every
    # rollback operation boundary, including quarantine and old EXE republish.
    for n in range(1,len(oldm['files'])+3):
        run_case(f'recovery-crash-{n:03}',fail=boundaries,recover_fail=n)
    mutation_cases(boundaries,normal)
    file_count_case(normal)
    print('native cases:',len(results),'forward boundaries:',boundaries)
except Exception as error:
    write(a.output/'failure.json',dict(case=active_case,error=str(error),completed_cases=len(results)))
    raise
finally:
    # Only isolated test-owned processes/handles. Never terminate a daily app.
    for release in a.workspace.glob('*/release.ready'): release.with_suffix('').touch()
    for install in a.workspace.glob('*/中文 安装'): (install/'stop').touch()
    for slot in a.workspace.glob('*/slot'):
        (slot/'release-startup').touch();(slot/'stop-restarted').touch()
    for process in owned_processes:
        if process.poll() is None:
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
    for state_path in a.workspace.glob('*/slot/slot.json'):
        state=read(state_path)
        if state.get('helper_pid'): wait_exit(state['helper_pid'],state['helper_creation'],75000)
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    for handle in owned_handles: kernel.CloseHandle(handle)
