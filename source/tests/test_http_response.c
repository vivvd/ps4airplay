/* Response framing must preserve RTSP/music and the reverse HTTP upgrade. */
#include "http_response.h"
#include <assert.h>
#include <string.h>
static void check(http_response_t *r,int lengths,const char *tail){
 int n=0,count=0;const char *data=http_response_get_data(r,&n);
 for(int i=0;i+15<=n;++i)if(!memcmp(data+i,"Content-Length:",15))++count;
 assert(count==lengths);assert(n>=(int)strlen(tail));assert(!memcmp(data+n-strlen(tail),tail,strlen(tail)));
}
int main(void){
 http_response_t *r=http_response_create();assert(r);
 http_response_init(r,"HTTP/1.1",200,"OK");http_response_finish(r,0,0);check(r,1,"Content-Length: 0\r\n\r\n");
 http_response_init(r,"HTTP/1.1",200,"OK");http_response_add_header(r,"Content-Length","0");http_response_finish(r,0,0);check(r,1,"\r\n\r\n");
 http_response_init(r,"HTTP/1.1",200,"OK");http_response_finish(r,"abc",3);check(r,1,"\r\n\r\nabc");
 int codes[]={101,204,304};for(unsigned i=0;i<sizeof(codes)/sizeof(*codes);++i){http_response_init(r,"HTTP/1.1",codes[i],"Status");http_response_finish(r,0,0);check(r,0,"\r\n\r\n");}
 http_response_init(r,"RTSP/1.0",200,"OK");http_response_finish(r,0,0);check(r,0,"\r\n\r\n");
 http_response_init(r,"RTSP/1.0",200,"OK");http_response_add_header(r,"Content-Type","application/octet-stream");http_response_finish(r,0,0);check(r,1,"Content-Length: 0\r\n\r\n");
 http_response_reverse_request_init(r,"POST","/event","HTTP/1.1");http_response_finish(r,"abc",3);check(r,1,"\r\n\r\nabc");http_response_destroy(r);return 0;
}
