/* Discovery leases must expire after crashes and be withdrawn on clean exit. */
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static time_t ticks;
static int sends;
static int fake_clock(clockid_t id,struct timespec *ts){(void)id;ts->tv_sec=ticks;ts->tv_nsec=0;return 0;}
static ssize_t capture(int fd,const void *data,size_t n,int flags,const struct sockaddr *to,socklen_t len){(void)fd;(void)data;(void)flags;(void)to;(void)len;++sends;return (ssize_t)n;}
static int tick_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv);
#define clock_gettime fake_clock
#define sendto capture
#define select tick_select
#include MDNS_SOURCE
static mdnsd_t *active;
static int tick_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv){(void)n;(void)r;(void)w;(void)e;(void)tv;if(++ticks==12)active->running=0;return 0;}
static unsigned u16(const unsigned char *p){return (unsigned)p[0]*256+p[1];}
static size_t name_end(const unsigned char *p,size_t at,size_t cap){while(at<cap && p[at]){unsigned n=p[at++];assert(n<64 && at+n<cap);at+=n;}assert(at<cap);return at+1;}
static void check_packet(mdns_packet_t *p,unsigned ttl){
 size_t at=12;unsigned count=u16(p->bytes+6);assert(count==5);
 for(unsigned i=0;i<count;++i){at=name_end(p->bytes,at,p->length);assert(at+10<=p->length);unsigned value=0;for(int j=0;j<4;++j)value=(value<<8)|p->bytes[at+4+j];assert(value==ttl);at+=10+u16(p->bytes+at+8);assert(at<=p->length);}assert(at==p->length);
}
int main(void){
 mdnsd_t *d=mdnsd_init("lease-test.local");assert(d);active=d;
 mdnsd_service_t service={0};strcpy(service.name,"Lease Test._airplay._tcp.local");strcpy(service.type,"_airplay._tcp.local");service.registered=1;service.port=7000;
 d->ipv4_addr=htonl(0x7f000001);d->sock_fd4=42;d->airplay=service;
 mdns_packet_t p;assert(!mdns_build_response(d,&p,&service,1,MDNSD_TTL_SERVICE,MDNS_FAMILY_IPV4));check_packet(&p,15);
 assert(!mdns_build_response(d,&p,&service,1,0,MDNS_FAMILY_IPV4));check_packet(&p,0);
 d->running=1;mdns_thread(d);assert(sends==2); /* renew at 5s and 10s */
 int before=sends;mdnsd_goodbye(d,&service);assert(sends==before+1);
 d->sock_fd4=-1;mdnsd_stop(d);mdnsd_stop(d);mdnsd_destroy(d);
 puts("Discovery: 15s TTL on every record, 5s renewal, zero-TTL goodbye and idempotent stop passed");return 0;
}
