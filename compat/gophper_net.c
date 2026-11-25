/* GOPHPER: sockets and DNS for wasm32-wasip1, backed by the Go host.
 *
 * wasip1 cannot open sockets, and wazero's poll_oneoff cannot wait on files
 * the host provides. So sockets live entirely in the host (gophper's internal/hostnet), and
 * this file routes the POSIX calls there:
 *
 * - socket() reserves a real WASI fd by opening /.gophper/socket, so the
 *   number never collides with a file. The host keys the socket by that fd.
 * - read, write, recv, send, close, poll, select, fcntl, ioctl, accept and
 *   shutdown are linked with --wrap. Calls on a socket fd go to the host.
 *   Calls on any other fd go to wasi-libc (__real_*).
 * - Addresses cross the boundary as text ("1.2.3.4:80", "[::1]:80" or a
 *   path), and flags as the host's own bits, so the host never depends on
 *   wasi-libc's struct layouts or constants. Errors come back as WASI errno.
 *
 * Unix socket paths are passed to the host unchanged. They work when the
 * guest path equals the host path, as with gophper's mounts.
 */
#include "gophper_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* Socket kinds, flags and poll bits shared with gophper's internal/hostnet. See ABI.md. */
enum {
	GOPHPER_TCP = 1,
	GOPHPER_UDP = 2,
	GOPHPER_UNIX = 3,
	GOPHPER_UNIXGRAM = 4,
};
enum {
	GOPHPER_MSG_PEEK = 1,
	GOPHPER_MSG_DONTWAIT = 2,
};
enum {
	GOPHPER_POLL_IN = 1,
	GOPHPER_POLL_OUT = 2,
	GOPHPER_POLL_HUP = 4,
	GOPHPER_POLL_ERR = 8,
};
enum {
	GOPHPER_OPT_ERROR = 1,
	GOPHPER_OPT_TYPE = 2,
	GOPHPER_OPT_NODELAY = 3,
	GOPHPER_OPT_KEEPALIVE = 4,
	GOPHPER_OPT_REUSEADDR = 5,
	GOPHPER_OPT_BROADCAST = 6,
	GOPHPER_OPT_RCVBUF = 7,
	GOPHPER_OPT_SNDBUF = 8,
};

/* Return a WASI errno, or 0. The int32_t ones return a count, or -errno. */
GOPHPER_IMPORT("sock_open") int32_t host_sock_open(int32_t fd, int32_t kind);
GOPHPER_IMPORT("sock_close") int32_t host_sock_close(int32_t fd);
GOPHPER_IMPORT("sock_connect") int32_t host_sock_connect(int32_t fd, const char *addr, int32_t addr_len, int32_t nonblock);
GOPHPER_IMPORT("sock_bind") int32_t host_sock_bind(int32_t fd, const char *addr, int32_t addr_len);
GOPHPER_IMPORT("sock_listen") int32_t host_sock_listen(int32_t fd, int32_t backlog);
GOPHPER_IMPORT("sock_accept") int32_t host_sock_accept(int32_t fd, int32_t new_fd, int32_t nonblock);
GOPHPER_IMPORT("sock_recv") int32_t host_sock_recv(int32_t fd, void *buf, int32_t len, int32_t flags, char *from, int32_t from_cap, int32_t *from_len);
GOPHPER_IMPORT("sock_send") int32_t host_sock_send(int32_t fd, const void *buf, int32_t len, int32_t flags, const char *to, int32_t to_len);
GOPHPER_IMPORT("sock_shutdown") int32_t host_sock_shutdown(int32_t fd, int32_t how);
GOPHPER_IMPORT("sock_name") int32_t host_sock_name(int32_t fd, int32_t peer, char *out, int32_t out_cap, int32_t *out_len);
GOPHPER_IMPORT("sock_getopt") int32_t host_sock_getopt(int32_t fd, int32_t opt, int32_t *value);
GOPHPER_IMPORT("sock_setopt") int32_t host_sock_setopt(int32_t fd, int32_t opt, int32_t value);
GOPHPER_IMPORT("sock_poll") int32_t host_sock_poll(int32_t *fds, int32_t *events, int32_t *revents, int32_t n, int32_t timeout_ms);
GOPHPER_IMPORT("sock_available") int32_t host_sock_available(int32_t fd);
GOPHPER_IMPORT("sock_pair") int32_t host_sock_pair(int32_t a, int32_t b);
GOPHPER_IMPORT("dns_lookup") int32_t host_dns_lookup(const char *name, int32_t name_len, int32_t family, char *out, int32_t out_cap);
GOPHPER_IMPORT("dns_reverse") int32_t host_dns_reverse(const char *addr, int32_t addr_len, char *out, int32_t out_cap);

ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_readv(int, const struct iovec *, int);
ssize_t __real_writev(int, const struct iovec *, int);
ssize_t __real_recv(int, void *, size_t, int);
ssize_t __real_send(int, const void *, size_t, int);
int __real_close(int);
int __real_poll(struct pollfd *, nfds_t, int);
int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int __real_fcntl(int, int, ...);
int __real_ioctl(int, int, ...);
int __real_accept(int, struct sockaddr *__restrict, socklen_t *__restrict);
int __real_accept4(int, struct sockaddr *__restrict, socklen_t *__restrict, int);
int __real_shutdown(int, int);

/* compat/gophper_fd.c: fd aliases from dup(2). Every wrapper resolves its
 * fd first, so a duplicate of a socket is that socket. */
