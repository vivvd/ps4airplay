/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FP_CONTROL_H
#define FP_CONTROL_H
#include <stddef.h>
#include <stdint.h>
#define FP_CONTROL_MAX 0x40000
/* Control transport only; no media/DRM content decoding. */
typedef struct fp_control_s fp_control_t;
int fp_control_session_keys(const unsigned char m3[164],unsigned char key[16],unsigned char mac[20]);
int fp_control_unwrap(const unsigned char m3[164],const unsigned char envelope[120],unsigned char material[64]);
fp_control_t *fp_control_new(const unsigned char material[64],const unsigned char *shared_secret);
void fp_control_free(fp_control_t *state);
int fp_control_read(fp_control_t *state,int fd,void *out,size_t capacity);
int fp_control_write(fp_control_t *state,int fd,const void *data,size_t length);
/* Stateless primitive exposed for independent wire-vector tests. */
int fp_control_record(int encrypt,const unsigned char key[16],unsigned char nonce[16],const unsigned char *input,size_t size,unsigned char *output);
#endif
