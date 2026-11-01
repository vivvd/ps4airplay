#!/usr/bin/env python3
"""Rebuild only the patched UxPlay library against the existing dependencies."""
from pathlib import Path
import os, subprocess

ROOT=Path(__file__).resolve().parents[1]
BASE=Path(os.environ.get('PS4_AIRPLAY_PORT',ROOT.parent/'work/airplay-port')).absolute()
RESEARCH=BASE.parent/'research'
wrapper=BASE/'ps4-cc'
lib=BASE/'vendor/uxplay-lib'
core=BASE/'build/core'; core.mkdir(parents=True,exist_ok=True)
ar=os.environ.get('PS4_AR','/opt/homebrew/opt/llvm/bin/llvm-ar')
flags=['-O2','-g','-fstack-usage','-fPIC','-DLAB_PS4','-DPLIST_210','-DPLIST_230','-DNOHOLD','-D_GNU_SOURCE',
       '-DOPENSSL_API_COMPAT=0x10101000L','-Wno-deprecated-declarations']
for include in [lib,lib/'playfair',lib/'llhttp',BASE/'build/crypto/include',
                RESEARCH/'openssl-3.5.4/include',RESEARCH/'libplist/include']:
    flags+=['-I',str(include)]
sources=sorted(lib.glob('*.c'))
for sub in ['playfair','llhttp','mdnsd']:sources+=sorted((lib/sub).glob('*.c'))
objects=[]
for src in sources:
    obj=core/(str(src.relative_to(lib)).replace('/','_')+'.o')
    subprocess.run([str(wrapper),*flags,'-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
subprocess.run([ar,'rcs',str(core/'libairplay.a'),*map(str,objects)],check=True)
print('Rebuilt',len(objects),'UxPlay objects')