int gophper_fd_resolve(int fd);
bool gophper_fd_close_name(int fd, int *target, bool *last);
int gophper_fd_fcntl_dupfd(int fd);
void gophper_fd_forget(int fd);
void gophper_lock_release(int fd);
/* The fd whose record locks always succeed. See __wrap_fcntl. */
static int gophper_fcntl_private_fd = -1;

#define GOPHPER_RESOLVE(fd)              \
	do {                                 \
		(fd) = gophper_fd_resolve(fd);   \
		if ((fd) < 0) {                  \
			return -1;                   \
		}                                \
	} while (0)

/* The socket table: which fds are host sockets, and their local state. */

struct gophper_socket {
	bool used;
	bool nonblock;
	/* One end of a pipe(2). The host treats it as a stream socket. */
	bool pipe;
	int family;
	int type;
};

static struct gophper_socket *gophper_sockets;
static int gophper_sockets_cap;

static struct gophper_socket *gophper_socket_get(int fd)
{
	if (fd < 0 || fd >= gophper_sockets_cap || !gophper_sockets[fd].used) {
		return NULL;
	}
	return &gophper_sockets[fd];
}

static struct gophper_socket *gophper_socket_add(int fd)
{
	if (fd >= gophper_sockets_cap) {
		int cap = gophper_sockets_cap ? gophper_sockets_cap : 64;
		while (cap <= fd) {
			cap *= 2;
		}
		struct gophper_socket *grown = realloc(gophper_sockets, cap * sizeof(*grown));
		if (grown == NULL) {
			return NULL;
		}
		memset(grown + gophper_sockets_cap, 0, (cap - gophper_sockets_cap) * sizeof(*grown));
		gophper_sockets = grown;
		gophper_sockets_cap = cap;
	}
	memset(&gophper_sockets[fd], 0, sizeof(gophper_sockets[fd]));
	gophper_sockets[fd].used = true;
	return &gophper_sockets[fd];
}

/* gophper_socket_kind tells gophper_fd.c and gophper_proc.c what fd is:
 * 0 for neither, 1 for a socket, 2 for a pipe. fd must be resolved. */
int gophper_socket_kind(int fd)
{
	struct gophper_socket *s = gophper_socket_get(fd);
	return s == NULL ? 0 : s->pipe ? 2 : 1;
}

/* gophper_pipe_add marks fd, a reserved placeholder, as a pipe end. */
bool gophper_pipe_add(int fd)
{
	struct gophper_socket *s = gophper_socket_add(fd);
	if (s == NULL) {
		return false;
	}
	s->pipe = true;
	s->family = AF_UNIX;
	s->type = SOCK_STREAM;
	return true;
}

/* Opens a placeholder file to reserve an fd number for a new socket. */
static int gophper_socket_reserve(void)
{
	return open("/.gophper/socket", O_RDONLY);
}

static int gophper_fail(int err)
{
	errno = err;
	return -1;
}

static int gophper_kind(int family, int type)
{
	switch (family) {
	case AF_INET:
	case AF_INET6:
		return type == SOCK_STREAM ? GOPHPER_TCP : type == SOCK_DGRAM ? GOPHPER_UDP : 0;
	case AF_UNIX:
		return type == SOCK_STREAM ? GOPHPER_UNIX : type == SOCK_DGRAM ? GOPHPER_UNIXGRAM : 0;
	}
	return 0;
}

/* Address conversion */

static int gophper_addr_to_text(const struct sockaddr *sa, socklen_t len, char *out, size_t cap)
{
	char ip[INET6_ADDRSTRLEN];

	if (sa == NULL) {
		return gophper_fail(EFAULT);
	}
	switch (sa->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *in = (const struct sockaddr_in *) sa;
		if (len < sizeof(*in) || inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip)) == NULL) {
			return gophper_fail(EINVAL);
		}
		return snprintf(out, cap, "%s:%u", ip, ntohs(in->sin_port));
	}
	case AF_INET6: {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) sa;
		if (len < sizeof(*in6) || inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip)) == NULL) {
			return gophper_fail(EINVAL);
		}
		return snprintf(out, cap, "[%s]:%u", ip, ntohs(in6->sin6_port));
	}
	case AF_UNIX: {
		const struct sockaddr_un *un = (const struct sockaddr_un *) sa;
		size_t max = len > offsetof(struct sockaddr_un, sun_path) ? len - offsetof(struct sockaddr_un, sun_path) : 0;
		if (max > sizeof(un->sun_path)) {
			max = sizeof(un->sun_path);
		}
		int plen = (int) strnlen(un->sun_path, max);
		/* The host maps paths as the mounts do, and knows no working
		 * directory: chdir() happens here. So a relative path is made
		 * absolute, as for files. */
		int n;
		if (plen > 0 && un->sun_path[0] != '/') {
			char cwd[PATH_MAX];
			if (getcwd(cwd, sizeof(cwd)) == NULL) {
				return -1;
			}
			n = snprintf(out, cap, "%s/%.*s", strcmp(cwd, "/") == 0 ? "" : cwd, plen, un->sun_path);
		} else {
			n = snprintf(out, cap, "%.*s", plen, un->sun_path);
		}
		/* snprintf returns the length it would have written: the callers
		 * would pass the host bytes past the end of out. */
		if (n < 0 || (size_t) n >= cap) {
			return gophper_fail(ENAMETOOLONG);
		}
		return n;
	}
	}
	return gophper_fail(EAFNOSUPPORT);
}

