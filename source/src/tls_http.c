/* SPDX-License-Identifier: GPL-3.0-or-later
 * HTTPS/1.1 transport with verified OpenSSL TLS for elevated Orbis processes.
 * No Sony TLS, certificate bypass, proxy, cookies or credential persistence.
 */
#include "tls_http.h"
#include "../generated/ca_bundle.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#ifdef LAB_PS4
#include <orbis/Net.h>
#include <orbis/libkernel.h>
#endif
#define TLS_TIMEOUT 5000
#define URL_CAP 8193
struct TlsHttp {int fd;SSL *ssl;uint64_t total,remaining,chunk_remaining;int chunked,chunk_crlf,eof;uint64_t header_deadline;unsigned char buffer[8192];size_t pos,end;};
static pthread_mutex_t context_mutex=PTHREAD_MUTEX_INITIALIZER;
static SSL_CTX *context;
static BIO_METHOD *socket_method;
static int net_pool=-1;
static int (*cancel_hook)(void *);static void *cancel_opaque;
void tls_http_interrupt(int (*hook)(void *),void *opaque){cancel_hook=hook;cancel_opaque=opaque;}
static int cancelled(void){return cancel_hook && cancel_hook(cancel_opaque);}
#ifndef LAB_PS4
static char *test_ca;static size_t test_ca_length;
#endif
void tls_http_net_pool(int pool){net_pool=pool;}
static uint64_t milliseconds(void){
#ifdef LAB_PS4
 return sceKernelGetProcessTime()/1000;
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;
#endif
}
static void phase(TlsDiagnostics *d,const char *s){snprintf(d->phase,sizeof(d->phase),"%s",s);}
static int wait_socket(int fd,int write,uint64_t deadline){
 for(;;){if(cancelled())return -ECANCELED;uint64_t now=milliseconds();if(now>=deadline)return -ETIMEDOUT;uint64_t remaining=deadline-now;if(remaining>100)remaining=100;struct timeval t={(time_t)(remaining/1000),(suseconds_t)((remaining%1000)*1000)};fd_set set;FD_ZERO(&set);FD_SET(fd,&set);int n=select(fd+1,write?NULL:&set,write?&set:NULL,NULL,&t);if(n>0)return 0;if(!n)continue;if(errno!=EINTR)return -errno;}
}
static int bio_create(BIO *b){BIO_set_init(b,1);BIO_set_data(b,NULL);return 1;}
static int bio_destroy(BIO *b){(void)b;return 1;}
static int bio_read(BIO *b,char *out,int n){int fd=(int)(intptr_t)BIO_get_data(b);BIO_clear_retry_flags(b);int rc=(int)recv(fd,out,(size_t)n,0);if(rc<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))BIO_set_retry_read(b);return rc;}
static int bio_write(BIO *b,const char *data,int n){int fd=(int)(intptr_t)BIO_get_data(b);BIO_clear_retry_flags(b);int rc=(int)send(fd,data,(size_t)n,0);if(rc<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))BIO_set_retry_write(b);return rc;}
static long bio_ctrl(BIO *b,int command,long n,void *p){(void)b;(void)n;(void)p;return command==BIO_CTRL_FLUSH?1:0;}
static int add_roots(SSL_CTX *ctx,const char *pem,size_t length){
 BIO *bio=BIO_new_mem_buf(pem,(int)length);if(!bio)return -1;X509 *cert;int count=0;
 while((cert=PEM_read_bio_X509(bio,NULL,NULL,NULL))){if(X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx),cert)==1)++count;X509_free(cert);ERR_clear_error();}
 BIO_free(bio);ERR_clear_error();return count>0?0:-1;
}
static SSL *new_ssl(void){
 pthread_mutex_lock(&context_mutex);
 if(!socket_method){socket_method=BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"Orbis socket");if(socket_method){BIO_meth_set_create(socket_method,bio_create);BIO_meth_set_destroy(socket_method,bio_destroy);BIO_meth_set_read(socket_method,bio_read);BIO_meth_set_write(socket_method,bio_write);BIO_meth_set_ctrl(socket_method,bio_ctrl);}}
 if(!context){
  context=SSL_CTX_new(TLS_client_method());
  if(context){SSL_CTX_set_verify(context,SSL_VERIFY_PEER,NULL);SSL_CTX_set_min_proto_version(context,TLS1_2_VERSION);SSL_CTX_set_options(context,SSL_OP_NO_COMPRESSION);
   const char *pem=media_ca_bundle;size_t length=sizeof(media_ca_bundle)-1;
#ifndef LAB_PS4
   if(test_ca){pem=test_ca;length=test_ca_length;}
#endif
   if(add_roots(context,pem,length)){SSL_CTX_free(context);context=NULL;}
  }
 }
 SSL *ssl=context && socket_method?SSL_new(context):NULL;pthread_mutex_unlock(&context_mutex);return ssl;
}
void tls_http_shutdown(void){pthread_mutex_lock(&context_mutex);SSL_CTX_free(context);context=NULL;pthread_mutex_unlock(&context_mutex);}
#ifndef LAB_PS4
int tls_http_test_ca(const char *pem,size_t length){tls_http_shutdown();pthread_mutex_lock(&context_mutex);free(test_ca);test_ca=NULL;test_ca_length=0;if(pem && length){test_ca=malloc(length);if(test_ca){memcpy(test_ca,pem,length);test_ca_length=length;}}pthread_mutex_unlock(&context_mutex);return pem && !test_ca?-1:0;}
#endif
static int url_parts(const char *url,char host[256],unsigned *port,char path[URL_CAP]){
 if(strncmp(url,"https://",8))return -EINVAL;const char *p=url+8,*end=strpbrk(p,"/?#");if(!end)end=p+strlen(p);size_t n=(size_t)(end-p);if(!n || n>300 || memchr(p,'@',n) || memchr(p,'[',n))return -EINVAL;
 const char *colon=memchr(p,':',n);size_t hn=colon?(size_t)(colon-p):n;if(!hn || hn>=256)return -EINVAL;
 for(size_t i=0;i<hn;++i)if(!isalnum((unsigned char)p[i]) && p[i]!='-' && p[i]!='.' && p[i]!='_')return -EINVAL;
 memcpy(host,p,hn);host[hn]=0;*port=443;
 if(colon){unsigned long number=0;const char *q=colon+1;if(q==end)return -EINVAL;for(;q<end;++q){if(!isdigit((unsigned char)*q))return -EINVAL;number=number*10+(unsigned)(*q-'0');if(number>65535)return -EINVAL;}if(!number)return -EINVAL;*port=(unsigned)number;}
 if(*end=='/')snprintf(path,URL_CAP,"%s",end);else snprintf(path,URL_CAP,"/%s",*end=='?'?end:"");char *fragment=strchr(path,'#');if(fragment)*fragment=0;
 for(const char *q=path;*q;++q)if((unsigned char)*q<=32 || (unsigned char)*q==127)return -EINVAL;return 0;
}
void media_url_origin(const char *url,char *out,size_t capacity){
 if(!capacity)return;out[0]=0;const char *p=strstr(url,"://");if(!p)return;size_t n=(size_t)(p-url);if(n>5)return;
 for(size_t i=0;i<n;++i)if(!isalpha((unsigned char)url[i]))return;
 p+=3;const char *end=strpbrk(p,"/?#");if(!end)end=p+strlen(p);const char *at=memchr(p,'@',(size_t)(end-p));if(at)p=at+1;
 size_t hn=(size_t)(end-p);if(hn>150)return;for(size_t i=0;i<hn;++i)if(!isalnum((unsigned char)p[i]) && p[i]!='.' && p[i]!='-' && p[i]!='_' && p[i]!=':' && p[i]!='[' && p[i]!=']')return;
 snprintf(out,capacity,"%.*s://%.*s",(int)n,url,(int)hn,p);
}
static int resolve_ipv4(const char *host,struct in_addr *address){
 if(inet_pton(AF_INET,host,address)==1)return 0;
#ifdef LAB_PS4
 if(net_pool<0)return -ENETDOWN;int resolver=sceNetResolverCreate("airplay-tls",net_pool,0);if(resolver<0)return resolver;OrbisNetInAddr result={0};int rc=sceNetResolverStartNtoa(resolver,host,&result,5000000,1,0);sceNetResolverDestroy(resolver);if(rc<0)return rc;memcpy(address,&result,sizeof(*address));return 0;
#else
 struct addrinfo hints={0},*result=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;int rc=getaddrinfo(host,NULL,&hints,&result);if(rc || !result)return -EHOSTUNREACH;*address=((struct sockaddr_in *)result->ai_addr)->sin_addr;freeaddrinfo(result);return 0;
#endif
}
static int connect_tls(TlsHttp *h,const char *host,unsigned port,TlsDiagnostics *d){
 struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);phase(d,"tls-dns");int rc=resolve_ipv4(host,&address.sin_addr);if(rc<0)return rc;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(LAB_PS4)
 address.sin_len=sizeof(address);
