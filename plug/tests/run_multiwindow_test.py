"""Test two independent DLL hosts, scoped mutex release, config isolation and misrouting rejection."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dll', type=Path, required=True)
args = parser.parse_args()
out = root / '.build/tests/multiwindow'
out.mkdir(parents=True, exist_ok=True)
shutil.copy2(args.dll.resolve(strict=True), out / 'Monitor.dll')
(out / 'config.ini').write_text('[game]\nmoduleName=MultiWindowTest.exe\n[pipe]\nname=Poe2MultiTest\n[nav]\nstepDistance=126\n', encoding='ascii')
env = dict(os.environ)
env['PATH'] = ';'.join(part.strip('"') for part in env.get('PATH', '').split(';'))
for key in ('POE2_PIPE_NAME', 'POE2_GAME_MODULE', 'POE2_GAME_WINDOW'):
    env.pop(key, None)
locator = Path(env.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(locator), '-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvars64.bat'
sources = ['tests/multiwindow_tests.cpp', 'Common/InstanceMutex.cpp', 'Common/AppConfig.cpp',
           'Common/Logger.cpp', 'Plug/NamedPipeClie.cpp']
batch = out / 'build.cmd'
exe = out / 'MultiWindowTest.exe'
batch.write_text('@echo off\ncall "' + str(vcvars) + '" >nul 2>&1\nif errorlevel 1 exit /b %errorlevel%\n' +
    'cl /nologo /EHsc /MT /O2 /utf-8 /std:c++17 /DNOMINMAX ' + ' '.join('"' + str(root / f) + '"' for f in sources) +
    ' /Fo:"' + out.as_posix() + '/" /Fe:"' + str(exe) + '" /link user32.lib\n', encoding='utf-8')
with (out / 'build.log').open('wb') as log:
    code = subprocess.call(['cmd.exe', '/d', '/c', str(batch)], cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
if code:
    print((out / 'build.log').read_text(encoding='utf-8', errors='replace'))
    sys.exit(code)
with (out / 'run.log').open('wb') as log:
    try:
        code = subprocess.call([str(exe)], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
    except subprocess.TimeoutExpired:
        print('MULTIWINDOW TIMEOUT; inspect', out / 'run.log')
        sys.exit(124)
print((out / 'run.log').read_text(encoding='utf-8', errors='replace'))
print('LOG', out / 'run.log')
sys.exit(code)