/* Parses "1.2.3.4:80", "[::1]:80" or a path, as the host formats them. */
static void gophper_text_to_addr(const char *text, int family, struct sockaddr *sa, socklen_t *len)
{
	struct sockaddr_storage ss;
	socklen_t n = 0;

	memset(&ss, 0, sizeof(ss));
	if (family == AF_UNIX) {
		struct sockaddr_un *un = (struct sockaddr_un *) &ss;
		un->sun_family = AF_UNIX;
		strncpy(un->sun_path, text, sizeof(un->sun_path) - 1);
		n = offsetof(struct sockaddr_un, sun_path) + strlen(un->sun_path) + 1;
	} else {
		char host[INET6_ADDRSTRLEN + 2];
		const char *colon = strrchr(text, ':');
		size_t hlen = colon ? (size_t) (colon - text) : strlen(text);
		unsigned port = colon ? (unsigned) atoi(colon + 1) : 0;

		if (hlen >= sizeof(host)) {
			hlen = sizeof(host) - 1;
		}
		memcpy(host, text, hlen);
		host[hlen] = '\0';
		if (host[0] == '[') {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) &ss;
			host[strlen(host) - 1] = '\0';
			in6->sin6_family = AF_INET6;
			in6->sin6_port = htons(port);
			inet_pton(AF_INET6, host + 1, &in6->sin6_addr);
			n = sizeof(*in6);
		} else {
			struct sockaddr_in *in = (struct sockaddr_in *) &ss;
			in->sin_family = AF_INET;
			in->sin_port = htons(port);
			inet_pton(AF_INET, host, &in->sin_addr);
			n = sizeof(*in);
		}
	}
	if (sa != NULL && len != NULL) {
		memcpy(sa, &ss, n < *len ? n : *len);
		*len = n;
	}
}

/* socket(2) and friends */

int socket(int domain, int type, int protocol)
{
	bool nonblock = (type & SOCK_NONBLOCK) != 0;
	int base = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	int kind = gophper_kind(domain, base);

	if (kind == 0) {
		return gophper_fail(domain == AF_INET || domain == AF_INET6 || domain == AF_UNIX ? EPROTONOSUPPORT : EAFNOSUPPORT);
	}
	int fd = gophper_socket_reserve();
	if (fd < 0) {
		return -1;
	}
	struct gophper_socket *s = gophper_socket_add(fd);
	if (s == NULL) {
		__real_close(fd);
		return gophper_fail(ENOMEM);
	}
	int err = host_sock_open(fd, kind);
	if (err != 0) {
		s->used = false;
		__real_close(fd);
		return gophper_fail(err);
	}
	s->nonblock = nonblock;
	s->family = domain;
	s->type = base;
	return fd;
}

/* socketpair(2): only AF_UNIX stream sockets, as proc_open's "socket"
 * descriptor uses. */
int socketpair(int domain, int type, int protocol, int sv[2])
{
	int base = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (domain != AF_UNIX) {
		return gophper_fail(EAFNOSUPPORT);
	}
	if (base != SOCK_STREAM) {
		return gophper_fail(EPROTONOSUPPORT);
	}
	int fds[2] = {gophper_socket_reserve(), -1};
	if (fds[0] < 0) {
		return -1;
	}
	fds[1] = gophper_socket_reserve();
	if (fds[1] < 0) {
		__real_close(fds[0]);
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		struct gophper_socket *s = gophper_socket_add(fds[i]);
		if (s == NULL) {
			close(fds[0]);
			close(fds[1]);
			return gophper_fail(ENOMEM);
		}
		s->nonblock = (type & SOCK_NONBLOCK) != 0;
		s->family = AF_UNIX;
		s->type = SOCK_STREAM;
	}
	int err = host_sock_pair(fds[0], fds[1]);
	if (err != 0) {
		close(fds[0]);
		close(fds[1]);
		return gophper_fail(err);
	}
	sv[0] = fds[0];
	sv[1] = fds[1];
	return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	char text[160];

	if (s == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int n = gophper_addr_to_text(addr, len, text, sizeof(text));
	if (n < 0) {
		return -1;
	}
	int err = host_sock_connect(fd, text, n, s->nonblock);
	return err ? gophper_fail(err) : 0;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	GOPHPER_RESOLVE(fd);
	char text[160];

	if (gophper_socket_get(fd) == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int n = gophper_addr_to_text(addr, len, text, sizeof(text));
	if (n < 0) {
		return -1;
	}
	int err = host_sock_bind(fd, text, n);
	return err ? gophper_fail(err) : 0;
}

int listen(int fd, int backlog)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_get(fd) == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int err = host_sock_listen(fd, backlog);
	return err ? gophper_fail(err) : 0;
}

static int gophper_accept(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
	struct gophper_socket *s = gophper_socket_get(fd);
	int new_fd = gophper_socket_reserve();

	if (new_fd < 0) {
		return -1;
	}
	struct gophper_socket *ns = gophper_socket_add(new_fd);
	if (ns == NULL) {
		__real_close(new_fd);
		return gophper_fail(ENOMEM);
	}
	/* gophper_socket_add may move the table. */
	s = gophper_socket_get(fd);
	int err = host_sock_accept(fd, new_fd, s->nonblock);
	if (err != 0) {
		ns->used = false;
		__real_close(new_fd);
		return gophper_fail(err);
	}
	ns->nonblock = (flags & SOCK_NONBLOCK) != 0;
	ns->family = s->family;
	ns->type = s->type;
	if (addr != NULL && len != NULL) {
		getpeername(new_fd, addr, len);
	}
	return new_fd;
}

int __wrap_accept(int fd, struct sockaddr *__restrict addr, socklen_t *__restrict len)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_get(fd) == NULL) {
		return __real_accept(fd, addr, len);
	}
	return gophper_accept(fd, addr, len, 0);
}

