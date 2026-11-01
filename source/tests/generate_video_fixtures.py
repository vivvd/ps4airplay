#!/usr/bin/env python3
"""Optional: regenerate original solid-color H.264 test vectors with PyAV/libx264."""
import struct
from fractions import Fraction
from pathlib import Path
import av

folder=Path(__file__).parent/'fixtures/video'
folder.mkdir(parents=True,exist_ok=True)
vectors=[('landscape',160,96,(81,90,240),'baseline','ultrafast'),
         ('portrait',96,160,(41,240,110),'baseline','ultrafast'),
         ('720p-high',1280,720,(145,54,34),'high','medium'),
         ('oversize',1920,1920,(81,90,240),'baseline','ultrafast'),
         ('long-gop',256,144,(41,240,110),'baseline','ultrafast'),
         ('1080p-high',1920,1080,(145,54,34),'high','medium')]
for name,width,height,values,profile,preset in vectors:
    codec=av.CodecContext.create('libx264','w')
    codec.width=width;codec.height=height;codec.pix_fmt='yuv420p';codec.time_base=Fraction(1,30)
    key=240 if name=='long-gop' else 6
    codec.options={'preset':preset,'tune':'zerolatency','profile':profile,
                   'x264-params':f'keyint={key}:min-keyint={key}:scenecut=0:repeat-headers=1:bframes=0'}
    packets=[]
    for i in range(1 if name=='oversize' else 24 if name=='long-gop' else 12):
        frame=av.VideoFrame(width,height,'yuv420p');frame.pts=i
        for plane,value in zip(frame.planes,values):plane.update(bytes([value])*plane.buffer_size)
        packets+=codec.encode(frame)
    packets+=codec.encode(None)
    with (folder/(name+'.packets')).open('wb') as output:
        for packet in packets:
            data=bytes(packet);output.write(struct.pack('<I',len(data)));output.write(data)
