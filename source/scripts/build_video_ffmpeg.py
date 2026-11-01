#!/usr/bin/env python3
"""Build FFmpeg audio+H.264+swscale without modifying the 0.10 audio libraries."""
from pathlib import Path
import os, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
BASE=Path(os.environ.get('PS4_AIRPLAY_PORT',ROOT.parent/'work/airplay-port')).absolute()
SRC=BASE.parent/'research/ffmpeg-7.1.2'
host='--host' in sys.argv
fast='--fast' in sys.argv
BUILD=BASE/'build'/('ffmpeg-video'+('-fast' if fast else '')+('-host' if host else ''))
BUILD.mkdir(parents=True,exist_ok=True)
args=[str(SRC/'configure'),'--disable-everything','--disable-programs','--disable-doc',
 '--disable-network','--disable-autodetect','--disable-x86asm','--disable-asm',
 '--disable-shared','--enable-static','--disable-small','--optflags=-O3',
 '--enable-avcodec','--enable-avutil','--enable-swresample','--enable-swscale',
 '--disable-avdevice','--disable-avformat','--disable-avfilter',
 '--enable-decoder=h264,alac,aac,aac_latm,pcm_s16be,pcm_s16le',
 '--enable-parser=h264,aac,aac_latm','--disable-iconv','--disable-pthreads',
 '--disable-w32threads','--disable-os2threads','--disable-debug','--disable-stripping',
 '--enable-pic','--disable-runtime-cpudetect','--disable-encoders']
if not host:
 sdk=Path(os.environ['OO_PS4_TOOLCHAIN']).absolute()
 ar=os.environ.get('PS4_AR','/opt/homebrew/opt/llvm/bin/llvm-ar')
 args+=['--enable-cross-compile','--target-os=freebsd','--arch=x86_64',
  '--cc='+str(BASE/'ps4-cc'),'--ar='+ar,'--ranlib='+str(Path(ar).parent/'llvm-ranlib'),
  '--extra-ldflags=-fuse-ld='+os.environ.get('PS4_LLD','/opt/homebrew/opt/lld/bin/ld.lld')+
  ' -nostdlib -L'+str(sdk/'lib')+' -lc -lkernel '+str(sdk/'lib/crt1.o')]
if fast:
 args=[a for a in args if a not in ['--disable-asm','--disable-x86asm','--disable-runtime-cpudetect']]
 args+=['--enable-asm','--enable-runtime-cpudetect']
 if not host:
  args+=['--enable-x86asm','--x86asmexe='+os.environ.get('PS4_NASM','/opt/homebrew/bin/nasm'),
   '--disable-avx','--disable-avx2','--disable-avx512','--disable-avx512icl',
   '--disable-fma3','--disable-fma4','--disable-xop','--extra-cflags=-mno-avx -mno-avx2']
subprocess.run(args,cwd=BUILD,check=True)
if not host:
 p=BUILD/'config.h';p.write_text(p.read_text().replace('#define HAVE_SYSCTL 1','#define HAVE_SYSCTL 0'))
subprocess.run(['make','-j6'],cwd=BUILD,check=True)
print('Built',BUILD)
