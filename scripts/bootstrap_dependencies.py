#!/usr/bin/env python3
"""Fetch verified RC1 source dependencies without replacing tracked app/vendor code."""
from pathlib import Path, PurePosixPath
from zipfile import ZipFile
import argparse
import hashlib
import re
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ASSET = 'PS4-AirPlay-1.00-RC1-source.zip'
SHA256 = '6bd4ae350c306d47a71d12cb199ea546556e0f5c700e497637d54f03aec2c8f7'
PREFIX = 'ps4-airplay-1.00-rc1/'
ALLOWED = ('work/research/', 'work/airplay-port/build/')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Use an existing full source ZIP')
    parser.add_argument('--repo', help='GitHub owner/repository; default: git origin')
    args = parser.parse_args()
    archive = args.archive
    if archive is None:
        repo = args.repo
        if not repo:
            remote = subprocess.check_output(['git', 'remote', 'get-url', 'origin'], cwd=ROOT, text=True).strip()
            match = re.fullmatch(r'(?:https://github\.com/|git@github\.com:)([\w.-]+/[\w.-]+?)(?:\.git)?/?', remote)
            if not match:
                parser.error('Use --repo owner/repository or --archive /path/to/source.zip')
            repo = match.group(1)
        if not re.fullmatch(r'[\w.-]+/[\w.-]+', repo):
            parser.error('Expected --repo owner/repository')
        cache = ROOT / 'work/cache'
        cache.mkdir(parents=True, exist_ok=True)
        archive = cache / ASSET
        if not archive.exists():
            url = f'https://github.com/{repo}/releases/download/v1.00-rc1/{ASSET}'
            temporary = archive.with_suffix('.download')
            print('Downloading', url)
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            temporary.replace(archive)
    archive = archive.expanduser().resolve()
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
        raise SystemExit(f'SHA256 mismatch; archive was not extracted: {digest}')
    count = 0
    with ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            if not entry.filename.startswith(PREFIX):
                continue
            relative = entry.filename[len(PREFIX):]
            if not relative.startswith(ALLOWED) or entry.is_dir():
                continue
            path = PurePosixPath(relative)
            if path.is_absolute() or '..' in path.parts:
                raise SystemExit('Unsafe archive path')
            destination = (ROOT / relative).resolve()
            if not destination.is_relative_to(ROOT):
                raise SystemExit('Archive destination escapes repository')
            destination.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(entry) as source, destination.open('wb') as output:
                shutil.copyfileobj(source, output)
            count += 1
    if not count:
        raise SystemExit('No dependency files found')
    print(f'SHA256 verified; extracted {count} dependency files. App/vendor code preserved.')

if __name__ == '__main__':
    main()
