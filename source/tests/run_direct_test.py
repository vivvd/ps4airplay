#!/usr/bin/env python3
import subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
server=subprocess.Popen(['python3','tests/direct_http_server.py'],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
 line=server.stdout.readline();assert line.startswith('READY '),line
 subprocess.run(['build/release/test-direct-ffmpeg','http://127.0.0.1:'+line.split()[1]],cwd=root,check=True)
finally:
 server.terminate();server.communicate(timeout=5)
