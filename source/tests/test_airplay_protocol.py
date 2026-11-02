import socket,subprocess,time,http.client,plistlib
from pathlib import Path
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
log=Path('build/release/protocol-server.log').open('w+')
p=subprocess.Popen(['build/release/test-airplay-server',str(port)],stdout=log,stderr=log)
try:
 for i in range(200):
  log.flush();log.seek(0)
  if 'READY' in log.read():break
  if p.poll() is not None:raise RuntimeError('Server failed; see log')
  time.sleep(.01)
 else:raise RuntimeError('No READY')
 def request(method,path,body=b'',content='text/parameters',session=True,cseq=False):
  c=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
  headers={'Content-Type':content}
  if cseq:headers['CSeq']='1'
  if session:headers['X-Apple-Session-ID']='11111111-1111-4111-8111-111111111111'
  c.request(method,path,body,headers);r=c.getresponse();data=r.read();status=r.status;c.close();return status,data
 # Browser discovery and playback share a keep-alive connection. Discovery
 # has no session header; the real session appears only on a later request.
 c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
 c.request('GET','/server-info');reply=c.getresponse()
 assert reply.status==200 and 'features' in plistlib.loads(reply.read())
 wire=c.sock
 # Safari's HTTP negotiation must return the actual binary messages, not an
 # empty 200. Exercise both stages on this discovery connection before its ID.
 for mode in range(4):
  setup=bytearray.fromhex('46504c590301010000000004020000bb');setup[14]=mode
  c.request('POST','/fp-setup',bytes(setup),{'Content-Type':'application/octet-stream'})
  reply=c.getresponse();assert reply.status==200;assert reply.getheader('Content-Type')=='application/octet-stream'
  message=reply.read();assert len(message)==142 and message[:12]==bytes.fromhex('46504c590301020000000082') and message[13]==mode
  assert c.sock is wire
 handshake=bytearray(164);handshake[:12]=bytes.fromhex('46504c590301030000000098');handshake[144:]=bytes(range(20))
 c.request('POST','/fp-setup',bytes(handshake),{'Content-Type':'application/octet-stream'})
 reply=c.getresponse();message=reply.read();assert reply.status==200 and len(message)==32
 assert message[:12]==bytes.fromhex('46504c590301040000000014') and message[12:]==bytes(range(20));assert c.sock is wire
 h={'X-Apple-Session-ID':'33333333-3333-4333-8333-333333333333','Content-Type':'text/parameters'}
 c.request('POST','/play',b'Content-Location: https://test.invalid/session.mp4\r\nStart-Position: 0\r\n',h)
 reply=c.getresponse();assert reply.status==200,reply.reason
 assert reply.getheader('Content-Length')=='0';assert reply.read()==b'';assert c.sock is wire
 # Headerless polling after binding must retain the session, without inventing a new ID.
 c.request('GET','/playback-info');reply=c.getresponse();assert reply.status==200;assert plistlib.loads(reply.read())['readyToPlay']
 c.request('POST','/rate?value=0',headers=h);reply=c.getresponse();assert reply.status==200;assert reply.read()==b'';assert c.sock is wire
 c.request('POST','/stop');reply=c.getresponse();assert reply.status==200;assert reply.read()==b''
 # A genuine change of session is still rejected and the old connection closed.
 h['X-Apple-Session-ID']='44444444-4444-4444-8444-444444444444'
 c.request('POST','/stop',headers=h);reply=c.getresponse();assert reply.status==400;assert reply.read()==b'';c.close()
 # Reconnect normally after a rejected session.
 assert request('GET','/server-info')[0]==200
 # Parser-level rejects must produce a complete, framed response and trace.
 c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
 c.request('POST','/stop',headers={**h,'CSeq':'invalid'});reply=c.getresponse();assert reply.status==400;assert reply.read()==b'';c.close()
 c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
 c.request('GET','/server-info',headers={'Long':'x'*1025});reply=c.getresponse();assert reply.status==431;assert reply.read()==b'';c.close()
 # Malformed setup must report errors rather than an empty success or invalid
 # memory access. Preserve HTTP status line even for unsupported versions.
 for body in [b'',b'x',b'FPLY',bytes(16),bytes(164),b'FPLY'+bytes(159),b'FPLY'+bytes(161)]:
  assert request('POST','/fp-setup',body,'application/octet-stream')[0]==400
 setup=bytearray.fromhex('46504c590301010000000004020000bb')
 for mode in [4,127,255]:
  setup[14]=mode;assert request('POST','/fp-setup',bytes(setup),'application/octet-stream')[0]==400
 setup[14]=0;setup[4]=5
 assert request('POST','/fp-setup',bytes(setup),'application/octet-stream')[0]==501
 assert request('POST','/fp-setup2',b'','application/octet-stream')[0]==403
 # The shared handler must still support RTSP mirroring negotiation.
 rtsp=socket.create_connection(('127.0.0.1',port),timeout=2);rf_rtsp=rtsp.makefile('rb')
 def rtsp_setup(body):
  rtsp.sendall(b'POST /fp-setup RTSP/1.0\r\nCSeq: 1\r\nContent-Type: application/octet-stream\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
  status=rf_rtsp.readline();headers={}
  while True:
   line=rf_rtsp.readline()
   if line==b'\r\n':break
   assert line
   key,value=line.decode().split(':',1);headers[key.lower()]=value.strip()
  return status,rf_rtsp.read(int(headers.get('content-length','0')))
 setup[4]=3;status,message=rtsp_setup(bytes(setup));assert status.startswith(b'RTSP/1.0 200') and len(message)==142
 status,message=rtsp_setup(bytes(handshake));assert status.startswith(b'RTSP/1.0 200') and len(message)==32
 setup[14]=255;assert rtsp_setup(bytes(setup))[0].startswith(b'RTSP/1.0 400')
 rf_rtsp.close();rtsp.close()
 # Actual host core HTTP parser + handlers + callback integration.
 assert request('POST','/play',b'Content-Location: https://test.invalid/a.mp4\r\nStart-Position: 0.25\r\n',session=False)[0]==200
 body=plistlib.dumps({'Content-Location':'https://test.invalid/b.mp4','Start-Position-Seconds':8.5},fmt=plistlib.FMT_BINARY)
 assert request('POST','/play',body,'application/x-apple-binary-plist')[0]==200
 assert request('POST','/play',body,'application/x-apple-binary-plist',cseq=True)[0]==200
 status,data=request('GET','/playback-info');assert status==200
 info=plistlib.loads(data);assert info['duration']==120 and info['position']==12 and info['readyToPlay']
 for name in ['readyToPlay','playbackBufferEmpty','playbackBufferFull','playbackLikelyToKeepUp']:
  assert type(info[name]) is bool,(name,info[name])
 assert info['seekableTimeRanges']==[{'start':0.0,'duration':120.0}]
 assert info['loadedTimeRanges']==[{'start':12.0,'duration':108.0}]
 status,data=request('GET','/scrub',session=False);assert status==200
 values={k:float(v) for k,v in (line.split(':',1) for line in data.decode().splitlines())}
 assert values=={'duration':120.0,'position':12.0},values
 assert request('POST','/scrub')[0]==400

 assert request('POST','/scrub?position=42')[0]==200
 assert request('POST','/rate?value=0')[0]==200
 assert request('POST','/rate?value=1')[0]==200
 assert request('POST','/stop')[0]==200
 for path in ['/scrub?bad','/scrub?position=nan','/rate?bad','/rate?value=nan']:
  assert request('POST',path)[0]==400
 assert request('POST','/play',b'garbage')[0]==400
 assert request('POST','/play',plistlib.dumps({'Content-Location':5},fmt=plistlib.FMT_BINARY),'application/x-apple-binary-plist')[0]==400
 assert request('POST','/play',b'Content-Location: https://test.invalid/'+b'x'*8200)[0]==400
 # Decoder errors and unsupported schemes must not crash server.
 assert request('POST','/play',b'bplist00bad','application/x-apple-binary-plist')[0]==400
 assert request('POST','/play',b'Content-Location: file:///tmp/test')[0]==400
 # Exercise the complete reverse-FCUP playlist exchange, then native localhost GETs.
 reverse=socket.create_connection(('127.0.0.1',port),timeout=5)
 reverse.sendall(b'POST /reverse HTTP/1.1\r\nHost: localhost\r\nX-Apple-Session-ID: 11111111-1111-4111-8111-111111111111\r\nX-Apple-Purpose: event\r\nConnection: Upgrade\r\nUpgrade: PTTH/1.0\r\nContent-Length: 0\r\n\r\n')
 rf=reverse.makefile('rb')
 def read_message():
  first=rf.readline();headers={}
  while True:
   line=rf.readline()
   if line in (b'\r\n',b'\n',b''):break
   key,value=line.decode().split(':',1);headers[key.lower()]=value.strip()
  return first,rf.read(int(headers.get('content-length',0)))
 assert b'101' in read_message()[0]
 source='x-apple-hls://test.invalid/item/master.m3u8'
 assert request('POST','/play',plistlib.dumps({'uuid':'22222222-2222-4222-8222-222222222222','Content-Location':source,'Start-Position-Seconds':3.0,'clientProcName':'YouTube'},fmt=plistlib.FMT_BINARY),'application/x-apple-binary-plist')[0]==200
 def fcup_reply(expected,playlist):
  first,body=read_message();assert first.startswith(b'POST /event')
  params=plistlib.loads(body)['request'];assert params['FCUP_Response_URL']==expected,params
  reverse.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n')
  params.update(FCUP_Response_Data=playlist,FCUP_Response_StatusCode=200)
  payload=plistlib.dumps({'type':'unhandledURLResponse','params':params},fmt=plistlib.FMT_BINARY)
  assert request('POST','/action',payload,'application/x-apple-binary-plist')[0]==200
 master=b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=1280x720,CODECS="avc1.4d401f,mp4a.40.2"\nx-apple-hls://test.invalid/item/video.m3u8\n'
 fcup_reply(source,master)
 fcup_reply('x-apple-hls://test.invalid/item/video.m3u8',b'#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:6.000,\nhttps://cdn.invalid/segment.ts\n#EXT-X-ENDLIST\n')
 status,master_out=request('GET','/master.m3u8',session=False);assert status==200 and b'http://127.0.0.1:' in master_out,master_out
 media_path=[line for line in master_out.decode().splitlines() if line.startswith('http://127.0.0.1:')][0].split(str(port),1)[1]
 status,media_out=request('GET',media_path,session=False);assert status==200 and b'https://cdn.invalid/segment.ts' in media_out,media_out
 empty=plistlib.dumps({'type':'unhandledURLResponse','params':{'FCUP_Response_URL':source,'FCUP_Response_Data':b''}},fmt=plistlib.FMT_BINARY)
 assert request('POST','/action',empty,'application/x-apple-binary-plist')[0]==400
 rf.close();reverse.close()
 time.sleep(.03);log.flush();log.seek(0);text=log.read()
 assert 'PROTOCOL HTTP POST /play -> 200' in text,text
 assert 'PROTOCOL HTTP POST /stop -> 400' in text,text
 assert 'PROTOCOL HTTP GET /server-info -> 431' in text,text
 assert all('test.invalid' not in line and '33333333' not in line for line in text.splitlines() if line.startswith('PROTOCOL ')),text
 assert 'FRACTION 0.250 https://test.invalid/a.mp4' in text,text
 assert 'PLAY 8.500 https://test.invalid/b.mp4' in text,text
 assert 'SEEK 0.000' not in text,text
 assert 'SEEK 42.000' in text and 'RATE 0.000' in text and 'STOP' in text,text
 assert f'PLAY 3.000 http://127.0.0.1:{port}/master.m3u8' in text,text
 print('Safari HTTP setup: four modes, both binary response stages, same discovery/play socket, malformed lengths/modes/versions, RTSP regression passed')
 print('AirPlay HLS: reverse upgrade, FCUP master/media exchange, localhost rewrite and playlist serving passed')
 print('AirPlay HTTP: legacy play without session/UUID, binary play, fraction/seconds, playback-info, seek/rate/stop and 10 malformed requests passed')
finally:
 p.terminate();p.wait(timeout=10);log.close()
 if p.returncode:raise RuntimeError(f'Server exit={p.returncode}; see protocol-server.log')
