/* GOPHPER: shadows wasi-libc's sockaddr_un, which lacks sun_path on wasip1.
 * Unix sockets still do not work; this only lets the code compile. */
#ifndef __wasilibc___struct_sockaddr_un_h
#define __wasilibc___struct_sockaddr_un_h

#include <__typedef_sa_family_t.h>

struct sockaddr_un {
  __attribute__((aligned(__BIGGEST_ALIGNMENT__))) sa_family_t sun_family;
  char sun_path[108];
};

#endif
