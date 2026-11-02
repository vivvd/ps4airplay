#!/usr/bin/env python3
"""Readiness, timeline and paused seek over the real protocol + FFmpeg engine."""
from pathlib import Path
import subprocess,socket,http.client,plistlib,time
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
log=Path('build/release/engine-control-server.log').open('w+')
media=subprocess.Popen(['python3','tests/direct_http_server.py'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
p=None
try:
 line=media.stdout.readline();assert line.startswith('READY ');url=f'http://127.0.0.1:{line.split()[1]}/av-test.mp4'
 p=subprocess.Popen(['build/release/test-engine-control-server',str(port)],stdout=log,stderr=log)
 for _ in range(300):
  log.flush();log.seek(0)
  if 'READY ' in log.read():break
  assert p.poll() is None;time.sleep(.01)
 else:raise RuntimeError('Engine server not ready')
 def request(method,path,body=b'',content='text/parameters'):
  c=http.client.HTTPConnection('127.0.0.1',port,timeout=3);c.request(method,path,body,{'Content-Type':content});r=c.getresponse();status=r.status;data=r.read();c.close();assert status==200,(status,path,data);return data
 def info():
  d=plistlib.loads(request('GET','/playback-info'))
  for key in ['readyToPlay','playbackBufferEmpty','playbackBufferFull','playbackLikelyToKeepUp']:assert type(d[key]) is bool,(key,d[key])
  for key in ['loadedTimeRanges','seekableTimeRanges']:
   for item in d[key]:assert item['duration']>=0
  return d
 def wait(test):
  until=time.monotonic()+5
  while time.monotonic()<until:
   d=info()
   if test(d):return d
   time.sleep(.02)
  raise AssertionError(('Timed out',d))
 def scrub():return {k:float(v) for k,v in (line.split(':',1) for line in request('GET','/scrub').decode().splitlines())}
 assert not info()['readyToPlay'] and info()['seekableTimeRanges']==[]
 body=plistlib.dumps({'Content-Location':url,'Start-Position-Seconds':0},fmt=plistlib.FMT_BINARY)
 request('POST','/play',body,'application/x-apple-binary-plist');request('POST','/rate?value=0')
 d=wait(lambda d:d['readyToPlay'] and d['rate']==0);assert abs(d['duration']-12)<.1;assert abs(d['seekableTimeRanges'][0]['duration']-12)<.1
 paused=d['position'];time.sleep(.15);assert abs(info()['position']-paused)<.1
 request('POST','/rate?value=1');d=wait(lambda d:d['position']>.3);assert d['readyToPlay'] and d['rate']==1
 current=scrub();assert abs(current['duration']-12)<.1 and current['position']>.2
 request('POST','/rate?value=0');wait(lambda d:d['rate']==0)
 request('POST','/scrub?position=6');d=wait(lambda d:abs(d['position']-6)<.1);assert d['readyToPlay'] and d['rate']==0 and abs(scrub()['position']-6)<.1
 request('POST','/rate?value=1');wait(lambda d:d['position']>6.3)
 request('POST','/stop');d=wait(lambda d:not d['readyToPlay']);assert d['rate']==0 and d['seekableTimeRanges']==[] and d['loadedTimeRanges']==[]
 print('Real engine over HTTP: boolean readiness, initial pause then ready/resume, duration/progress, GET scrub read-only, paused seek/resume, empty idle ranges passed')
 # Quit during active playback with an established control socket.
 request('POST','/play',body,'application/x-apple-binary-plist');wait(lambda d:d['readyToPlay'])
 control=socket.create_connection(('127.0.0.1',port),timeout=3)
 control.sendall(b'GET /playback-info HTTP/1.1\r\nHost: localhost\r\n\r\n')
 control.recv(8192)
 p.terminate();p.wait(timeout=5)
 control.settimeout(2)
 # Drain a possible tail, then require EOF (the sender is disconnected).
 while control.recv(8192):pass
 control.close()
 with socket.socket() as closed:
  closed.settimeout(1);assert closed.connect_ex(('127.0.0.1',port))!=0
 print('Active playback exit: bounded shutdown, control socket EOF and closed listener passed')
finally:
 if p and p.poll() is None:p.terminate();p.wait(timeout=10)
 media.terminate();media.communicate(timeout=5);log.close()
 if p and p.returncode:raise RuntimeError(f'Engine process exit {p.returncode}; see engine-control-server.log')
