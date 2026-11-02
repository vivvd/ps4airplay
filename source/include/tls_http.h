/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct TlsHttp TlsHttp;
typedef struct {char phase[32],origin[192];long verify;unsigned long ssl_error;int status;} TlsDiagnostics;
void tls_http_net_pool(int pool);
/* Set only before starting IO workers; clear after joining all workers. */
void tls_http_interrupt(int (*cancelled)(void *),void *opaque);
int tls_http_open(TlsHttp **out,const char *url,uint64_t offset,TlsDiagnostics *diagnostics);
int tls_http_read(TlsHttp *,void *,uint32_t length,TlsDiagnostics *);
uint64_t tls_http_size(TlsHttp *);
void tls_http_close(TlsHttp *);
void tls_http_shutdown(void);
void media_url_origin(const char *url,char *out,size_t capacity);
#ifndef LAB_PS4
int tls_http_test_ca(const char *pem,size_t length);
#endif
