#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host callback-model tests for actual WSA source; NEVER a kernel/hardware test.

Requires an explicit source path. Uses only host Clang/pthreads and new retained
output directories. No kernel module, real netlink, sysfs, radio or credentials.
"""
import argparse,hashlib,json,os,pathlib,re,resource,shutil,subprocess,tempfile
B=pathlib.Path(__file__).resolve().parent
CASES=['registration-failure','family-failure-unwind','unbound-interfaces','normal-control','rejected-inputs','failed-switch-recovery','duplicate-device','remove-rebind','select-remove-race','parameter-remove-race','repeated-lifecycle','probe-select-failure','missing-dt']+['probe-fault-'+str(i) for i in range(1,8)]
def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True,type=pathlib.Path);p.add_argument('--out-parent',type=pathlib.Path,default=B/'runs');args=p.parse_args()
source=args.source.resolve();data=source.read_bytes()
if len(data)>128*1024:raise SystemExit('Unexpectedly large driver source')
args.out_parent.mkdir(parents=True,exist_ok=True);out=pathlib.Path(tempfile.mkdtemp(prefix='host-',dir=args.out_parent)).resolve()
cc=shutil.which('clang')
if not cc:raise SystemExit('Host Clang required; nothing will be installed')
version=subprocess.run([cc,'--version'],capture_output=True,text=True,timeout=10,check=True).stdout
(out/'compiler-version.txt').write_text(version)
snapshot=out/'candidate.c';snapshot.write_bytes(data)
for name in ['driver-boundary.c','kernel-shim.h','run.py']:
 (out/name).write_bytes((B/name).read_bytes())
includes=re.findall(r'^#include <([^>]+)>',data.decode(),re.M)
allowed={'linux/capability.h','linux/delay.h','linux/device.h','linux/err.h','linux/kernel.h','linux/module.h','linux/moduleparam.h','linux/mutex.h','linux/of.h','linux/of_device.h','linux/pinctrl/consumer.h','linux/platform_device.h','linux/slab.h','net/genetlink.h'}
if set(includes)!=allowed:raise SystemExit('Unexpected driver include footprint')
for name in includes:
 q=out/'include'/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_text('#include "kernel-shim.h"\n')
cmd=[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+str(out/'include'),'-I'+str(out),'-DWSA_DRIVER_SOURCE="'+str(snapshot)+'"',str(out/'driver-boundary.c'),'-o',str(out/'driver-boundary')]
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
result={'scope':'Host API-boundary model only; no kernel compilation, real capability enforcement or hardware behavior tested','source_sha256':digest(snapshot),'compiler':cc,'compiler_sha256':digest(pathlib.Path(cc).resolve()),'compiler_version':version,'command':cmd,'harness_sha256':digest(out/'driver-boundary.c'),'shim_sha256':digest(out/'kernel-shim.h'),'runner_sha256':digest(out/'run.py'),'cases':[]}
def record(): (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
r=subprocess.run(cmd,capture_output=True,text=True,timeout=90);(out/'compile.stdout').write_text(r.stdout);(out/'compile.stderr').write_text(r.stderr);result['compile_returncode']=r.returncode;record()
if r.returncode:raise SystemExit('Host compile failed; retained '+str(out))
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
# LeakSanitizer is unavailable on the primary macOS host; the harness separately
# checks every modeled devm allocation is released. This is not kernel kmemleak.
for case in CASES:
 r=subprocess.run([str(out/'driver-boundary'),case],capture_output=True,text=True,timeout=20,env=env)
 (out/(case+'.stdout')).write_text(r.stdout);(out/(case+'.stderr')).write_text(r.stderr)
 ok=r.returncode==0 and r.stdout.strip()=='PASS '+case
 result['cases'].append({'name':case,'returncode':r.returncode,'pass':ok});record()
 if not ok:raise SystemExit('Host case failed: '+case+'; retained '+str(out))
result['pass']=True;record();print('PASS: '+str(len(CASES))+' host callback-model cases with ASan/UBSan; NOT kernel/live tests');print('Retained '+str(out))
