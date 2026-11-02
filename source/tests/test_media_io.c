/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_io.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
static int requests,deleted,start,pos,ignore_range,bad_range,fail_read,unknown_length;
static const char payload[]="0123456789abcdefghijklmnopqrstuvwxyz";
static int template_create(int ctx,const char *ua,int version,int proxy){assert(ctx==7 && ua && version==2 && proxy==1);return 10;}
static int connection_create(int t,const char *url,int keep){assert(t==10 && !strncmp(url,"https://",8) && keep);return 11;}
static int request_create(int c,int method,const char *url,uint64_t n){assert(c==11 && method==0 && url && !n);++requests;return 12;}
static int header_add(int r,const char *name,const char *value,int mode){assert(r==12 && !strcmp(name,"Range") && mode==0);assert(sscanf(value,"bytes=%d-",&start)==1);pos=start;return 0;}
static int send_request(int r,const void *data,size_t n){assert(r==12 && !data && !n);return 0;}
static int status(int r,int *s){assert(r==12);*s=ignore_range?200:206;return 0;}
static int length(int r,int *kind,size_t *n){assert(r==12);*kind=unknown_length?2:0;*n=unknown_length?0:sizeof(payload)-1;return 0;}
static int headers(int r,char **out,size_t *n){assert(r==12);static char text[200];snprintf(text,sizeof(text),"content-range: bytes %d-%zu/%zu\r\n",bad_range?start+1:start,sizeof(payload)-2,sizeof(payload)-1);*out=text;*n=strlen(text);return 0;}
static int read_data(int r,void *out,uint32_t n){assert(r==12);if(fail_read)return -33;unsigned left=(unsigned)strlen(payload)-(unsigned)pos;if(n>left)n=left;if(n>3)n=3;memcpy(out,payload+pos,n);pos+=(int)n;return (int)n;}
static int delete_request(int id){assert(id==12);++deleted;return 0;}
static int delete_connection(int id){assert(id==11);return 0;}
static int delete_template(int id){assert(id==10);return 0;}
int main(void){
 MediaHttp api={template_create,connection_create,request_create,header_add,send_request,status,length,headers,read_data,delete_request,delete_connection,delete_template,0};
 MediaFile file;media_file_init(&file,&api,7);uint8_t out[64];
 assert(media_file_open(&file,"https://test.invalid/media.mp4")==0);assert(media_file_size(&file)==36);
 assert(media_file_read(&file,out,0,8)==8 && !memcmp(out,payload,8));
 assert(media_file_read(&file,out,8,4)==4 && requests==1);
 assert(media_file_read(&file,out,2,4)==4 && !memcmp(out,"2345",4) && requests==2);
 assert(media_file_read(&file,out,35,8)==1 && out[0]=='z');assert(media_file_read(&file,out,36,1)==0);
 bad_range=1;assert(media_file_read(&file,out,0,1)<0);bad_range=0;media_file_close(&file);
 ignore_range=1;assert(media_file_open(&file,"https://test.invalid/media.mp4")==0);assert(media_file_read(&file,out,2,1)<0);media_file_close(&file);
 ignore_range=1;unknown_length=1;assert(media_file_open(&file,"https://test.invalid/media.mp4")==0 && media_file_size(&file)==UINT64_MAX);assert(media_file_read(&file,out,0,40)==36);assert(media_file_read(&file,out,36,1)==0);media_file_close(&file);unknown_length=0;
 ignore_range=0;assert(media_file_open(&file,"https://test.invalid/media.mp4")==0);fail_read=1;assert(media_file_read(&file,out,0,1)==-33);fail_read=0;media_file_close(&file);
 char path[]="/tmp/ps4-media-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && write(fd,payload,36)==36);close(fd);
 assert(media_file_open(&file,path)==0 && media_file_size(&file)==36);assert(media_file_read(&file,out,10,4)==4 && !memcmp(out,"abcd",4));assert(media_file_read(&file,out,35,8)==1);
 media_file_close(&file);unlink(path);assert(media_file_open(&file,path)<0);media_file_destroy(&file);assert(requests==deleted);
 puts("Media IO: kernel-file adapter, sequential HTTP reads, Range seeks, lowercase headers, EOF, ignored/malformed ranges, read failures and cleanup passed");
}
