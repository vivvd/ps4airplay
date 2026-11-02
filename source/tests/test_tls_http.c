/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
static uint64_t ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static int cancel(void *p){return ms()>=*(uint64_t *)p;}
int main(int argc,char **argv){assert(argc==4);signal(SIGPIPE,SIG_IGN);
 if(strcmp(argv[2],"-")){FILE *f=fopen(argv[2],"rb");assert(f);char pem[10000];size_t n=fread(pem,1,sizeof(pem),f);fclose(f);assert(n && !tls_http_test_ca(pem,n));}
 uint64_t started=ms(),deadline=started+200;if(!strcmp(argv[3],"cancel"))tls_http_interrupt(cancel,&deadline);
 MediaHttp api={.verified_tls=1};MediaFile file;media_file_init(&file,&api,0);int rc=media_file_open(&file,argv[1]);
 if(!strcmp(argv[3],"fail") || !strcmp(argv[3],"verify") || !strcmp(argv[3],"cancel")){assert(rc<0);if(!strcmp(argv[3],"cancel"))assert(ms()-started<1500);if(!strcmp(argv[3],"verify"))assert(file.diagnostics.verify>0 && !strcmp(file.diagnostics.phase,"tls-verify"));}
 else{
  if(rc<0)fprintf(stderr,"TLS open failed: phase=%s verify=%ld ssl=%lx rc=%d\n",file.diagnostics.phase,file.diagnostics.verify,file.diagnostics.ssl_error,rc);assert(!rc);
  unsigned char out[100];
  if(!strcmp(argv[3],"truncated")){assert(media_file_read(&file,out,0,36)<0);}
  else if(!strcmp(argv[3],"chunked")){assert(media_file_size(&file)==UINT64_MAX);assert(media_file_read(&file,out,0,60)==36 && !memcmp(out,"0123456789abcdefghijklmnopqrstuvwxyz",36));assert(media_file_read(&file,out,36,1)==0);}
  else{assert(media_file_size(&file)==36);assert(media_file_read(&file,out,0,8)==8 && !memcmp(out,"01234567",8));assert(media_file_read(&file,out,8,4)==4 && !memcmp(out,"89ab",4));
   if(!strcmp(argv[3],"ignore"))assert(media_file_read(&file,out,2,4)<0);
   else{assert(media_file_read(&file,out,2,4)==4 && !memcmp(out,"2345",4));assert(media_file_read(&file,out,35,8)==1 && out[0]=='z');assert(media_file_read(&file,out,36,1)==0);}
  }
 }
 assert(!strstr(file.diagnostics.origin,"secret"));media_file_destroy(&file);tls_http_interrupt(NULL,NULL);tls_http_shutdown();tls_http_test_ca(NULL,0);return 0;
}
