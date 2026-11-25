/* GOPHPER: dup(2) for wasm32-wasip1, and the paths of open files.
 *
 * Paths: open() records the absolute path and flags of each fd, so that
 * gophper_proc.c can give a child process the same file. The host opens it
 * again by that path.
 *
 * WASI cannot duplicate a file descriptor. wasi-libc's dup() fails, and its
 * dup2() moves the descriptor with fd_renumber, which closes the old one.
 * PHP needs real duplicates, for php://stdout and proc_open among others.
 *
 * Here a duplicate is an alias: a reserved fd number that names another
 * descriptor, the target. The calls below resolve aliases first, and the
 * target is closed only when its last name is. Like sockets, a reserved
 * number is an open placeholder file, so it never collides with a real fd.
 */
#include "gophper_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wasi/api.h>

int gophper_fd_resolve(int fd);
bool gophper_fd_close_name(int fd, int *target, bool *last);
void gophper_fd_forget(int fd);
const char *gophper_fd_path(int fd, int *flags);
int gophper_socket_kind(int fd);
void gophper_stat_fill(const char *path, struct stat *st, int follow);

int __real_open(const char *, int, ...);

int __real_close(int);
int __real_fcntl(int, int, ...);
int __real_fstat(int, struct stat *);
off_t __real_lseek(int, off_t, int);
int __real_ftruncate(int, off_t);
int __real_fsync(int);
int __real_fdatasync(int);
int __real_futimens(int, const struct timespec[2]);
int __real_isatty(int);
int __real_fstatvfs(int, struct statvfs *);
DIR *__real_fdopendir(int);

struct gophper_fd {
	/* For an alias: the descriptor it names. -1 otherwise. */
	int target;
	/* For a target: how many names it has, itself included. 0: not tracked. */
	int names;
	/* The target's own number was closed while aliases still name it. */
	bool closed;
	/* For a file from open(): its absolute path and open flags. */
	char *path;
	int flags;
};

static struct gophper_fd *gophper_fds;
static int gophper_fds_cap;

static struct gophper_fd *gophper_fd_at(int fd, bool grow)
{
	if (fd < 0) {
		return NULL;
	}
	if (fd >= gophper_fds_cap) {
		if (!grow) {
			return NULL;
		}
		int cap = gophper_fds_cap ? gophper_fds_cap : 64;
		while (cap <= fd) {
			cap *= 2;
		}
		struct gophper_fd *grown = realloc(gophper_fds, cap * sizeof(*grown));
		if (grown == NULL) {
			return NULL;
		}
		for (int i = gophper_fds_cap; i < cap; i++) {
			grown[i] = (struct gophper_fd){.target = -1};
		}
		gophper_fds = grown;
		gophper_fds_cap = cap;
	}
	return &gophper_fds[fd];
}

/* gophper_fd_resolve returns the descriptor fd names, or -1 with EBADF
 * for a closed name. Used by every wrapper, including gophper_net.c's. */
int gophper_fd_resolve(int fd)
{
	struct gophper_fd *e = gophper_fd_at(fd, false);
	if (e == NULL) {
		return fd;
	}
	if (e->target >= 0) {
		return e->target;
	}
	if (e->closed) {
		errno = EBADF;
		return -1;
	}
	return fd;
}

/* gophper_fd_close_name forgets the name fd. It returns false when fd was
 * not part of an alias group, and the caller closes it as usual. Otherwise
 * *target is the descriptor, and *last says whether its last name went, so
 * that the caller closes the descriptor itself. */
bool gophper_fd_close_name(int fd, int *target, bool *last)
{
	struct gophper_fd *e = gophper_fd_at(fd, false);
	if (e == NULL || (e->target < 0 && e->names == 0)) {
		return false;
	}
	int t = fd;
	if (e->target >= 0) {
		t = e->target;
		e->target = -1;
		/* The alias's placeholder file. */
		__real_close(fd);
	} else {
		e->closed = true;
	}
	struct gophper_fd *te = gophper_fd_at(t, false);
	*target = t;
	*last = --te->names == 0;
	if (*last) {
		te->closed = false;
	}
	return true;
}

/* gophper_fd_forget drops what is known about fd, which is being closed. */
void gophper_fd_forget(int fd)
{
	struct gophper_fd *e = gophper_fd_at(fd, false);
	if (e != NULL && e->target < 0 && !e->closed) {
		free(e->path);
		e->path = NULL;
	}
}

