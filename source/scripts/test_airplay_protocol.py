#!/usr/bin/env python3
"""Build host libplist + real patched AirPlay core, run HTTP/FCUP tests on loopback.
Needs C compiler, make and OpenSSL headers/library; no PS4 APIs are invoked.
"""
from pathlib import Path
import os,subprocess
r=Path(__file__).resolve().parents[1]
os.chdir(r)
base=Path(os.environ.get('PS4_AIRPLAY_PORT',r.parent/'work/airplay-port')).absolute()
b=r/'build/release';b.mkdir(parents=True,exist_ok=True)
plist=b/'plist';plist.mkdir(exist_ok=True)
ssl=Path(os.environ.get('HOST_OPENSSL','/opt/homebrew/opt/openssl@3'))
cc=os.environ.get('CC','cc')
def run(args,**kwargs):subprocess.run(list(map(str,args)),check=True,**kwargs)
if not (plist/'src/.libs/libplist-2.0.a').is_file():
 run([base.parent/'research/libplist/configure','--without-cython','--disable-shared'],cwd=plist)
 run(['make','-j4'],cwd=plist)
vendor=base/'vendor/uxplay-lib';objects=[]
sources=list(vendor.glob('*.c'))
for d in ['playfair','llhttp','mdnsd']:sources+=list((vendor/d).glob('*.c'))
flags=['-std=c11','-D_DEFAULT_SOURCE','-D_DARWIN_C_SOURCE','-DNOHOLD','-DPLIST_210','-DPLIST_230','-DOPENSSL_API_COMPAT=0x10101000L','-Wno-deprecated-declarations','-O1','-g','-fsanitize=address,undefined']
includes=['-I'+str(p) for p in [vendor,vendor/'playfair',vendor/'llhttp',base.parent/'research/libplist/include',ssl/'include']]
for src in sources:
 obj=b/(str(src.relative_to(vendor)).replace('/','_')+'.o');objects.append(obj)
 run([cc,*flags,*includes,'-c',src,'-o',obj])
run([cc,*flags,*includes,'tests/test_airplay_server.c',*objects,plist/'src/.libs/libplist-2.0.a','-L'+str(ssl/'lib'),'-lcrypto','-lpthread','-lm','-o',b/'test-airplay-server'])
run(['python3','tests/test_airplay_protocol.py'])
