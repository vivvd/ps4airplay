import http.server,functools,os,re,argparse
from pathlib import Path
class Handler(http.server.SimpleHTTPRequestHandler):
 def log_message(self,*args):pass
 def send_head(self):
  path=self.translate_path(self.path)
  if not os.path.isfile(path):return super().send_head()
  f=open(path,'rb');size=os.fstat(f.fileno()).st_size;first=0;last=size-1;partial=False
  value=self.headers.get('Range')
  if value:
   m=re.fullmatch(r'bytes=(\d+)-(\d*)',value)
   if not m:f.close();self.send_error(416);return None
   first=int(m[1]);last=min(int(m[2]) if m[2] else size-1,size-1);partial=True
   if first>=size or last<first:f.close();self.send_error(416);return None
  self.send_response(206 if partial else 200);self.send_header('Content-Type',self.guess_type(path));self.send_header('Content-Length',str(last-first+1));self.send_header('Accept-Ranges','bytes');self.send_header('Connection','close')
  if partial:self.send_header('Content-Range',f'bytes {first}-{last}/{size}')
  self.end_headers();f.seek(first);self.remaining=last-first+1;return f
 def copyfile(self,source,output):
  if not hasattr(self,'remaining'):return super().copyfile(source,output)
  while self.remaining:
   data=source.read(min(65536,self.remaining))
   if not data:break
   try:output.write(data)
   except (BrokenPipeError,ConnectionResetError):break
   self.remaining-=len(data)
if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=0);args=parser.parse_args()
 server=http.server.ThreadingHTTPServer(('127.0.0.1',args.port),functools.partial(Handler,directory=str(Path(__file__).parent/'fixtures/direct')))
 print('READY',server.server_address[1],flush=True);server.serve_forever()