int __wrap_accept4(int fd, struct sockaddr *__restrict addr, socklen_t *__restrict len, int flags)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_get(fd) == NULL) {
		return __real_accept4(fd, addr, len, flags);
	}
	return gophper_accept(fd, addr, len, flags);
}

static int gophper_name(int fd, int peer, struct sockaddr *addr, socklen_t *len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	char text[160];
	int32_t n = 0;

	if (s == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int err = host_sock_name(fd, peer, text, sizeof(text) - 1, &n);
	if (err != 0) {
		return gophper_fail(err);
	}
	text[n] = '\0';
	gophper_text_to_addr(text, s->family, addr, len);
	return 0;
}

int getsockname(int fd, struct sockaddr *__restrict addr, socklen_t *__restrict len)
{
	return gophper_name(fd, 0, addr, len);
}

int getpeername(int fd, struct sockaddr *__restrict addr, socklen_t *__restrict len)
{
	return gophper_name(fd, 1, addr, len);
}

static int gophper_option(int level, int name)
{
	if (level == SOL_SOCKET) {
		switch (name) {
		case SO_ERROR: return GOPHPER_OPT_ERROR;
		case SO_TYPE: return GOPHPER_OPT_TYPE;
		case SO_KEEPALIVE: return GOPHPER_OPT_KEEPALIVE;
		case SO_REUSEADDR: return GOPHPER_OPT_REUSEADDR;
		case SO_BROADCAST: return GOPHPER_OPT_BROADCAST;
		case SO_RCVBUF: return GOPHPER_OPT_RCVBUF;
		case SO_SNDBUF: return GOPHPER_OPT_SNDBUF;
		}
	} else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
		return GOPHPER_OPT_NODELAY;
	}
	return 0;
}

int getsockopt(int fd, int level, int name, void *__restrict value, socklen_t *__restrict len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	int32_t v = 0;

	if (s == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int opt = gophper_option(level, name);
	if (opt == GOPHPER_OPT_TYPE) {
		v = s->type;
	} else if (opt != 0) {
		int err = host_sock_getopt(fd, opt, &v);
		if (err != 0) {
			return gophper_fail(err);
		}
	} else {
		return gophper_fail(ENOPROTOOPT);
	}
	if (value != NULL && len != NULL && *len >= sizeof(int)) {
		*(int *) value = v;
		*len = sizeof(int);
	}
	return 0;
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_get(fd) == NULL) {
		return gophper_fail(ENOTSOCK);
	}
	int opt = gophper_option(level, name);
	if (opt == 0 || opt == GOPHPER_OPT_ERROR || opt == GOPHPER_OPT_TYPE) {
		/* Timeouts, linger and the rest: accepted and ignored. PHP applies
		 * its own timeouts through poll(). */
		return 0;
	}
	int v = value != NULL && len >= sizeof(int) ? *(const int *) value : 0;
	int err = host_sock_setopt(fd, opt, v);
	return err ? gophper_fail(err) : 0;
}

/* Data transfer */

static int gophper_msg_flags(const struct gophper_socket *s, int flags)
{
	int out = 0;
	if (flags & MSG_PEEK) {
		out |= GOPHPER_MSG_PEEK;
	}
	if ((flags & MSG_DONTWAIT) || s->nonblock) {
		out |= GOPHPER_MSG_DONTWAIT;
	}
	return out;
}

static ssize_t gophper_result(int32_t n)
{
	return n < 0 ? gophper_fail(-n) : n;
}

static ssize_t gophper_recv(struct gophper_socket *s, int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *from_len)
{
	char text[160];
	int32_t text_len = 0;
	int32_t n = host_sock_recv(fd, buf, len > INT32_MAX ? INT32_MAX : (int32_t) len, gophper_msg_flags(s, flags), text, sizeof(text) - 1, &text_len);

	if (n >= 0 && from != NULL && from_len != NULL) {
		text[text_len] = '\0';
		gophper_text_to_addr(text, s->family, from, from_len);
	}
	return gophper_result(n);
}

static ssize_t gophper_send(struct gophper_socket *s, int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t to_len)
{
	char text[160];
	int n = 0;

	if (to != NULL) {
		n = gophper_addr_to_text(to, to_len, text, sizeof(text));
		if (n < 0) {
			return -1;
		}
	}
	return gophper_result(host_sock_send(fd, buf, len > INT32_MAX ? INT32_MAX : (int32_t) len, gophper_msg_flags(s, flags), to ? text : NULL, n));
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_recv(s, fd, buf, len, flags, NULL, NULL) : __real_recv(fd, buf, len, flags);
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_send(s, fd, buf, len, flags, NULL, 0) : __real_send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *__restrict buf, size_t len, int flags, struct sockaddr *__restrict from, socklen_t *__restrict from_len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_recv(s, fd, buf, len, flags, from, from_len) : gophper_fail(ENOTSOCK);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t to_len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_send(s, fd, buf, len, flags, to, to_len) : gophper_fail(ENOTSOCK);
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_recv(s, fd, buf, len, 0, NULL, NULL) : __real_read(fd, buf, len);
}

ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	return s ? gophper_send(s, fd, buf, len, 0, NULL, 0) : __real_write(fd, buf, len);
}

