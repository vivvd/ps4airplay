#!/usr/bin/env python3
"""Build a native PS4 fSELF and installable fake PKG using open-source tools."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ps4'
STAGE = BUILD / 'stage'
CONTENT = 'IV0000-ARPL00001_00-RECEIVERLAB00001'
PACKAGE_VERSION = '1.00'

def run(*args, cwd=ROOT):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)

def required(name):
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f'Set {name}; see README.md')
    # Preserve tool symlink names: ld.lld selects its driver using argv[0].
    return Path(value).absolute()

sdk = required('OO_PS4_TOOLCHAIN')
cc = required('PS4_CLANG')
ld = required('PS4_LLD')
fself = required('PS4_FSELF')
pkgtool = required('PS4_PKGTOOL')
for p in (sdk / 'lib/crt1.o', cc, ld, fself, pkgtool):
    if not p.exists(): raise SystemExit(f'Missing dependency: {p}')
(STAGE / 'sce_sys').mkdir(parents=True, exist_ok=True)
# OpenOrbis replacement modules must ship with the application, even with
# static musl. They are open-source stubs, not Sony SDK binaries.
runtime_files = ('sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                 'sce_sys/about/right.sprx')
for target in runtime_files:
    source = sdk / 'src/modules' / Path(target).name
    if not source.is_file():
        raise SystemExit(f'Missing OpenOrbis support module: {source}')
    destination = STAGE / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
port = required('PS4_AIRPLAY_PORT')
ff = port / 'build/ffmpeg-video-fast'
objects = []
for name in ('state', 'http', 'render', 'main_ps4', 'audio_receiver', 'socket_compat', 'hen_bridge', 'session_trace', 'airplay_threads', 'video_receiver', 'video_profile', 'video_pacer', 'now_playing', 'player_ui', 'direct_player'):
    obj = BUILD / (name + '.o')
    run(cc, '--target=x86_64-pc-freebsd12-elf', '-std=c11', '-D_DEFAULT_SOURCE', '-DLAB_PS4',
        '-fPIC', '-funwind-tables', '-fno-stack-protector', '-O2', '-g',
        '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
        '-isysroot', sdk, '-isystem', sdk / 'include',
        '-I', ROOT / 'include', '-I', ROOT / 'generated', '-I', ROOT / 'third_party/stb', '-Wno-unused-function',
        '-DPLIST_230', '-DOPENSSL_API_COMPAT=0x10101000L', '-Wno-deprecated-declarations',
        '-I', port / 'vendor/uxplay-lib', '-I', port / 'build/crypto/include',
        '-I', port.parent / 'research/openssl-3.5.4/include',
        '-I', port.parent / 'research/ffmpeg-7.1.2', '-I', ff,
        '-c', ROOT / 'src' / (name + '.c'), '-o', obj)
    objects.append(obj)
elf = BUILD / 'receiver.elf'
run(ld, '-m', 'elf_x86_64', '-pie', '--script', sdk / 'link.x', '--eh-frame-hdr',
    '--gc-sections', '-z', 'max-page-size=0x4000', '-L', sdk / 'lib',
    *objects, sdk / 'lib/crt1.o',
    '--wrap=bind', '--wrap=connect', '--wrap=sendto',
    '--start-group', port / 'build/core/libairplay.a', port / 'build/crypto/libcrypto.a',
    port / 'build/plist/src/.libs/libplist-2.0.a',
    ff / 'libavcodec/libavcodec.a', ff / 'libswscale/libswscale.a',
    ff / 'libswresample/libswresample.a', ff / 'libavutil/libavutil.a',
    '--end-group', '-lSceSysmodule', '-lc', '-lkernel', '-lSceVideoOut',
    '-lSceNet', '-lSceAudioOut', '-lScePad', '-lSceUserService', '-lSceNetCtl', '-o', elf)
run(fself, '-in=' + str(elf), '-out=' + str(BUILD / 'receiver.oelf'),
    '--eboot', STAGE / 'eboot.bin', '--paid', '0x3800000000000011')
shutil.copyfile(ROOT / 'assets/icon0.png', STAGE / 'sce_sys/icon0.png')
sfo = STAGE / 'sce_sys/param.sfo'
if sfo.exists(): sfo.unlink()
run(pkgtool, 'sfo_new', sfo)
fields = [
    ('APP_TYPE', 'Integer', 4, '1'), ('APP_VER', 'Utf8', 8, PACKAGE_VERSION),
    ('ATTRIBUTE', 'Integer', 4, '0'), ('CATEGORY', 'Utf8', 4, 'gd'),
    ('CONTENT_ID', 'Utf8', 48, CONTENT), ('DOWNLOAD_DATA_SIZE', 'Integer', 4, '0'),
    ('SYSTEM_VER', 'Integer', 4, '0'), ('TITLE', 'Utf8', 128, 'PS4 AirPlay RC1'),
    ('TITLE_ID', 'Utf8', 12, 'ARPL00001'), ('VERSION', 'Utf8', 8, PACKAGE_VERSION),
]
for name, kind, size, value in fields:
    run(pkgtool, 'sfo_setentry', sfo, name, '--type', kind, '--maxsize', size, '--value', value)

gp4 = ET.Element('psproject', fmt='gp4', version='1000')
volume = ET.SubElement(gp4, 'volume')
ET.SubElement(volume, 'volume_type').text = 'pkg_ps4_app'
ET.SubElement(volume, 'volume_id').text = 'PS4VOLUME'
ET.SubElement(volume, 'volume_ts').text = '2026-11-01 00:00:00'
ET.SubElement(volume, 'package', content_id=CONTENT, passcode='0' * 32, storage_type='digital25', app_type='full')
chunks = ET.SubElement(volume, 'chunk_info', chunk_count='1', scenario_count='1')
chunk_list = ET.SubElement(chunks, 'chunks')
ET.SubElement(chunk_list, 'chunk', id='0', label='Chunk #0')
scenarios = ET.SubElement(chunks, 'scenarios', default_id='0')
ET.SubElement(scenarios, 'scenario', id='0', type='sp', initial_chunk_count='1', label='Scenario #0').text = '0'
files = ET.SubElement(gp4, 'files', img_no='0')
for name in ('eboot.bin', 'sce_sys/param.sfo', 'sce_sys/icon0.png', *runtime_files):
    ET.SubElement(files, 'file', targ_path=name, orig_path=str(STAGE / name))
rootdir = ET.SubElement(gp4, 'rootdir')
sysdir = ET.SubElement(rootdir, 'dir', targ_name='sce_sys')
ET.SubElement(sysdir, 'dir', targ_name='about')
ET.SubElement(rootdir, 'dir', targ_name='sce_module')
project = BUILD / 'receiver.gp4'
ET.ElementTree(gp4).write(project, encoding='utf-8', xml_declaration=True)
run(pkgtool, 'pkg_build', project, BUILD)
result = BUILD / (CONTENT + '.pkg')
if not result.exists() or result.stat().st_size < 4096:
    raise SystemExit('Package tool did not produce a non-empty PKG')
validation = subprocess.run([str(pkgtool), 'pkg_validate', '--verbose', str(result)],
                            cwd=ROOT, check=True, text=True, capture_output=True)
print(validation.stdout, end='')
# Upstream PkgTool can exit 0 even when individual validations fail.
if '[ERROR]' in validation.stdout or '[WARN]' in validation.stdout or '[OK]' not in validation.stdout:
    raise SystemExit('Package validation failed or remained incomplete')
(BUILD / 'pkg-validation.txt').write_text(validation.stdout)
# Hash validation alone does not detect missing runtime files. Inspect the
# actual inner filesystem, not just staging or the GP4 file list.
with tempfile.TemporaryDirectory(prefix='pkg-check-', dir=BUILD) as temp:
    run(pkgtool, 'pkg_extract', result, temp)
    for name in ('eboot.bin', *runtime_files):
        unpacked = Path(temp) / 'uroot' / name
        if not unpacked.is_file() or unpacked.read_bytes() != (STAGE / name).read_bytes():
            raise SystemExit(f'Package payload missing or changed: {name}')
destination = ROOT.parent / 'PS4-AirPlay-1.00-RC1.pkg'
shutil.copyfile(result, destination)
print('Built and structurally validated:', destination)
print('PS4 hardware installation/run still unverified.')