#endif
 phase(d,"tls-tcp");h->fd=socket(AF_INET,SOCK_STREAM,0);if(h->fd<0)return -errno;
#ifdef LAB_PS4
 int on=1;rc=setsockopt(h->fd,SOL_SOCKET,0x1200,&on,sizeof(on));
#else
 rc=fcntl(h->fd,F_SETFL,fcntl(h->fd,F_GETFL,0)|O_NONBLOCK);
#endif
 if(rc<0)return -errno;
 uint64_t deadline=milliseconds()+TLS_TIMEOUT;rc=connect(h->fd,(struct sockaddr *)&address,sizeof(address));
 if(rc<0){if(errno!=EINPROGRESS && errno!=EWOULDBLOCK)return -errno;if((rc=wait_socket(h->fd,1,deadline))<0)return rc;int error=0;socklen_t size=sizeof(error);if(getsockopt(h->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)return -errno;if(error)return error>0?-error:error;}
 phase(d,"tls-init");h->ssl=new_ssl();if(!h->ssl)return -ENOMEM;BIO *bio=BIO_new(socket_method);if(!bio)return -ENOMEM;BIO_set_data(bio,(void *)(intptr_t)h->fd);SSL_set_bio(h->ssl,bio,bio);
 struct in_addr numeric;if(inet_pton(AF_INET,host,&numeric)==1){if(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(h->ssl),host)!=1)return -EINVAL;}
 else{if(SSL_set1_host(h->ssl,host)!=1 || SSL_set_tlsext_host_name(h->ssl,host)!=1)return -EINVAL;}
 static const unsigned char alpn[]={8,'h','t','t','p','/','1','.','1'};SSL_set_alpn_protos(h->ssl,alpn,sizeof(alpn));
 phase(d,"tls-handshake");deadline=milliseconds()+TLS_TIMEOUT;
 for(;;){ERR_clear_error();rc=SSL_connect(h->ssl);if(rc==1)break;int error=SSL_get_error(h->ssl,rc);d->verify=SSL_get_verify_result(h->ssl);d->ssl_error=ERR_peek_last_error();if(error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE){if(d->verify)phase(d,"tls-verify");return -EIO;}if((rc=wait_socket(h->fd,error==SSL_ERROR_WANT_WRITE,deadline))<0)return rc;}
 d->verify=SSL_get_verify_result(h->ssl);if(d->verify!=X509_V_OK){phase(d,"tls-verify");return -EIO;}return 0;
}
static int ssl_io(TlsHttp *h,void *buffer,int length,int writing,TlsDiagnostics *d){
 uint64_t deadline=milliseconds()+TLS_TIMEOUT;if(h->header_deadline && h->header_deadline<deadline)deadline=h->header_deadline;
 for(;;){if(cancelled())return -ECANCELED;if(milliseconds()>=deadline)return -ETIMEDOUT;ERR_clear_error();int n=writing?SSL_write(h->ssl,buffer,length):SSL_read(h->ssl,buffer,length);if(n>0)return n;int error=SSL_get_error(h->ssl,n);if(!writing && error==SSL_ERROR_ZERO_RETURN)return 0;if(error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE){d->ssl_error=ERR_peek_last_error();return -EIO;}int rc=wait_socket(h->fd,error==SSL_ERROR_WANT_WRITE,deadline);if(rc<0)return rc;}
}
static int buffered_read(TlsHttp *h,void *out,size_t length,TlsDiagnostics *d){
 if(cancelled())return -ECANCELED;if(!length)return 0;if(h->pos==h->end){int n=ssl_io(h,h->buffer,sizeof(h->buffer),0,d);if(n<=0)return n;h->pos=0;h->end=(size_t)n;}
 size_t n=h->end-h->pos;if(n>length)n=length;memcpy(out,h->buffer+h->pos,n);h->pos+=n;return (int)n;
}
static int line(TlsHttp *h,char *out,size_t capacity,TlsDiagnostics *d){
 size_t used=0;while(used+1<capacity){char c;int n=buffered_read(h,&c,1,d);if(n<=0)return n<0?n:-EIO;if(c=='\n'){if(!used || out[used-1]!='\r')return -EIO;out[used-1]=0;return (int)used-1;}if(!c)return -EIO;out[used++]=c;}return -EOVERFLOW;
}
static int decimal(const char *text,uint64_t *out){if(!isdigit((unsigned char)*text))return -EINVAL;uint64_t n=0;for(;*text;++text){if(!isdigit((unsigned char)*text) || n>(UINT64_MAX-9)/10)return -EINVAL;n=n*10+(unsigned)(*text-'0');}*out=n;return 0;}
void tls_http_close(TlsHttp *h){if(h){SSL_free(h->ssl);if(h->fd>=0)close(h->fd);memset(h,0,sizeof(*h));free(h);}}
uint64_t tls_http_size(TlsHttp *h){return h->total;}
int tls_http_open(TlsHttp **out,const char *url,uint64_t offset,TlsDiagnostics *d){
 char current[URL_CAP];if(cancelled())return -ECANCELED;if(!url || strnlen(url,URL_CAP)>=URL_CAP)return -EINVAL;snprintf(current,sizeof(current),"%s",url);*out=NULL;memset(d,0,sizeof(*d));
 for(int redirect=0;redirect<6;++redirect){
  char host[256],path[URL_CAP];unsigned port;media_url_origin(current,d->origin,sizeof(d->origin));phase(d,"tls-url");int rc=url_parts(current,host,&port,path);if(rc<0)return rc;
  TlsHttp *h=calloc(1,sizeof(*h));if(!h)return -ENOMEM;h->fd=-1;h->total=h->remaining=UINT64_MAX;
  rc=connect_tls(h,host,port,d);if(rc<0){tls_http_close(h);return rc;}
  char request[URL_CAP+512];int n=snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: PS4AirPlay/1.10\r\nAccept: */*\r\nAccept-Encoding: identity\r\nRange: bytes=%llu-\r\nConnection: close\r\n\r\n",path,host,port,(unsigned long long)offset);if(n<0 || (size_t)n>=sizeof(request)){tls_http_close(h);return -EOVERFLOW;}
  h->header_deadline=milliseconds()+TLS_TIMEOUT;phase(d,"tls-send");size_t sent=0;while(sent<(size_t)n){rc=ssl_io(h,request+sent,n-(int)sent,1,d);if(rc<=0){tls_http_close(h);return rc?rc:-EIO;}sent+=(size_t)rc;}
  int interim=0;
read_headers:
  phase(d,"tls-headers");char text[8192],location[URL_CAP]="";rc=line(h,text,sizeof(text),d);unsigned status=0;char version[16],extra[2];if(rc<0 || sscanf(text,"%15s %u %1s",version,&status,extra)<2 || status<100 || status>599 || (strcmp(version,"HTTP/1.1") && strcmp(version,"HTTP/1.0"))){tls_http_close(h);return rc<0?rc:-EIO;}d->status=(int)status;
  size_t header_bytes=0;int has_length=0,has_range=0;uint64_t content_length=0,range_start=0,range_end=0,range_size=0;
  for(;;){rc=line(h,text,sizeof(text),d);if(rc<0 || (header_bytes+=(size_t)rc+2)>65536){tls_http_close(h);return rc<0?rc:-EOVERFLOW;}if(!rc)break;char *colon=strchr(text,':');if(!colon){tls_http_close(h);return -EIO;}*colon++=0;while(*colon==' ' || *colon=='\t')++colon;char *last=colon+strlen(colon);while(last>colon && (last[-1]==' ' || last[-1]=='\t'))*--last=0;
   if(!strcasecmp(text,"Content-Length")){uint64_t value;if(decimal(colon,&value)<0 || (has_length && value!=content_length)){tls_http_close(h);return -EIO;}content_length=value;has_length=1;}
   else if(!strcasecmp(text,"Transfer-Encoding")){if(strcasecmp(colon,"chunked")){tls_http_close(h);return -EPROTONOSUPPORT;}h->chunked=1;}
   else if(!strcasecmp(text,"Content-Range")){unsigned long long a,b,c;char tail[2];if(sscanf(colon,"bytes %llu-%llu/%llu%1s",&a,&b,&c,tail)!=3 || b<a || c<=b){tls_http_close(h);return -EIO;}range_start=a;range_end=b;range_size=c;has_range=1;}
   else if(!strcasecmp(text,"Location")){if(strlen(colon)>=sizeof(location)){tls_http_close(h);return -EOVERFLOW;}snprintf(location,sizeof(location),"%s",colon);}
  }
  if(status>=100 && status<200){if(status==101 || ++interim>8){tls_http_close(h);return -EIO;}h->chunked=0;goto read_headers;}
  if(status==301 || status==302 || status==303 || status==307 || status==308){
   if(!strncmp(location,"https://",8))snprintf(current,sizeof(current),"%s",location);
   else if(location[0]=='/' && location[1]!='/'){int count=snprintf(current,sizeof(current),"https://%s:%u%s",host,port,location);if(count<0 || (size_t)count>=sizeof(current)){tls_http_close(h);return -EOVERFLOW;}}
   else{tls_http_close(h);return -EPROTONOSUPPORT;}tls_http_close(h);continue;
  }
  phase(d,"tls-range");
  if(h->chunked && has_length){tls_http_close(h);return -EIO;}
  if(status==206){if(!has_range || range_start!=offset || (has_length && content_length!=range_end-range_start+1)){tls_http_close(h);return -EIO;}h->total=range_size;h->remaining=range_end-range_start+1;}
  else if(status==200 && !offset){if(has_length)h->total=h->remaining=content_length;}
  else{tls_http_close(h);return -EIO;}
  h->header_deadline=0;phase(d,"tls-read");*out=h;return 0;
 }
 phase(d,"tls-redirect-limit");return -ELOOP;
}
int tls_http_read(TlsHttp *h,void *out,uint32_t length,TlsDiagnostics *d){
 phase(d,"tls-read");if(h->eof || !length || !h->remaining)return 0;
 if(h->chunked){
  if(!h->chunk_remaining){char text[8192];int rc;
   if(h->chunk_crlf){rc=line(h,text,sizeof(text),d);if(rc!=0)return rc<0?rc:-EIO;h->chunk_crlf=0;}
   rc=line(h,text,sizeof(text),d);if(rc<=0)return rc<0?rc:-EIO;char *extension=strchr(text,';');if(extension)*extension=0;
   uint64_t size=0;for(char *p=text;*p;++p){unsigned c=(unsigned char)*p;unsigned value=isdigit(c)?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(value>=16 || size>(UINT64_MAX-15)/16)return -EIO;size=size*16+value;}
   if(!size){size_t trailer=0;do{rc=line(h,text,sizeof(text),d);if(rc<0)return rc;trailer+=(size_t)rc+2;if(trailer>65536)return -EOVERFLOW;}while(rc);h->eof=1;return 0;}h->chunk_remaining=size;
  }
  if(length>h->chunk_remaining)length=(uint32_t)h->chunk_remaining;
 }
 if(h->remaining!=UINT64_MAX && length>h->remaining)length=(uint32_t)h->remaining;
 int n=buffered_read(h,out,length,d);if(n<0)return n;if(!n){if(h->chunked || (h->remaining!=UINT64_MAX && h->remaining))return -EIO;h->eof=1;return 0;}
 if(h->remaining!=UINT64_MAX)h->remaining-=(unsigned)n;if(h->chunked){h->chunk_remaining-=(unsigned)n;if(!h->chunk_remaining)h->chunk_crlf=1;}return n;
}