ssize_t __wrap_readv(int fd, const struct iovec *iov, int iovcnt)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	if (s == NULL) {
		return __real_readv(fd, iov, iovcnt);
	}
	/* One recv into the first non-empty buffer, which readv(2) allows. */
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len > 0) {
			return gophper_recv(s, fd, iov[i].iov_base, iov[i].iov_len, 0, NULL, NULL);
		}
	}
	return 0;
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int iovcnt)
{
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	if (s == NULL) {
		return __real_writev(fd, iov, iovcnt);
	}
	ssize_t total = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0) {
			continue;
		}
		ssize_t n = gophper_send(s, fd, iov[i].iov_base, iov[i].iov_len, 0, NULL, 0);
		if (n < 0) {
			return total > 0 ? total : -1;
		}
		total += n;
		if ((size_t) n < iov[i].iov_len) {
			break;
		}
	}
	return total;
}

int __wrap_shutdown(int fd, int how)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_get(fd) == NULL) {
		return __real_shutdown(fd, how);
	}
	int err = host_sock_shutdown(fd, how == SHUT_RD ? 1 : how == SHUT_WR ? 2 : 3);
	return err ? gophper_fail(err) : 0;
}

int __wrap_close(int fd)
{
	int target;
	bool last;
	if (gophper_fd_close_name(fd, &target, &last)) {
		if (!last) {
			return 0;
		}
		fd = target;
	}
	struct gophper_socket *s = gophper_socket_get(fd);
	if (s != NULL) {
		host_sock_close(fd);
		s->used = false;
	}
	gophper_lock_release(fd);
	if (fd == gophper_fcntl_private_fd) {
		gophper_fcntl_private_fd = -1;
	}
	gophper_fd_forget(fd);
	return __real_close(fd);
}

/* WASI has no record locks, and wasi-libc fails them. Only opcache's lock
 * file gets locks that always succeed: patches/0011 registers it, since the
 * shared memory it guards never leaves the instance. Any other file may be
 * shared with other instances or processes, such as an SQLite database whose
 * VFS a script picks with ?vfs=unix. A lock that succeeds there without
 * excluding anyone lets two writers corrupt it, so those still fail. */
void gophper_fcntl_private_locks(int fd)
{
	gophper_fcntl_private_fd = fd;
}

int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	int arg = va_arg(ap, int);
	va_end(ap);

	if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
		return gophper_fd_fcntl_dupfd(fd);
	}
	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	if (s == NULL) {
		if (fd == gophper_fcntl_private_fd) {
			switch (cmd) {
			case F_GETLK:
				((struct flock *) (uintptr_t) arg)->l_type = F_UNLCK;
				return 0;
			case F_SETLK:
			case F_SETLKW:
				return 0;
			}
		}
		return __real_fcntl(fd, cmd, arg);
	}
	switch (cmd) {
	case F_GETFL:
		return O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
	case F_SETFL:
		s->nonblock = (arg & O_NONBLOCK) != 0;
		return 0;
	case F_GETFD:
	case F_SETFD:
		return 0;
	}
	return gophper_fail(EINVAL);
}

int __wrap_ioctl(int fd, int request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	GOPHPER_RESOLVE(fd);
	struct gophper_socket *s = gophper_socket_get(fd);
	if (s == NULL) {
		return __real_ioctl(fd, request, arg);
	}
	switch (request) {
	case FIONBIO:
		s->nonblock = *(int *) arg != 0;
		return 0;
	case FIONREAD: {
		int32_t n = host_sock_available(fd);
		if (n < 0) {
			return gophper_fail(-n);
		}
		*(int *) arg = n;
		return 0;
	}
	}
	return gophper_fail(ENOTSUP);
}

/* Waiting */

/* Without sockets, poll() goes to wasi-libc unchanged. With them, the host
 * waits on the sockets. Other fds are checked between short host waits, so
 * a mixed set still notices either side. */
static int gophper_poll(struct pollfd *fds, nfds_t nfds, int timeout);

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	bool aliased = false;
	for (nfds_t i = 0; i < nfds && !aliased; i++) {
		aliased = fds[i].fd >= 0 && gophper_fd_resolve(fds[i].fd) != fds[i].fd;
	}
	if (!aliased) {
		return gophper_poll(fds, nfds, timeout);
	}
	struct pollfd *resolved = malloc(nfds * sizeof(*resolved));
	if (resolved == NULL) {
		return gophper_fail(ENOMEM);
	}
	for (nfds_t i = 0; i < nfds; i++) {
		resolved[i] = fds[i];
		if (fds[i].fd >= 0) {
			resolved[i].fd = gophper_fd_resolve(fds[i].fd);
		}
	}
	int n = gophper_poll(resolved, nfds, timeout);
	for (nfds_t i = 0; i < nfds; i++) {
		fds[i].revents = resolved[i].revents;
	}
	free(resolved);
	return n;
}

