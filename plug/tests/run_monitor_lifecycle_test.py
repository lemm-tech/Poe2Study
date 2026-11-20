"""Load the built DLL only into a throwaway test process, never into the game."""
from pathlib import Path
import argparse,os,subprocess,shutil,sys
root=Path(__file__).resolve().parents[1];out=root/'.build'/'tests'/'lifecycle';out.mkdir(parents=True,exist_ok=True)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dll',type=Path,required=True,help='Exact built DLL to test')
args=parser.parse_args()
source=args.dll.resolve(strict=True)
shutil.copy2(source,out/'Monitor.dll')
print('TEST DLL',source,flush=True)
env=dict(os.environ)
env['PATH']=';'.join(part.strip('"') for part in env.get('PATH','').split(';'))
(out/'config.ini').write_text('[pipe]\nname=Poe2LifecyclePending\n',encoding='ascii')
vc=r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
cmd='"%s" >nul 2>&1 && cl /nologo /EHsc /MT /O2 /utf-8 /std:c++17 /DNOMINMAX tests/monitor_lifecycle_test.cpp Plug/NamedPipeClie.cpp Common/Logger.cpp /Fo:.build\\tests\\lifecycle\\ /Fe:.build\\tests\\lifecycle\\lifecycle.exe /link user32.lib'%vc
with (out/'build.log').open('wb') as a:rc=subprocess.call(cmd,shell=True,cwd=str(root),env=env,stdout=a,stderr=subprocess.STDOUT)
print((out/'build.log').read_text(encoding='utf-8',errors='replace'))
if rc:sys.exit(rc)
with (out/'stdout.log').open('wb') as a,(out/'stderr.log').open('wb') as b:
    try:rc=subprocess.call([str(out/'lifecycle.exe')],cwd=str(out),env=env,stdout=a,stderr=b,timeout=60)
    except subprocess.TimeoutExpired:print('LIFECYCLE TIMEOUT');sys.exit(124)
print((out/'stdout.log').read_text(encoding='utf-8',errors='replace'))
print((out/'stderr.log').read_text(encoding='utf-8',errors='replace'))
sys.exit(rc)
