#!/usr/bin/env python3
"""Offline TLS verification, HTTP Range/chunks/redirects and actual AV playback."""
import ssl,http.server,tempfile,subprocess,threading,functools,os,time
from pathlib import Path
from direct_http_server import Handler as MediaHandler
root=Path(__file__).resolve().parents[1]
class Handler(MediaHandler):
 def do_GET(self):
  path=self.path.split('?',1)[0]
  if path in ['/redirect','/downgrade']:
   self.send_response(302);scheme='http' if path=='/downgrade' else 'https'
   self.send_header('Location',f'{scheme}://localhost:{self.server.server_port}/data?secret=synthetic');self.send_header('Content-Length','0');self.end_headers();return
  if path=='/slow':time.sleep(6);return
  if path=='/interim':
   self.wfile.write(b'HTTP/1.1 103 Early Hints\r\nLink: </hint>; rel=preload\r\n\r\n');path='/data'
  if path in ['/data','/chunked','/ignore','/bad-range','/truncated']:
   payload=b'0123456789abcdefghijklmnopqrstuvwxyz';offset=0
   if path not in ['/chunked','/ignore','/truncated']:offset=int(self.headers.get('Range','bytes=0-').split('=')[1].split('-')[0])
   if offset>=len(payload):self.send_error(416);return
   self.send_response(206 if path in ['/data','/bad-range'] else 200)
   if path=='/chunked':self.send_header('Transfer-Encoding','chunked')
   else:self.send_header('Content-Length',str(len(payload)-offset))
   if path in ['/data','/bad-range']:self.send_header('Content-Range',f'bytes {offset+(path=="/bad-range")}-{len(payload)-1}/{len(payload)}')
   self.send_header('Connection','close');self.end_headers();self.close_connection=True
   if path=='/chunked':
    for segment in [payload[:7],payload[7:15],payload[15:]]:self.wfile.write(f'{len(segment):x};test=yes\r\n'.encode()+segment+b'\r\n')
    self.wfile.write(b'0\r\nX-Trailer: ignored\r\n\r\n')
   else:self.wfile.write(payload[offset:10] if path=='/truncated' else payload[offset:])
   return
  super().do_GET()
 def log_error(self,*args):pass
with tempfile.TemporaryDirectory(prefix='ps4-tls-test-') as folder:
 folder=Path(folder);cert=folder/'cert.pem';key=folder/'key.pem'
 openssl='/opt/homebrew/opt/openssl@3/bin/openssl'
 if not Path(openssl).exists():openssl='openssl'
 subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-days','2','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(root/'tests/fixtures/direct')))
 context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key);server.socket=context.wrap_socket(server.socket,server_side=True)
 thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();base=f'https://localhost:{server.server_port}'
 try:
  for path,trust,mode in [('/data?secret=synthetic',str(cert),'normal'),('/redirect',str(cert),'normal'),('/interim',str(cert),'normal'),('/chunked',str(cert),'chunked'),('/ignore',str(cert),'ignore'),('/bad-range',str(cert),'fail'),('/truncated',str(cert),'truncated'),('/downgrade',str(cert),'fail'),('/data','-','verify'),('/slow',str(cert),'fail'),('/slow',str(cert),'cancel')]:
   subprocess.run(['build/release/test-tls-http',base+path,trust,mode],check=True,timeout=12,cwd=root)
  subprocess.run(['build/release/test-tls-http',f'https://127.0.0.1:{server.server_port}/data',str(cert),'verify'],check=True,timeout=12,cwd=root)
  subprocess.run(['build/release/test-direct-ffmpeg',f'https://127.0.0.1:{server.server_port}'],check=True,env={**os.environ,'PS4_TEST_CA':str(cert),'PS4_EXPECT_TLS_FAIL':'1'},timeout=10,cwd=root)
  subprocess.run(['build/release/test-direct-ffmpeg',base],check=True,env={**os.environ,'PS4_TEST_CA':str(cert)},timeout=45,cwd=root)
 finally:server.shutdown();server.server_close();thread.join(timeout=3)
print('Verified TLS: trusted certificate, untrusted certificate and wrong hostname refusal; redirects, Range/seek, chunking, truncation, interim headers, timeout/cancellation; real HTTPS MP4+AAC/HLS/fMP4/pause/seek passed')