static int gophper_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	int nsock = 0;
	for (nfds_t i = 0; i < nfds; i++) {
		if (gophper_socket_get(fds[i].fd) != NULL) {
			nsock++;
		}
	}
	if (nsock == 0) {
		return __real_poll(fds, nfds, timeout);
	}

	int nother = (int) nfds - nsock;
	int32_t *sfd = malloc(nsock * sizeof(int32_t) * 3);
	struct pollfd *other = nother > 0 ? malloc(nother * sizeof(struct pollfd)) : NULL;
	if (sfd == NULL || (nother > 0 && other == NULL)) {
		free(sfd);
		free(other);
		return gophper_fail(ENOMEM);
	}
	int32_t *sevents = sfd + nsock, *srevents = sfd + 2 * nsock;

	for (nfds_t i = 0, j = 0, k = 0; i < nfds; i++) {
		fds[i].revents = 0;
		if (gophper_socket_get(fds[i].fd) != NULL) {
			sfd[j] = fds[i].fd;
			sevents[j] = ((fds[i].events & POLLIN) ? GOPHPER_POLL_IN : 0) | ((fds[i].events & POLLOUT) ? GOPHPER_POLL_OUT : 0);
			j++;
		} else {
			other[k++] = fds[i];
		}
	}

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int ready;
	for (;;) {
		int wait = timeout;
		if (nother > 0) {
			ready = __real_poll(other, nother, 0);
			if (ready != 0) {
				break;
			}
			wait = timeout < 0 || timeout > 10 ? 10 : timeout;
		}
		ready = host_sock_poll(sfd, sevents, srevents, nsock, wait);
		if (ready != 0) {
			break;
		}
		if (timeout >= 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
			if (elapsed >= timeout) {
				break;
			}
		}
	}

	if (ready < 0) {
		free(sfd);
		free(other);
		return gophper_fail(-ready);
	}
	int count = 0;
	for (nfds_t i = 0, j = 0, k = 0; i < nfds; i++) {
		if (gophper_socket_get(fds[i].fd) != NULL) {
			int r = srevents[j++];
			fds[i].revents = ((r & GOPHPER_POLL_IN) ? POLLIN : 0) | ((r & GOPHPER_POLL_OUT) ? POLLOUT : 0) |
				((r & GOPHPER_POLL_HUP) ? POLLHUP : 0) | ((r & GOPHPER_POLL_ERR) ? POLLERR : 0);
		} else {
			fds[i].revents = other[k++].revents;
		}
		if (fds[i].revents != 0) {
			count++;
		}
	}
	free(sfd);
	free(other);
	return count;
}

int __wrap_select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *tv)
{
	bool any = false;
	for (int fd = 0; fd < nfds && !any; fd++) {
		any = gophper_socket_get(fd) != NULL &&
			((rfds && FD_ISSET(fd, rfds)) || (wfds && FD_ISSET(fd, wfds)) || (efds && FD_ISSET(fd, efds)));
	}
	if (!any) {
		return __real_select(nfds, rfds, wfds, efds, tv);
	}

	struct pollfd *fds = calloc(nfds > 0 ? nfds : 1, sizeof(*fds));
	if (fds == NULL) {
		return gophper_fail(ENOMEM);
	}
	int n = 0;
	for (int fd = 0; fd < nfds; fd++) {
		short ev = 0;
		if (rfds && FD_ISSET(fd, rfds)) ev |= POLLIN;
		if (wfds && FD_ISSET(fd, wfds)) ev |= POLLOUT;
		if (efds && FD_ISSET(fd, efds)) ev |= POLLPRI;
		if (ev) {
			fds[n].fd = fd;
			fds[n].events = ev;
			n++;
		}
	}
	/* Rounded up: a timeout under a millisecond must still wait. */
	int timeout = tv ? (int) (tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000) : -1;
	int r = __wrap_poll(fds, n, timeout);
	if (r < 0) {
		free(fds);
		return -1;
	}
	if (rfds) FD_ZERO(rfds);
	if (wfds) FD_ZERO(wfds);
	if (efds) FD_ZERO(efds);
	/* As Linux does: an error or a hangup makes a socket readable, and an
	 * error writable, but only in the sets the caller asked for. A failed
	 * connect, waited on for writing, must not turn up as readable too. */
	int count = 0;
	for (int i = 0; i < n; i++) {
		short ev = fds[i].events, rev = fds[i].revents;
		if ((ev & POLLIN) && (rev & (POLLIN | POLLHUP | POLLERR))) { FD_SET(fds[i].fd, rfds); count++; }
		if ((ev & POLLOUT) && (rev & (POLLOUT | POLLERR))) { FD_SET(fds[i].fd, wfds); count++; }
		if ((ev & POLLPRI) && (rev & POLLPRI)) { FD_SET(fds[i].fd, efds); count++; }
	}
	free(fds);
	return count;
}

/* DNS */

int h_errno;

/* Looks up name and returns its addresses as a list of "1.2.3.4" lines,
 * or NULL with *err set to an EAI_* code. */
static char *gophper_lookup(const char *name, int family, int *err)
{
	int32_t cap = 1024;
	for (;;) {
		char *out = malloc(cap + 1);
		if (out == NULL) {
			*err = EAI_MEMORY;
			return NULL;
		}
		int32_t n = host_dns_lookup(name, (int32_t) strlen(name), family == AF_INET6 ? 6 : family == AF_INET ? 4 : 0, out, cap);
		if (n >= 0 && n <= cap) {
			out[n] = '\0';
			return out;
		}
		free(out);
		if (n > cap) {
			cap = n;
			continue;
		}
		*err = n == -1 ? EAI_NONAME : EAI_FAIL;
		return NULL;
	}
}

static int gophper_service_port(const char *service, int socktype, int flags)
{
	if (service == NULL || service[0] == '\0') {
		return 0;
	}
	char *end;
	long port = strtol(service, &end, 10);
	if (*end == '\0' && port >= 0 && port <= 65535) {
		return (int) port;
	}
	if (flags & AI_NUMERICSERV) {
		return -1;
	}
	struct servent *se = getservbyname(service, socktype == SOCK_DGRAM ? "udp" : "tcp");
	return se ? ntohs(se->s_port) : -1;
}

