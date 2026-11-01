/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
int __real_bind(int, const struct sockaddr *, socklen_t);
int __real_connect(int, const struct sockaddr *, socklen_t);
ssize_t __real_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
static int native_address(struct sockaddr_storage *out,const struct sockaddr *in,socklen_t size) {
    if(!in || size>sizeof(*out) || size<2){errno=EINVAL;return -1;}
    memset(out,0,sizeof(*out));memcpy(out,in,size);
    if(in->sa_family==AF_INET)((struct sockaddr_in *)out)->sin_len=sizeof(struct sockaddr_in);
    if(in->sa_family==AF_INET6)((struct sockaddr_in6 *)out)->sin6_len=sizeof(struct sockaddr_in6);
    return 0;
}
int __wrap_bind(int fd,const struct sockaddr *addr,socklen_t size) {
    struct sockaddr_storage native;
    return native_address(&native,addr,size)<0?-1:__real_bind(fd,(struct sockaddr *)&native,size);
}
int __wrap_connect(int fd,const struct sockaddr *addr,socklen_t size) {
    struct sockaddr_storage native;
    return native_address(&native,addr,size)<0?-1:__real_connect(fd,(struct sockaddr *)&native,size);
}
ssize_t __wrap_sendto(int fd,const void *buf,size_t n,int flags,const struct sockaddr *addr,socklen_t size) {
    if(!addr)return __real_sendto(fd,buf,n,flags,addr,size);
    struct sockaddr_storage native;
    return native_address(&native,addr,size)<0?-1:__real_sendto(fd,buf,n,flags,(struct sockaddr *)&native,size);
}
