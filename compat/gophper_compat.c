/* GOPHPER: stub implementations of POSIX APIs missing from wasi-libc (wasm32-wasip1).
 * Most of them fail with ENOSYS. A few have a trivial but working implementation. */
#include "gophper_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* Working directory: WASI starts every process in "/". The Go host passes
 * the intended one in GOPHPER_CWD, and this runs before main(). */

__attribute__((constructor))
static void gophper_init_cwd(void)
{
	const char *dir = getenv("GOPHPER_CWD");
	if (dir != NULL && dir[0] != '\0' && chdir(dir) != 0) {
		fprintf(stderr, "gophper: chdir(%s) failed: %s\n", dir, strerror(errno));
	}
	/* Keep it out of getenv() and $_SERVER. */
	unsetenv("GOPHPER_CWD");
}



static mode_t gophper_umask = 022;
mode_t umask(mode_t mask)
{
	mode_t old = gophper_umask;
	gophper_umask = mask & 0777;
	return old;
}


/* Time */

void tzset(void) {}

/* Processes */

pid_t getppid(void) { return 1; }
int getdtablesize(void) { return 1024; }
pid_t fork(void) { errno = ENOSYS; return -1; }
pid_t getpgrp(void) { return getpid(); }
/* The instance is its own process group and session. */
pid_t getpgid(pid_t pid) { return pid == 0 || pid == getpid() ? getpid() : (errno = ESRCH, -1); }
pid_t getsid(pid_t pid) { return getpgid(pid); }
char *ctermid(char *s) { static char tty[] = "/dev/tty"; return s ? strcpy(s, tty) : tty; }
/* Priorities are the host's business. */
int nice(int inc) { (void) inc; return 0; }
int getloadavg(double loadavg[], int nelem) { (void) loadavg; (void) nelem; errno = ENOSYS; return -1; }
int seteuid(uid_t uid) { return uid == geteuid() ? 0 : (errno = EPERM, -1); }
int setegid(gid_t gid) { return gid == getegid() ? 0 : (errno = EPERM, -1); }
pid_t setsid(void) { errno = EPERM; return -1; }
int setpgid(pid_t pid, pid_t pgid) { errno = EPERM; return -1; }
char *ttyname(int fd) { errno = ENOTTY; return NULL; }
int setuid(uid_t uid) { errno = EPERM; return -1; }
int setgid(gid_t gid) { errno = EPERM; return -1; }
int initgroups(const char *user, gid_t group) { errno = EPERM; return -1; }

int mkstemp(char *template)
{
	static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(template);

	if (len < 6 || memcmp(template + len - 6, "XXXXXX", 6) != 0) {
		errno = EINVAL;
		return -1;
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		for (size_t i = len - 6; i < len; i++) {
			template[i] = chars[arc4random_uniform(sizeof(chars) - 1)];
		}
		int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST) {
			return fd;
		}
	}
	errno = EEXIST;
	return -1;
}

/* tmpfile(3): wasi-libc has none. The file is unlinked at once, so that it
 * goes away with the last close, as tmpfile's does. */
FILE *tmpfile(void)
{
	char path[] = "/tmp/tmpfile.XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		return NULL;
	}
	unlink(path);
	FILE *f = fdopen(fd, "w+");
	if (f == NULL) {
		close(fd);
	}
	return f;
}

char *mktemp(char *template)
{
	int fd = mkstemp(template);
	if (fd < 0) {
		template[0] = '\0';
		return template;
	}
	close(fd);
	unlink(template);
	return template;
}

/* syslog: write to stderr */

void openlog(const char *ident, int option, int facility) {}
void closelog(void) {}

void vsyslog(int priority, const char *format, va_list ap)
{
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
}

void syslog(int priority, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsyslog(priority, format, ap);
	va_end(ap);
}

/* mman: wasi-emulated-mman lacks madvise. Advice is only a hint, so ignore it. */

#include <sys/mman.h>

int madvise(void *addr, size_t len, int advice) { return 0; }

/* wasi-emulated-mman refuses MAP_SHARED. One instance is one process, so a
 * shared anonymous mapping is no different from a private one. opcache
 * keeps its scripts in such a mapping. */
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	if ((flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
		flags = (flags & ~MAP_SHARED) | MAP_PRIVATE;
	}
	return __real_mmap(addr, len, prot, flags, fd, off);
}

/* Terminals */

#include <termios.h>

int tcgetattr(int fd, struct termios *t) { errno = ENOTTY; return -1; }
int tcsetattr(int fd, int action, const struct termios *t) { errno = ENOTTY; return -1; }

/* Resource limits: what a typical host allows, and setrlimit keeps them. */

static struct rlimit gophper_rlimits[RLIM_NLIMITS];
static int gophper_rlimits_ready;

int getrlimit(int resource, struct rlimit *rlim)
{
	if (resource < 0 || resource >= RLIM_NLIMITS) {
		errno = EINVAL;
		return -1;
	}
	if (!gophper_rlimits_ready) {
		for (int i = 0; i < RLIM_NLIMITS; i++) {
			gophper_rlimits[i] = (struct rlimit){RLIM_INFINITY, RLIM_INFINITY};
		}
		gophper_rlimits[RLIMIT_NOFILE] = (struct rlimit){1024, 1024};
		gophper_rlimits[RLIMIT_STACK] = (struct rlimit){8 << 20, 8 << 20};
		gophper_rlimits_ready = 1;
	}
	*rlim = gophper_rlimits[resource];
	return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
	struct rlimit cur;
	if (getrlimit(resource, &cur) != 0) {
		return -1;
	}
	if (rlim->rlim_cur > rlim->rlim_max || rlim->rlim_max > cur.rlim_max) {
		errno = EPERM;
		return -1;
	}
	gophper_rlimits[resource] = *rlim;
	return 0;
}

/* Special files cannot be made in WASI. */

int mkfifo(const char *path, mode_t mode) { (void) path; (void) mode; errno = ENOSYS; return -1; }
int mknod(const char *path, mode_t mode, dev_t dev) { (void) path; (void) mode; (void) dev; errno = ENOSYS; return -1; }