static struct addrinfo *gophper_addrinfo(int family, const void *addr, int port, int socktype, int protocol)
{
	struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_storage));
	if (ai == NULL) {
		return NULL;
	}
	ai->ai_family = family;
	ai->ai_socktype = socktype;
	ai->ai_protocol = protocol;
	ai->ai_addr = (struct sockaddr *) (ai + 1);
	if (family == AF_INET6) {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *) ai->ai_addr;
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
		memcpy(&in6->sin6_addr, addr, sizeof(in6->sin6_addr));
		ai->ai_addrlen = sizeof(*in6);
	} else {
		struct sockaddr_in *in = (struct sockaddr_in *) ai->ai_addr;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		memcpy(&in->sin_addr, addr, sizeof(in->sin_addr));
		ai->ai_addrlen = sizeof(*in);
	}
	return ai;
}

int getaddrinfo(const char *__restrict node, const char *__restrict service, const struct addrinfo *__restrict hints, struct addrinfo **__restrict res)
{
	int family = hints ? hints->ai_family : AF_UNSPEC;
	int socktype = hints ? hints->ai_socktype : 0;
	int protocol = hints ? hints->ai_protocol : 0;
	int flags = hints ? hints->ai_flags : 0;
	struct addrinfo *head = NULL, **tail = &head;

	if (node == NULL && service == NULL) {
		return EAI_NONAME;
	}
	int port = gophper_service_port(service, socktype, flags);
	if (port < 0) {
		return EAI_SERVICE;
	}

	unsigned char buf[16];
	char *list = NULL;
	if (node == NULL) {
		/* The wildcard address for a server, loopback for a client. */
		node = (flags & AI_PASSIVE) ? (family == AF_INET6 ? "::" : "0.0.0.0") : (family == AF_INET6 ? "::1" : "127.0.0.1");
	}
	if (inet_pton(AF_INET, node, buf) == 1 || inet_pton(AF_INET6, node, buf) == 1) {
		list = strdup(node);
	} else if (flags & AI_NUMERICHOST) {
		return EAI_NONAME;
	} else {
		int err = 0;
		list = gophper_lookup(node, family, &err);
		if (list == NULL) {
			return err;
		}
	}

	for (char *line = strtok(list, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		int af = strchr(line, ':') ? AF_INET6 : AF_INET;
		if ((family != AF_UNSPEC && family != af) || inet_pton(af, line, buf) != 1) {
			continue;
		}
		int types[2] = {socktype ? socktype : SOCK_STREAM, socktype ? 0 : SOCK_DGRAM};
		for (int t = 0; t < 2 && types[t]; t++) {
			int proto = protocol ? protocol : types[t] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
			struct addrinfo *ai = gophper_addrinfo(af, buf, port, types[t], proto);
			if (ai == NULL) {
				free(list);
				freeaddrinfo(head);
				return EAI_MEMORY;
			}
			*tail = ai;
			tail = &ai->ai_next;
		}
	}
	free(list);
	if (head == NULL) {
		return EAI_NONAME;
	}
	if ((flags & AI_CANONNAME) && head != NULL) {
		head->ai_canonname = strdup(node);
	}
	*res = head;
	return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
	while (ai != NULL) {
		struct addrinfo *next = ai->ai_next;
		free(ai->ai_canonname);
		free(ai);
		ai = next;
	}
}

int getnameinfo(const struct sockaddr *__restrict sa, socklen_t salen, char *__restrict host, socklen_t hostlen, char *__restrict serv, socklen_t servlen, int flags)
{
	char ip[INET6_ADDRSTRLEN];
	int port;

	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in *) sa;
		inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
		port = ntohs(in->sin_port);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) sa;
		inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
		port = ntohs(in6->sin6_port);
	} else {
		return EAI_FAMILY;
	}
	if (host != NULL && hostlen > 0) {
		int n = (flags & NI_NUMERICHOST) ? -1 : host_dns_reverse(ip, (int32_t) strlen(ip), host, hostlen - 1);
		if (n >= 0) {
			host[n] = '\0';
		} else if (flags & NI_NAMEREQD) {
			return EAI_NONAME;
		} else {
			snprintf(host, hostlen, "%s", ip);
		}
	}
	if (serv != NULL && servlen > 0) {
		snprintf(serv, servlen, "%d", port);
	}
	return 0;
}

const char *gai_strerror(int code)
{
	switch (code) {
	case EAI_NONAME: return "Name or service not known";
	case EAI_AGAIN: return "Temporary failure in name resolution";
	case EAI_FAIL: return "Non-recoverable failure in name resolution";
	case EAI_FAMILY: return "Address family not supported";
	case EAI_SERVICE: return "Servname not supported for ai_socktype";
	case EAI_MEMORY: return "Memory allocation failure";
	}
	return "Unknown error";
}

/* gethostbyname and gethostbyaddr return static storage, as in POSIX. */
static struct hostent gophper_hostent;
static char *gophper_hostent_aliases[1];
static char *gophper_hostent_addrs[17];
static struct in_addr gophper_hostent_buf[16];
static char gophper_hostent_name[256];

struct hostent *gethostbyname(const char *name)
{
	struct in_addr a;
	int n = 0;