/* gophper_fd_path returns the path fd was opened with, or NULL. */
const char *gophper_fd_path(int fd, int *flags)
{
	struct gophper_fd *e = gophper_fd_at(gophper_fd_resolve(fd), false);
	if (e == NULL || e->path == NULL) {
		return NULL;
	}
	*flags = e->flags;
	return e->path;
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	int fd = __real_open(path, flags, mode);
	if (fd < 0) {
		return fd;
	}
	struct gophper_fd *e = gophper_fd_at(fd, true);
	if (e == NULL) {
		return fd;
	}
	free(e->path);
	e->path = NULL;
	if (path[0] == '/') {
		e->path = strdup(path);
	} else {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			size_t n = strlen(cwd) + strlen(path) + 2;
			e->path = malloc(n);
			if (e->path != NULL) {
				snprintf(e->path, n, "%s/%s", cwd, path);
			}
		}
	}
	e->flags = flags;
	return fd;
}

static int gophper_fd_alias(int target, int at)
{
	int fd = open("/.gophper/socket", O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	if (at >= 0 && fd != at) {
		if (__wasi_fd_renumber(fd, at) != 0) {
			__real_close(fd);
			errno = EBADF;
			return -1;
		}
		fd = at;
	}
	struct gophper_fd *te = gophper_fd_at(target, true);
	struct gophper_fd *e = gophper_fd_at(fd, true);
	if (te == NULL || e == NULL) {
		__real_close(fd);
		errno = ENOMEM;
		return -1;
	}
	te = gophper_fd_at(target, false);
	if (te->names == 0) {
		te->names = 1;
	}
	te->names++;
	e->target = target;
	e->names = 0;
	e->closed = false;
	return fd;
}

static bool gophper_fd_valid(int fd)
{
	return fd >= 0 && __real_fcntl(fd, F_GETFD) >= 0;
}

int __wrap_dup(int fd)
{
	int t = gophper_fd_resolve(fd);
	if (t < 0 || !gophper_fd_valid(t)) {
		errno = EBADF;
		return -1;
	}
	return gophper_fd_alias(t, -1);
}

int __wrap_dup2(int fd, int to)
{
	int t = gophper_fd_resolve(fd);
	if (t < 0 || !gophper_fd_valid(t)) {
		errno = EBADF;
		return -1;
	}
	if (fd == to) {
		return to;
	}
	if (gophper_fd_resolve(to) == t) {
		return to;
	}
	close(to);
	return gophper_fd_alias(t, to);
}

int __wrap_dup3(int fd, int to, int flags)
{
	if (fd == to) {
		errno = EINVAL;
		return -1;
	}
	(void) flags;
	return __wrap_dup2(fd, to);
}

/* gophper_fd_fcntl_dupfd is fcntl(F_DUPFD): the lowest free number >= min
 * is not available in WASI, so any free number is returned. */
int gophper_fd_fcntl_dupfd(int fd)
{
	return __wrap_dup(fd);
}

#define GOPHPER_RESOLVE(fd)              \
	do {                                 \
		(fd) = gophper_fd_resolve(fd);   \
		if ((fd) < 0) {                  \
			return -1;                   \
		}                                \
	} while (0)

/* A socket or pipe fd is a placeholder file on the guest side. fstat reports
 * what it stands for, so that PHP's plain streams treat a pipe as one. */
int __wrap_fstat(int fd, struct stat *st)
{
	GOPHPER_RESOLVE(fd);
	int kind = gophper_socket_kind(fd);
	if (kind == 0) {
		if (__real_fstat(fd, st) != 0) {
			return -1;
		}
		int flags;
		const char *path = gophper_fd_path(fd, &flags);
		if (path != NULL) {
			gophper_stat_fill(path, st, 1);
		}
		return 0;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = (kind == 2 ? S_IFIFO : S_IFSOCK) | 0600;
	st->st_nlink = 1;
	st->st_blksize = 4096;
	return 0;
}

off_t __wrap_lseek(int fd, off_t off, int whence)
{
	GOPHPER_RESOLVE(fd);
	if (gophper_socket_kind(fd) != 0) {
		errno = ESPIPE;
		return -1;
	}
	return __real_lseek(fd, off, whence);
}
int __wrap_ftruncate(int fd, off_t len) { GOPHPER_RESOLVE(fd); return __real_ftruncate(fd, len); }
int __wrap_fsync(int fd) { GOPHPER_RESOLVE(fd); return __real_fsync(fd); }
int __wrap_fdatasync(int fd) { GOPHPER_RESOLVE(fd); return __real_fdatasync(fd); }
int __wrap_futimens(int fd, const struct timespec ts[2]) { GOPHPER_RESOLVE(fd); return __real_futimens(fd, ts); }
int __wrap_isatty(int fd) { fd = gophper_fd_resolve(fd); return fd < 0 ? 0 : __real_isatty(fd); }
int __wrap_fstatvfs(int fd, struct statvfs *st) { GOPHPER_RESOLVE(fd); return __real_fstatvfs(fd, st); }

DIR *__wrap_fdopendir(int fd)
{
	fd = gophper_fd_resolve(fd);
	return fd < 0 ? NULL : __real_fdopendir(fd);
}
