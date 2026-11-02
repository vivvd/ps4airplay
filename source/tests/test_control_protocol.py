#!/usr/bin/env python3
"""Actual core plus an independent Go standard-library encrypted client."""
import socket,subprocess,time,json,http.client
from pathlib import Path
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
log=Path('build/release/control-server.log').open('w+')
p=subprocess.Popen(['build/release/test-airplay-server',str(port)],stdout=log,stderr=log)
try:
 for _ in range(300):
  log.flush();log.seek(0)
  if 'READY' in log.read():break
  assert p.poll() is None
  time.sleep(.01)
 else:raise RuntimeError('No control server ready')
 row=json.loads(Path('tests/fixtures/control.json').read_text())['sessions'][0]
 def attempt(setup,bad=False,mix=False):
  c=http.client.HTTPConnection('127.0.0.1',port,timeout=3)
  def post(path,b,headers=None):
   c.request('POST',path,b,{'Content-Type':'application/octet-stream',**(headers or {})});r=c.getresponse();data=r.read();return r.status,data
  if setup==1:
   assert post('/fp-setup',bytes.fromhex('46504c590301010000000004020000bb'))[0]==200
  if setup:
   assert post('/fp-setup',bytes.fromhex(row['m3']))[0]==200
  envelope=bytearray.fromhex(row['envelope'])
  if bad:envelope[-1]^=1
  assert post('/fp-setup2',bytes(envelope),{'X-Apple-PD':'1'} if mix else {})[0]==403
  c.close()
 attempt(False);attempt(2);attempt(True,bad=True);attempt(True,mix=True)
 subprocess.run(['build/release/control-client',f'127.0.0.1:{port}','tests/fixtures/control.json'],check=True)
 time.sleep(.02);log.flush();log.seek(0);text=log.read()
 assert 'FP control: encrypted connection active' in text
 assert 'FRACTION 0.000 https://test.invalid/encrypted.mp4' in text
 assert 'runtime error:' not in text and 'ERROR: AddressSanitizer' not in text,text[-3000:]
 print('Control integration: no SAP, tampered envelope and unverified key mixing refused; real encrypted client integration passed')
finally:
 p.terminate();p.wait(timeout=10);log.close()
 if p.returncode:raise RuntimeError(f'Control server exit={p.returncode}; see control-server.log')
