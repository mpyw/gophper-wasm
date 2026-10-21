/* GOPHPER: wasi-libc's <sys/socket.h> leaves out socket(), connect() and the
 * rest on wasip1. compat/gophper_net.c implements them through the host.
 * Declaring them here, and not only in the force-included gophper_compat.h,
 * lets a library's configure find them, as it would on Linux. */
#ifndef _GOPHPER_SYS_SOCKET_H
#define _GOPHPER_SYS_SOCKET_H

#include_next <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int socket(int, int, int);
int socketpair(int, int, int, int[2]);
int connect(int, const struct sockaddr *, socklen_t);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int getsockname(int, struct sockaddr *__restrict, socklen_t *__restrict);
int getpeername(int, struct sockaddr *__restrict, socklen_t *__restrict);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom(int, void *__restrict, size_t, int, struct sockaddr *__restrict, socklen_t *__restrict);
int getsockopt(int, int, int, void *__restrict, socklen_t *__restrict);
int setsockopt(int, int, int, const void *, socklen_t);

#ifdef __cplusplus
}
#endif

#endif
