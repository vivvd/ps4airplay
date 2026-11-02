/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fp_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
static int decode(const char *hex,unsigned char *out){size_t n=strlen(hex);assert(!(n&1));for(size_t i=0;i<n/2;++i){unsigned x;assert(sscanf(hex+2*i,"%2x",&x)==1);out[i]=(unsigned char)x;}return (int)n/2;}
int main(int argc,char **argv){
 /* The pre-existing media key decryptor must retain its captured vector. */
 extern void playfair_decrypt(unsigned char *,unsigned char *,unsigned char *);
 unsigned char old_m3[164],old_key[72],old_plain[16],old_want[16];
 decode("46504c590301030000000098018f1a9c7d0af257b31f21f5c2d2bc814c032d457835ad0b06250574bbc7ab4a58cca6eead2c911d7f3e1e7ed4c058955dff3d5ceef014387a985bdb34995015e3dfbdacc56047cb926e093b13e9fdb5e1eee317c018bbc87fc5453c7671647da686da3d564875d03f8aea9d60092de06110bc7be0c16f391c369c75344ae47f33acfcf10e63a9b58bfce215e96001c49e4be967c5067f2a",old_m3);
 decode("46504c59010201000000003c0000000088e4f82c8178c18b4751ac24b27c0c2a00000010c899dc6965c1081de6a9d966e2ba3e34548cdbc651c322db18dc22f58fe154a60aecee18",old_key);
 decode("8e1214398d46d72e7b1b8e32f80c8bf0",old_want);playfair_decrypt(old_m3,old_key,old_plain);assert(!memcmp(old_plain,old_want,16));
 assert(argc==6);unsigned char m3[164],key[16],mac[20],envelope[120],material[64],want[64],a[16],b[20];
 assert(decode(argv[1],m3)==164);assert(decode(argv[2],a)==16);assert(decode(argv[3],b)==20);assert(decode(argv[4],envelope)==120);assert(decode(argv[5],want)==64);
 assert(!fp_control_session_keys(m3,key,mac));assert(!memcmp(key,a,16));assert(!memcmp(mac,b,20));assert(!fp_control_unwrap(m3,envelope,material));assert(!memcmp(material,want,64));
 for(int i=0;i<120;++i){envelope[i]^=1;assert(fp_control_unwrap(m3,envelope,material)<0);envelope[i]^=1;}
 m3[12]=255;assert(fp_control_session_keys(m3,key,mac)<0);
 /* Independent native CoreUtils synthetic GCM record (nonce is 16 bytes). */
 unsigned char nonce[16],plain[32],record[64],expected[64],opened[64];for(int i=0;i<16;++i){key[i]=(unsigned char)i;nonce[i]=(unsigned char)(i+16);}
 int pn=decode("474554202f706c61796261636b2d696e666f20485454502f312e310d0a0d0a",plain);
 int rn=decode("0000001fe957ece35d2501ac0984bdc2ecc8584c288d9061e2e9d0be6047b37d55f1efe3815cb904d78fe5c00c51781b52669d",expected);
 assert(fp_control_record(1,key,nonce,plain,pn,record)==rn);assert(!memcmp(record,expected,rn));
 for(int i=0;i<16;++i)nonce[i]=(unsigned char)(i+16);assert(fp_control_record(0,key,nonce,record,rn,opened)==pn);assert(!memcmp(plain,opened,pn));
 memset(nonce,255,sizeof(nonce));assert(fp_control_record(1,key,nonce,plain,pn,record)<0);
 /* TCP fragmentation never exposes unauthenticated plaintext. */
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));assert(!fcntl(fd[1],F_SETFL,O_NONBLOCK));for(int i=0;i<64;++i)material[i]=(unsigned char)i;
 fp_control_t *s=fp_control_new(material,NULL);assert(s);memset(opened,0xaa,sizeof(opened));int result=-1;
 for(int i=0;i<rn;++i){assert(write(fd[0],expected+i,1)==1);result=fp_control_read(s,fd[1],opened,sizeof(opened));if(i<rn-1){assert(result<0 && errno==EAGAIN);for(int j=0;j<pn;++j)assert(opened[j]==0xaa);}}
 assert(result==pn && !memcmp(opened,plain,pn));
 rn=decode("00000001790cabd39963b8e3a3311a533151b94023",expected);expected[rn-1]^=1;assert(write(fd[0],expected,rn)==rn);assert(fp_control_read(s,fd[1],opened,sizeof(opened))<0);assert(fp_control_read(s,fd[1],opened,sizeof(opened))<0 && errno==EIO);assert(fp_control_read(s,fd[1],opened,sizeof(opened))<0 && errno==EIO);
 fp_control_free(s);close(fd[0]);close(fd[1]);puts("Control KDF/envelope vectors, all-byte tampering, independent GCM, nonce exhaustion, fragmented authenticated reads and permanent failure passed");return 0;
}
