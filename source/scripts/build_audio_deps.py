#!/usr/bin/env python3
"""Build the bundled audio dependencies for Orbis; does not install globally.

Expects the source bundle layout source/ and work/{airplay-port,research}.
Requires LLVM, make, Perl, autoconf, automake, libtool and pkg-config.
"""
from pathlib import Path
import os, subprocess, shutil, shlex

ROOT = Path(__file__).resolve().parents[1]
BASE = Path(os.environ.get('PS4_AIRPLAY_PORT', ROOT.parent / 'work/airplay-port')).absolute()
RESEARCH = BASE.parent / 'research'
SDK = Path(os.environ['OO_PS4_TOOLCHAIN']).absolute()
CC = os.environ.get('PS4_CLANG', '/opt/homebrew/opt/llvm/bin/clang')
LD = os.environ.get('PS4_LLD', '/opt/homebrew/opt/lld/bin/ld.lld')
AR = os.environ.get('PS4_AR', str(Path(CC).parent / 'llvm-ar'))
RANLIB = str(Path(AR).parent / 'llvm-ranlib')
LOGS = BASE / 'logs'; LOGS.mkdir(parents=True, exist_ok=True)
COMPAT = BASE / 'compat/sys'; COMPAT.mkdir(parents=True, exist_ok=True)
(COMPAT / 'endian.h').write_text('#include <endian.h>\n')
(COMPAT / 'sysctl.h').write_text('#pragma once\n#include <sys/types.h>\n')
wrapper = BASE / 'ps4-cc'
command = [CC,'--target=x86_64-pc-freebsd12-elf','-isysroot',str(SDK),'-isystem',str(SDK/'include'),'-I',str(BASE/'compat'),'-fPIC']
wrapper.write_text('#!/bin/sh\nexec '+shlex.join(command)+' "$@"\n'); wrapper.chmod(0o755)
env = dict(os.environ, CC=str(wrapper), CXX=str(wrapper), AR=AR, RANLIB=RANLIB)
prefix = BASE / 'prefix'
ldflags = f'-fuse-ld={LD} -nostdlib -L{SDK}/lib -lc -lkernel {SDK}/lib/crt1.o'

def run(args, cwd, name, extra=None):
    with (LOGS / name).open('w') as f:
        subprocess.run(list(map(str,args)), cwd=cwd, env=dict(env, **(extra or {})),
                       stdout=f, stderr=subprocess.STDOUT, check=True)

crypto = BASE/'build/crypto'; crypto.mkdir(parents=True,exist_ok=True)
run(['perl',RESEARCH/'openssl-3.5.4/Configure','BSD-x86_64',
     'no-shared','no-asm','no-dso','no-tests','no-apps','no-module','no-sock',
     'no-ui-console','no-devcryptoeng','no-engine','no-async',
     '--with-rand-seed=none',f'--prefix={prefix}'],crypto,'crypto-config.log')
run(['make','-j6','build_libs'],crypto,'crypto-build.log')

plist_src = RESEARCH/'libplist'
run(['sh','autogen.sh'],plist_src,'plist-autogen.log',{'NOCONFIGURE':'1'})
plist = BASE/'build/plist'; plist.mkdir(parents=True,exist_ok=True)
run([plist_src/'configure','--host=x86_64-unknown-freebsd','--disable-shared',
     '--enable-static','--without-cython','--without-tests',f'--prefix={prefix}'],
    plist,'plist-config.log',{'CFLAGS':'-O2 -fPIC','CXXFLAGS':f'-O2 -fPIC -isystem {SDK}/include/c++/v1','LDFLAGS':ldflags})
run(['make','-C','libcnary','-j4'],plist,'plist-cnary.log')
run(['make','-C','src','-j4','libplist-2.0.la'],plist,'plist-build.log')

ff = BASE/'build/ffmpeg'; ff.mkdir(parents=True,exist_ok=True)
run([RESEARCH/'ffmpeg-7.1.2/configure','--enable-cross-compile','--target-os=freebsd',
     '--arch=x86_64',f'--cc={wrapper}',f'--ar={AR}',f'--ranlib={RANLIB}',
     '--disable-everything','--disable-programs','--disable-doc','--disable-network',
     '--disable-autodetect','--disable-x86asm','--disable-asm','--disable-shared',
     '--enable-static','--enable-small','--enable-avcodec','--enable-avutil',
     '--enable-swresample','--disable-avdevice','--disable-avformat','--disable-avfilter',
     '--disable-swscale','--enable-decoder=alac,aac,aac_latm,pcm_s16be,pcm_s16le',
     '--enable-parser=aac,aac_latm','--disable-iconv','--disable-pthreads',
     '--disable-w32threads','--disable-os2threads','--disable-debug','--disable-stripping',
     '--enable-pic','--disable-runtime-cpudetect','--disable-outdevs','--disable-indevs',
     '--disable-encoders',f'--extra-ldflags={ldflags}',f'--prefix={prefix}'],ff,'ffmpeg-config.log')
# OpenOrbis exposes a kernel symbol but not the generic FreeBSD sysctl API.
p=ff/'config.h';p.write_text(p.read_text().replace('#define HAVE_SYSCTL 1','#define HAVE_SYSCTL 0'))
run(['make','-j6'],ff,'ffmpeg-build.log')

lib = BASE/'vendor/uxplay-lib'; core=BASE/'build/core'; core.mkdir(parents=True,exist_ok=True)
flags=['-O2','-fPIC','-DLAB_PS4','-DPLIST_210','-DPLIST_230','-DNOHOLD','-D_GNU_SOURCE',
       '-DOPENSSL_API_COMPAT=0x10101000L','-Wno-deprecated-declarations']
for include in [lib,lib/'playfair',lib/'llhttp',crypto/'include',
                RESEARCH/'openssl-3.5.4/include',plist_src/'include']:
    flags += ['-I',str(include)]
sources=list(lib.glob('*.c'))
for sub in ['playfair','llhttp','mdnsd']:sources+=list((lib/sub).glob('*.c'))
objects=[]
for src in sources:
    obj=core/(str(src.relative_to(lib)).replace('/','_')+'.o')
    run([wrapper,*flags,'-c',src,'-o',obj],BASE,'core-'+src.name+'.log')
    objects.append(obj)
run([AR,'rcs',core/'libairplay.a',*objects],BASE,'core-archive.log')
print('Built audio dependencies in',BASE)
