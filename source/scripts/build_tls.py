#!/usr/bin/env python3
"""Build pinned OpenSSL libssl with the existing Orbis crypto configuration."""
from pathlib import Path
import os,subprocess,shutil
r=Path(__file__).resolve().parents[1]
port=Path(os.environ.get('PS4_AIRPLAY_PORT',r.parent/'work/airplay-port')).absolute()
out=port/'build/crypto/libssl.a'
if out.is_file():print('Using',out);raise SystemExit(0)
if not os.environ.get('OO_PS4_TOOLCHAIN'):raise SystemExit('Set OO_PS4_TOOLCHAIN')
source=port.parent/'research/openssl-3.5.4';build=port/'build/tls-openssl';build.mkdir(parents=True,exist_ok=True)
ar=os.environ.get('PS4_AR') or shutil.which('llvm-ar') or '/opt/homebrew/opt/llvm/bin/llvm-ar'
ranlib=os.environ.get('PS4_RANLIB') or str(Path(ar).with_name('llvm-ranlib'))
env={**os.environ,'CC':str(port/'ps4-cc'),'AR':ar,'RANLIB':ranlib}
flags=['BSD-x86_64','no-shared','no-asm','no-dso','no-tests','no-apps','no-module','no-sock','no-ui-console','no-devcryptoeng','no-engine','no-async','--with-rand-seed=none','--prefix='+str(build/'prefix')]
subprocess.run(['perl',str(source/'Configure'),*flags],cwd=build,env=env,check=True)
subprocess.run(['make','-j4','build_libs'],cwd=build,env=env,check=True)
out.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(build/'libssl.a',out)
print('Built',out,'using custom BIO; existing libcrypto remains unchanged')
