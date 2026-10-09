/* GOPHPER: wasi-libc has no resolver library. compat/gophper_dns.c sends
 * res_search(3)'s queries through the host. */
#ifndef _RESOLV_H
#define _RESOLV_H

#include <arpa/nameser.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

int res_init(void);
int res_search(const char *, int, int, unsigned char *, int);
int res_query(const char *, int, int, unsigned char *, int);
int dn_expand(const unsigned char *, const unsigned char *, const unsigned char *, char *, int);
int dn_skipname(const unsigned char *, const unsigned char *);

#ifdef __cplusplus
}
#endif

#endif
