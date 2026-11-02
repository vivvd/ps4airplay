/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native legacy AirPlay control records. Protocol research reference:
 * https://github.com/jkiddo/go-atv/tree/7eb98df84d4d0d17c0c037dc1134edeca11cd9bc/internal/fpcontrol
 * Uses this project's existing PlayFair primitives and independently expressed
 * MD5 schedule/KDF, authenticated envelope and OpenSSL record implementation.
 */
#include "fp_control.h"
#include "compat.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
extern void decryptMessage(unsigned char *,unsigned char *);
extern void sap_hash(unsigned char *,unsigned char *);
extern unsigned char default_sap[],static_source_1[],static_source_2[],initial_session_key[];
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[3]<<24|(uint32_t)p[2]<<16|(uint32_t)p[1]<<8|p[0];}
static void put32(unsigned char *p,uint32_t n){p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n;}
static uint32_t rotate(uint32_t x,unsigned n){return (x<<n)|(x>>(32-n));}
/* MD5 compression with the two protocol-specific schedule mutations. */
static void compress(uint32_t state[4],const unsigned char block[64],int cycle){
 static const unsigned shifts[4][4]={{7,12,17,22},{5,9,14,20},{4,11,16,23},{6,10,15,21}};
 uint32_t words[16],a=state[0],b=state[1],c=state[2],d=state[3];
 for(int i=0;i<16;++i)words[i]=be32(block+4*i);
 for(unsigned i=0;i<64;++i){
  uint32_t f;unsigned j;
  if(i<16){f=(b&c)|(~b&d);j=i;}else if(i<32){f=(d&b)|(~d&c);j=(5*i+1)&15;}
  else if(i<48){f=b^c^d;j=(3*i+5)&15;}else{f=c^(b|~d);j=(7*i)&15;}
  uint32_t constant=(uint32_t)(fabs(sin((double)i+1))*4294967296.0),old=d;
  d=c;c=b;b+=rotate(a+f+constant+words[j],shifts[i/16][i&3]);a=old;
  if(i==31){
   if(cycle){unsigned indexes[8]={a&15,b&15,c&15,d&15,(a>>4)&15,(b>>4)&15,(c>>4)&15,(d>>4)&15};uint32_t first=words[indexes[0]];for(int k=0;k<7;++k)words[indexes[k]]=words[indexes[k+1]];words[indexes[7]]=first;}
   else{unsigned pairs[10]={a&15,b&15,c&15,d&15,(a>>4)&15,(b>>4)&15,(a>>8)&15,(b>>8)&15,(a>>12)&15,(b>>12)&15};for(int k=0;k<10;k+=2){uint32_t x=words[pairs[k]];words[pairs[k]]=words[pairs[k+1]];words[pairs[k+1]]=x;}}
  }
 }
 state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
 OPENSSL_cleanse(words,sizeof(words));
}
static void derive(const unsigned char sender[128],unsigned char key[16],unsigned char mac[20]){
 static const unsigned char prefix[17]={0xa0,0x44,0x9c,0x4d,0x09,0xe4,0xbd,0x7f,0x6e,0xc5,0xd0,0xcc,0x35,0x9d,0xa7,0x46,0x7a};
 static const unsigned char suffix[17]={0x97,0xb5,0x0f,0x84,0xe2,0x15,0x5a,0x9c,0x24,0x99,0x1c,0xf4,0x3a,0x09,0x63,0x55,0x47};
 unsigned char padded[320]={0},hash[16];uint32_t state[4],modified[4];
 for(int descriptor=0;descriptor<2;++descriptor){
  memset(padded,0,sizeof(padded));memcpy(padded,descriptor?prefix:static_source_1,17);memcpy(padded+17,sender,128);memcpy(padded+145,default_sap+128,128);memcpy(padded+273,descriptor?suffix:static_source_2,17);
  padded[290]=0x80;padded[312]=0x10;padded[313]=0x09; /* 290 * 8, little endian */
  for(int j=0;j<4;++j)state[j]=le32(initial_session_key+4*j);
  for(int i=0;i<5;++i){
   unsigned char *block=padded+64*i;sap_hash(block,hash);
   if(!descriptor){memcpy(modified,state,sizeof(state));compress(modified,block,0);for(int j=0;j<4;++j)state[j]=modified[j]+le32(hash+4*j);}
   else{for(int j=0;j<4;++j)state[j]+=le32(hash+4*j);compress(state,block,1);if(i==4){put32(mac,state[0]);compress(state,block,1);}}
  }
  for(int j=0;j<4;++j)put32((descriptor?mac+4:key)+4*j,state[j]);
 }
 OPENSSL_cleanse(padded,sizeof(padded));OPENSSL_cleanse(hash,sizeof(hash));OPENSSL_cleanse(state,sizeof(state));OPENSSL_cleanse(modified,sizeof(modified));
}
int fp_control_session_keys(const unsigned char m3[164],unsigned char key[16],unsigned char mac[20]){
 if(memcmp(m3,"FPLY",4)||m3[4]!=3||m3[12]>3)return -1;
 unsigned char sender[128];decryptMessage((unsigned char *)m3,sender);derive(sender,key,mac);OPENSSL_cleanse(sender,sizeof(sender));return 0;
}
int fp_control_unwrap(const unsigned char m3[164],const unsigned char envelope[120],unsigned char material[64]){
 static const unsigned char header[16]={'F','P','L','Y',1,2,1,0,0,0,0,108,0,0,0,0};
 unsigned char sender[128],key[16],mac[20],plain[64]={0},auth[100],tag[EVP_MAX_MD_SIZE];unsigned tag_len=0;int n=0,last=0,ok=0;
 if(memcmp(m3,"FPLY",4)||m3[4]!=3||m3[12]>3||memcmp(envelope,header,16)||be32(envelope+32)!=64)return -1;
 decryptMessage((unsigned char *)m3,sender);derive(sender,key,mac);
 EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
 if(ctx && EVP_DecryptInit_ex(ctx,EVP_aes_128_cbc(),NULL,key,envelope+16)==1 && EVP_CIPHER_CTX_set_padding(ctx,0)==1 && EVP_DecryptUpdate(ctx,plain,&n,envelope+56,64)==1 && EVP_DecryptFinal_ex(ctx,plain+n,&last)==1 && n+last==64){
  memcpy(auth,envelope,36);memcpy(auth+36,plain,64);
  if(HMAC(EVP_sha1(),mac,20,auth,sizeof(auth),tag,&tag_len) && tag_len==20 && !CRYPTO_memcmp(tag,envelope+36,20)){memcpy(material,plain,64);ok=1;}
 }
 EVP_CIPHER_CTX_free(ctx);OPENSSL_cleanse(sender,sizeof(sender));OPENSSL_cleanse(key,sizeof(key));OPENSSL_cleanse(mac,sizeof(mac));OPENSSL_cleanse(plain,sizeof(plain));OPENSSL_cleanse(auth,sizeof(auth));OPENSSL_cleanse(tag,sizeof(tag));return ok?0:-1;
}
/* Nonce increments before each record. Header length is authenticated as AAD. */
int fp_control_record(int encrypt,const unsigned char key[16],unsigned char nonce[16],const unsigned char *input,size_t size,unsigned char *output){
 if((encrypt && size>FP_CONTROL_MAX)||(!encrypt && (size<20 || be32(input)>FP_CONTROL_MAX || size!=(size_t)be32(input)+20)))return -1;
 int carry=1;for(int i=15;i>=0 && carry;--i)carry=++nonce[i]==0;if(carry)return -1;
 size_t length=encrypt?size:size-20;int n=0,last=0,unused=0,ok=0;EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return -1;
 if(encrypt){put32(output,(uint32_t)length);ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_gcm(),NULL,NULL,NULL)==1 && EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,16,NULL)==1 && EVP_EncryptInit_ex(ctx,NULL,NULL,key,nonce)==1 && EVP_EncryptUpdate(ctx,NULL,&unused,output,4)==1 && EVP_EncryptUpdate(ctx,output+4,&n,input,(int)length)==1 && EVP_EncryptFinal_ex(ctx,output+4+n,&last)==1 && EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,16,output+4+length)==1;}
 else{ok=EVP_DecryptInit_ex(ctx,EVP_aes_128_gcm(),NULL,NULL,NULL)==1 && EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,16,NULL)==1 && EVP_DecryptInit_ex(ctx,NULL,NULL,key,nonce)==1 && EVP_DecryptUpdate(ctx,NULL,&unused,input,4)==1 && EVP_DecryptUpdate(ctx,output,&n,input+4,(int)length)==1 && EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,16,(void *)(input+4+length))==1 && EVP_DecryptFinal_ex(ctx,output+n,&last)==1;}
 EVP_CIPHER_CTX_free(ctx);if(!ok){if(!encrypt)OPENSSL_cleanse(output,length);return -1;}return (int)(encrypt?length+20:length);
}
struct fp_control_s {unsigned char read_key[16],read_nonce[16],write_key[16],write_nonce[16];unsigned char *input;size_t have,want;int failed;};
fp_control_t *fp_control_new(const unsigned char material[64],const unsigned char *shared_secret){
 fp_control_t *s=calloc(1,sizeof(*s));if(!s)return NULL;s->input=malloc(FP_CONTROL_MAX+20);if(!s->input){free(s);return NULL;}
 memcpy(s->read_key,material,16);memcpy(s->read_nonce,material+16,16);memcpy(s->write_key,material+32,16);memcpy(s->write_nonce,material+48,16);s->want=4;
 if(shared_secret){unsigned char seed[48],digest[SHA512_DIGEST_LENGTH];memcpy(seed+16,shared_secret,32);memcpy(seed,s->read_key,16);SHA512(seed,sizeof(seed),digest);memcpy(s->read_key,digest,16);memcpy(seed,s->write_key,16);SHA512(seed,sizeof(seed),digest);memcpy(s->write_key,digest,16);OPENSSL_cleanse(seed,sizeof(seed));OPENSSL_cleanse(digest,sizeof(digest));}return s;
}
void fp_control_free(fp_control_t *s){if(s){OPENSSL_cleanse(s->input,FP_CONTROL_MAX+20);free(s->input);OPENSSL_cleanse(s,sizeof(*s));free(s);}}
int fp_control_read(fp_control_t *s,int fd,void *out,size_t capacity){
 if(s->failed){errno=EIO;return -1;}
 int n=(int)recv(fd,s->input+s->have,s->want-s->have,0);if(n<=0)return n;s->have+=(size_t)n;
 if(s->have==4 && s->want==4){size_t size=be32(s->input);if(size>FP_CONTROL_MAX || size>capacity){s->failed=1;errno=EMSGSIZE;return -1;}s->want=size+20;}
 if(s->have<s->want){errno=EAGAIN;return -1;}
 int result=fp_control_record(0,s->read_key,s->read_nonce,s->input,s->want,out);s->have=0;s->want=4;
 if(result<0){s->failed=1;errno=EIO;return -1;}if(!result){errno=EAGAIN;return -1;}return result;
}
int fp_control_write(fp_control_t *s,int fd,const void *data,size_t length){
 if(s->failed)return -1;size_t capacity=(length>FP_CONTROL_MAX?FP_CONTROL_MAX:length)+20;unsigned char *record=malloc(capacity);if(!record)return -1;size_t done=0;
 while(done<length){size_t size=length-done;if(size>FP_CONTROL_MAX)size=FP_CONTROL_MAX;int n=fp_control_record(1,s->write_key,s->write_nonce,(const unsigned char *)data+done,size,record);if(n<0)break;size_t sent=0;while(sent<(size_t)n){int rc=(int)send(fd,record+sent,(size_t)n-sent,0);if(rc<0 && errno==EINTR)continue;if(rc<=0)break;sent+=(size_t)rc;}if(sent!=(size_t)n)break;done+=size;}
 OPENSSL_cleanse(record,capacity);free(record);if(done!=length){s->failed=1;return -1;}return (int)length;
}
