/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <orbis/NetCtl.h>
/* Reproduce the mixed ABI header in OpenOrbis 0.5.4. */
#undef IP_MULTICAST_IF
#undef IP_MULTICAST_TTL
#undef IP_MULTICAST_LOOP
#undef IP_ADD_MEMBERSHIP
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
static int fail_bind, fail_join, joins, closed;
int sceNetCtlGetInfo(int code,OrbisNetCtlInfo *out){(void)code;(void)out;return -1;}
static int test_socket(int family,int type,int proto){assert(family==AF_INET);assert(type==SOCK_DGRAM);assert(proto==17);return 42;}
static int test_bind(int fd,const struct sockaddr *addr,socklen_t size){assert(fd==42);assert(size==sizeof(struct sockaddr_in));assert(ntohs(((const struct sockaddr_in *)addr)->sin_port)==5353);if(fail_bind){errno=EACCES;return -1;}return 0;}
static int test_close(int fd){assert(fd==42);++closed;return 0;}
static int test_setsockopt(int fd,int level,int option,const void *value,socklen_t size){
 assert(fd==42);(void)value;(void)size;
 if(level==IPPROTO_IP){
  if(option==12){++joins;if(fail_join){errno=EACCES;return -1;}}
  else assert(option==9 || option==10 || option==11);
 }
 return 0;
}
#define socket test_socket
#define bind test_bind
#define close test_close
#define setsockopt test_setsockopt
#include MDNS_SOURCE
int main(void){
 assert(mdns_open_socket4(htonl(0xc0a8003a))==42);assert(joins==1);assert(closed==0);
 fail_bind=1;assert(mdns_open_socket4(0)==-EACCES);assert(strcmp(mdnsd_last_stage(),"IPv4 bind 5353")==0);
 fail_bind=0;fail_join=1;joins=0;
 assert(mdns_open_socket4(htonl(0xc0a8003a))==-EACCES);assert(joins==2);
 assert(strcmp(mdnsd_last_stage(),"IPv4 multicast join")==0);
 puts("Orbis mDNS option and failure-path regression tests passed");return 0;
}