	if (inet_pton(AF_INET, name, &a) == 1) {
		gophper_hostent_buf[n++] = a;
	} else {
		int err = 0;
		char *list = gophper_lookup(name, AF_INET, &err);
		if (list == NULL) {
			h_errno = err == EAI_NONAME ? HOST_NOT_FOUND : NO_RECOVERY;
			return NULL;
		}
		for (char *line = strtok(list, "\n"); line != NULL && n < 16; line = strtok(NULL, "\n")) {
			if (inet_pton(AF_INET, line, &gophper_hostent_buf[n]) == 1) {
				n++;
			}
		}
		free(list);
		if (n == 0) {
			h_errno = NO_DATA;
			return NULL;
		}
	}
	for (int i = 0; i < n; i++) {
		gophper_hostent_addrs[i] = (char *) &gophper_hostent_buf[i];
	}
	gophper_hostent_addrs[n] = NULL;
	snprintf(gophper_hostent_name, sizeof(gophper_hostent_name), "%s", name);
	gophper_hostent.h_name = gophper_hostent_name;
	gophper_hostent.h_aliases = gophper_hostent_aliases;
	gophper_hostent.h_addrtype = AF_INET;
	gophper_hostent.h_length = sizeof(struct in_addr);
	gophper_hostent.h_addr_list = gophper_hostent_addrs;
	return &gophper_hostent;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
	char ip[INET6_ADDRSTRLEN];

	if (inet_ntop(type, addr, ip, sizeof(ip)) == NULL) {
		h_errno = NO_RECOVERY;
		return NULL;
	}
	int n = host_dns_reverse(ip, (int32_t) strlen(ip), gophper_hostent_name, sizeof(gophper_hostent_name) - 1);
	if (n < 0) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}
	gophper_hostent_name[n] = '\0';
	memcpy(&gophper_hostent_buf[0], addr, len < sizeof(gophper_hostent_buf[0]) ? len : sizeof(gophper_hostent_buf[0]));
	gophper_hostent_addrs[0] = (char *) &gophper_hostent_buf[0];
	gophper_hostent_addrs[1] = NULL;
	gophper_hostent.h_name = gophper_hostent_name;
	gophper_hostent.h_aliases = gophper_hostent_aliases;
	gophper_hostent.h_addrtype = type;
	gophper_hostent.h_length = len;
	gophper_hostent.h_addr_list = gophper_hostent_addrs;
	return &gophper_hostent;
}

const char *hstrerror(int code)
{
	switch (code) {
	case HOST_NOT_FOUND: return "Unknown host";
	case TRY_AGAIN: return "Host name lookup failure";
	case NO_RECOVERY: return "Unknown server error";
	case NO_DATA: return "No address associated with name";
	}
	return "Unknown error";
}

/* Service and protocol tables: the entries PHP and its extensions ask for. */

static const struct {
	const char *name;
	int port;
	const char *proto;
} gophper_services[] = {
	{"ftp", 21, "tcp"}, {"ssh", 22, "tcp"}, {"telnet", 23, "tcp"}, {"smtp", 25, "tcp"},
	{"domain", 53, "udp"}, {"domain", 53, "tcp"}, {"http", 80, "tcp"}, {"pop3", 110, "tcp"},
	{"ntp", 123, "udp"}, {"imap", 143, "tcp"}, {"ldap", 389, "tcp"}, {"https", 443, "tcp"},
	{"submission", 587, "tcp"}, {"imaps", 993, "tcp"}, {"pop3s", 995, "tcp"},
	{"mysql", 3306, "tcp"}, {"postgresql", 5432, "tcp"}, {"redis", 6379, "tcp"},
};

static struct servent gophper_servent;
static char *gophper_servent_aliases[1];

static struct servent *gophper_servent_fill(int i)
{
	gophper_servent.s_name = (char *) gophper_services[i].name;
	gophper_servent.s_aliases = gophper_servent_aliases;
	gophper_servent.s_port = htons(gophper_services[i].port);
	gophper_servent.s_proto = (char *) gophper_services[i].proto;
	return &gophper_servent;
}

struct servent *getservbyname(const char *name, const char *proto)
{
	for (size_t i = 0; i < sizeof(gophper_services) / sizeof(gophper_services[0]); i++) {
		if (strcmp(gophper_services[i].name, name) == 0 && (proto == NULL || strcmp(gophper_services[i].proto, proto) == 0)) {
			return gophper_servent_fill(i);
		}
	}
	return NULL;
}

struct servent *getservbyport(int port, const char *proto)
{
	for (size_t i = 0; i < sizeof(gophper_services) / sizeof(gophper_services[0]); i++) {
		if (htons(gophper_services[i].port) == port && (proto == NULL || strcmp(gophper_services[i].proto, proto) == 0)) {
			return gophper_servent_fill(i);
		}
	}
	return NULL;
}

static const struct {
	const char *name;
	int number;
} gophper_protocols[] = {
	{"ip", 0}, {"icmp", 1}, {"tcp", 6}, {"udp", 17}, {"ipv6", 41}, {"icmpv6", 58},
};

static struct protoent gophper_protoent;
static char *gophper_protoent_aliases[1];

static struct protoent *gophper_protoent_fill(int i)
{
	gophper_protoent.p_name = (char *) gophper_protocols[i].name;
	gophper_protoent.p_aliases = gophper_protoent_aliases;
	gophper_protoent.p_proto = gophper_protocols[i].number;
	return &gophper_protoent;
}

struct protoent *getprotobyname(const char *name)
{
	for (size_t i = 0; i < sizeof(gophper_protocols) / sizeof(gophper_protocols[0]); i++) {
		if (strcmp(gophper_protocols[i].name, name) == 0) {
			return gophper_protoent_fill(i);
		}
	}
	return NULL;
}

struct protoent *getprotobynumber(int number)
{
	for (size_t i = 0; i < sizeof(gophper_protocols) / sizeof(gophper_protocols[0]); i++) {
		if (gophper_protocols[i].number == number) {
			return gophper_protoent_fill(i);
		}
	}
	return NULL;
}
