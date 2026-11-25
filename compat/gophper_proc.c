/* GOPHPER: child processes for wasm32-wasip1.
 *
 * WASI cannot create processes, so the host starts them: posix_spawn()
 * describes the child to gophper.proc_spawn, which runs it with Go's
 * os/exec. A pipe(2) is a host pipe, and on the guest side a socket fd that
 * compat/gophper_net.c routes to the host, like any socket.
 *
 * The child gets only the fds that its file actions name, plus stdin,
 * stdout and stderr. Each is described by what it stands for: a host pipe
 * or socket, one of the instance's own stdio streams, or a file the host
 * opens again by path. Other fds, such as a directory, cannot be passed.
 *
 * popen() and pclose() are built on these, so exec(), shell_exec(),
 * system() and passthru() work too.
 */
#include "gophper_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wasi/api.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* What a child fd stands for. Shared with gophper's internal/hostproc. See ABI.md. */
enum {
	GOPHPER_CHILD_HOST = 1,
	GOPHPER_CHILD_STDIO = 2,
	GOPHPER_CHILD_FILE = 3,
};
/* Open flags for GOPHPER_CHILD_FILE. */
enum {
	GOPHPER_OPEN_READ = 1,
	GOPHPER_OPEN_WRITE = 2,
	GOPHPER_OPEN_APPEND = 4,
	GOPHPER_OPEN_CREATE = 8,
	GOPHPER_OPEN_TRUNCATE = 16,
	GOPHPER_OPEN_EXCLUSIVE = 32,
};

struct gophper_child_fd {
	int32_t child;
	int32_t kind;
	int32_t value;
	int32_t flags;
	const char *path;
	int32_t path_len;
};

/* Return a WASI errno, or 0. proc_wait returns a pid, 0, or -errno. */
GOPHPER_IMPORT("pipe_open") int32_t host_pipe_open(int32_t read_fd, int32_t write_fd);
GOPHPER_IMPORT("proc_spawn") int32_t host_proc_spawn(const char *path, int32_t path_len, int32_t search,
	const char *argv, int32_t argv_len, const char *envp, int32_t envp_len, const char *cwd, int32_t cwd_len,
	const struct gophper_child_fd *fds, int32_t nfds, int32_t *pid);
GOPHPER_IMPORT("proc_wait") int32_t host_proc_wait(int32_t pid, int32_t nohang, int32_t *status);
GOPHPER_IMPORT("proc_kill") int32_t host_proc_kill(int32_t pid, int32_t sig);

int gophper_fd_resolve(int fd);
const char *gophper_fd_path(int fd, int *flags);
int gophper_socket_kind(int fd);
bool gophper_pipe_add(int fd);
int __real_close(int);

extern char **environ;

/* pipe(2) */

int __wrap_pipe(int fds[2])
{
	int r = open("/.gophper/socket", O_RDONLY);
	if (r < 0) {
		return -1;
	}
	int w = open("/.gophper/socket", O_RDONLY);
	if (w < 0) {
		__real_close(r);
		return -1;
	}
	if (!gophper_pipe_add(r) || !gophper_pipe_add(w)) {
		close(r);
		close(w);
		errno = ENOMEM;
		return -1;
	}
	int err = host_pipe_open(r, w);
	if (err != 0) {
		close(r);
		close(w);
		errno = err;
		return -1;
	}
	fds[0] = r;
	fds[1] = w;
	return 0;
}

int __wrap_pipe2(int fds[2], int flags)
{
	if (__wrap_pipe(fds) != 0) {
		return -1;
	}
	if (flags & O_NONBLOCK) {
		fcntl(fds[0], F_SETFL, O_NONBLOCK);
		fcntl(fds[1], F_SETFL, O_NONBLOCK);
	}
	return 0;
}

/* posix_spawn(3) */

enum {
	GOPHPER_ACTION_CLOSE,
	GOPHPER_ACTION_DUP2,
	GOPHPER_ACTION_OPEN,
	GOPHPER_ACTION_CHDIR,
};

struct __gophper_spawn_action {
	int op;
	int fd;
	int newfd;
	char *path;
	int flags;
	mode_t mode;
};

static int gophper_action_add(posix_spawn_file_actions_t *fa, struct __gophper_spawn_action a)
{
	if (fa->__n == fa->__cap) {
		int cap = fa->__cap ? fa->__cap * 2 : 8;
		struct __gophper_spawn_action *grown = realloc(fa->__actions, cap * sizeof(*grown));
		if (grown == NULL) {
			free(a.path);
			return ENOMEM;
		}
		fa->__actions = grown;
		fa->__cap = cap;
	}
	fa->__actions[fa->__n++] = a;
	return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
	memset(fa, 0, sizeof(*fa));
	return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
	for (int i = 0; i < fa->__n; i++) {
		free(fa->__actions[i].path);
	}
	free(fa->__actions);
	memset(fa, 0, sizeof(*fa));
	return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
	return fd < 0 ? EBADF : gophper_action_add(fa, (struct __gophper_spawn_action){.op = GOPHPER_ACTION_CLOSE, .fd = fd});
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
	if (fd < 0 || newfd < 0) {
		return EBADF;
	}
	return gophper_action_add(fa, (struct __gophper_spawn_action){.op = GOPHPER_ACTION_DUP2, .fd = fd, .newfd = newfd});
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict fa, int fd, const char *__restrict path, int flags, mode_t mode)
{
	char *copy = strdup(path);
	if (copy == NULL) {
		return ENOMEM;
	}
	return gophper_action_add(fa, (struct __gophper_spawn_action){.op = GOPHPER_ACTION_OPEN, .fd = fd, .path = copy, .flags = flags, .mode = mode});
}

int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *__restrict fa, const char *__restrict path)
{
	char *copy = strdup(path);
	if (copy == NULL) {
		return ENOMEM;
	}
	return gophper_action_add(fa, (struct __gophper_spawn_action){.op = GOPHPER_ACTION_CHDIR, .path = copy});
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *__restrict fa, const char *__restrict path)
{
	return posix_spawn_file_actions_addchdir(fa, path);
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
	attr->__flags = 0;
	return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
	(void) attr;
	return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
	attr->__flags = flags;
	return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict attr, short *__restrict flags)
{
	*flags = attr->__flags;
	return 0;
}

/* The highest child fd a spawn can set up. */
#define GOPHPER_SPAWN_FDS 64

static int gophper_open_flags(int flags)
{
	int out = 0;
	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		out = GOPHPER_OPEN_READ;
		break;
	case O_WRONLY:
		out = GOPHPER_OPEN_WRITE;
		break;
	default:
		out = GOPHPER_OPEN_READ | GOPHPER_OPEN_WRITE;
	}
	if (flags & O_APPEND) {
		out |= GOPHPER_OPEN_APPEND;
	}
	if (flags & O_CREAT) {
		out |= GOPHPER_OPEN_CREATE;
	}
	if (flags & O_TRUNC) {
		out |= GOPHPER_OPEN_TRUNCATE;
	}
	if (flags & O_EXCL) {
		out |= GOPHPER_OPEN_EXCLUSIVE;
	}
	return out;
}

/* gophper_describe fills d with what the parent's fd stands for. */
static int gophper_describe(int child, int fd, struct gophper_child_fd *d)
{
	int t = gophper_fd_resolve(fd);
	if (t < 0) {
		return EBADF;
	}
	memset(d, 0, sizeof(*d));
	d->child = child;
	if (gophper_socket_kind(t) != 0) {
		d->kind = GOPHPER_CHILD_HOST;
		d->value = t;
		return 0;
	}
	int flags;
	const char *path = gophper_fd_path(t, &flags);
	if (path != NULL) {
		d->kind = GOPHPER_CHILD_FILE;
		d->flags = gophper_open_flags(flags) & ~(GOPHPER_OPEN_CREATE | GOPHPER_OPEN_TRUNCATE | GOPHPER_OPEN_EXCLUSIVE);
		d->path = path;
		d->path_len = (int32_t) strlen(path);
		return 0;
	}
	if (t <= 2) {
		d->kind = GOPHPER_CHILD_STDIO;
		d->value = t;
		return 0;
	}
	return EBADF;
}

/* gophper_join writes the strings of list, each ending with a NUL. */
static char *gophper_join(char *const list[], int32_t *len)
{
	size_t n = 0;
	for (int i = 0; list != NULL && list[i] != NULL; i++) {
		n += strlen(list[i]) + 1;
	}
	char *out = malloc(n ? n : 1);
	if (out == NULL) {
		return NULL;
	}
	char *p = out;
	for (int i = 0; list != NULL && list[i] != NULL; i++) {
		size_t l = strlen(list[i]) + 1;
		memcpy(p, list[i], l);
		p += l;
	}
	*len = (int32_t) n;
	return out;
}

static int gophper_spawn(pid_t *pid, const char *path, bool search, const posix_spawn_file_actions_t *fa,
	char *const argv[], char *const envp[])
{
	/* map[i] is the parent fd that child fd i gets, or -1. */
	int map[GOPHPER_SPAWN_FDS];
	int opened[GOPHPER_SPAWN_FDS];
	int nopened = 0;
	char cwd[PATH_MAX];
	int err = 0;

	for (int i = 0; i < GOPHPER_SPAWN_FDS; i++) {
		map[i] = i <= 2 ? i : -1;
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		return errno;
	}
	for (int i = 0; fa != NULL && i < fa->__n; i++) {
		const struct __gophper_spawn_action *a = &fa->__actions[i];
		switch (a->op) {
		case GOPHPER_ACTION_CLOSE:
			if (a->fd < GOPHPER_SPAWN_FDS) {
				map[a->fd] = -1;
			}
			break;
		case GOPHPER_ACTION_DUP2: {
			if (a->newfd >= GOPHPER_SPAWN_FDS) {
				err = EBADF;
				goto out;
			}
			/* In the child, fd names what an earlier action put there. */
			int from = a->fd < GOPHPER_SPAWN_FDS && map[a->fd] >= 0 ? map[a->fd] : a->fd;
			map[a->newfd] = from;
			break;
		}
		case GOPHPER_ACTION_OPEN: {
			if (a->fd >= GOPHPER_SPAWN_FDS || nopened == GOPHPER_SPAWN_FDS) {
				err = EBADF;
				goto out;
			}
			int fd = open(a->path, a->flags, a->mode);
			if (fd < 0) {
				err = errno;
				goto out;
			}
			opened[nopened++] = fd;
			map[a->fd] = fd;
			break;
		}
		case GOPHPER_ACTION_CHDIR:
			if (a->path[0] == '/') {
				snprintf(cwd, sizeof(cwd), "%s", a->path);
			} else {
				size_t l = strlen(cwd);
				snprintf(cwd + l, sizeof(cwd) - l, "/%s", a->path);
			}
			break;
		}
	}

	struct gophper_child_fd fds[GOPHPER_SPAWN_FDS];
	int nfds = 0;
	for (int i = 0; i < GOPHPER_SPAWN_FDS; i++) {
		if (map[i] < 0) {
			continue;
		}
		err = gophper_describe(i, map[i], &fds[nfds]);
		if (err != 0) {
			goto out;
		}
		nfds++;
	}

	int32_t argv_len = 0, envp_len = 0;
	char *argv_buf = gophper_join(argv, &argv_len);
	char *envp_buf = gophper_join(envp != NULL ? envp : environ, &envp_len);
	if (argv_buf == NULL || envp_buf == NULL) {
		free(argv_buf);
		free(envp_buf);
		err = ENOMEM;
		goto out;
	}
	int32_t child = 0;
	err = host_proc_spawn(path, (int32_t) strlen(path), search, argv_buf, argv_len, envp_buf, envp_len,
		cwd, (int32_t) strlen(cwd), fds, nfds, &child);
	free(argv_buf);
	free(envp_buf);
	if (err == 0 && pid != NULL) {
		*pid = child;
	}

out:
	for (int i = 0; i < nopened; i++) {
		close(opened[i]);
	}
	return err;
}

int posix_spawn(pid_t *__restrict pid, const char *__restrict path, const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *__restrict attr, char *const argv[], char *const envp[])
{
	(void) attr;
	return gophper_spawn(pid, path, false, fa, argv, envp);
}

int posix_spawnp(pid_t *__restrict pid, const char *__restrict file, const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *__restrict attr, char *const argv[], char *const envp[])
{
	(void) attr;
	return gophper_spawn(pid, file, true, fa, argv, envp);
}

/* wait(2) and kill(2) */

pid_t waitpid(pid_t pid, int *status, int options)
{
	int32_t st = 0;
	int32_t r = host_proc_wait(pid, (options & WNOHANG) != 0, &st);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	if (r > 0 && status != NULL) {
		*status = st;
	}
	return r;
}

pid_t wait(int *status)
{
	return waitpid(-1, status, 0);
}

int kill(pid_t pid, int sig)
{
	/* The instance itself, or its process group, which is just itself. */
	if (pid == getpid() || pid == 0) {
		if (sig == 0) {
			return 0;
		}
		return raise(sig);
	}
	int err = host_proc_kill(pid, sig);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

/* popen(3) */

struct gophper_popen {
	FILE *f;
	pid_t pid;
	struct gophper_popen *next;
};

static struct gophper_popen *gophper_popens;

FILE *popen(const char *command, const char *mode)
{
	bool reading = mode[0] == 'r';
	if (!reading && mode[0] != 'w') {
		errno = EINVAL;
		return NULL;
	}
	int p[2];
	if (pipe(p) != 0) {
		return NULL;
	}
	int parent = reading ? p[0] : p[1];
	int child = reading ? p[1] : p[0];

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, child, reading ? 1 : 0);
	pid_t pid;
	int err = posix_spawn(&pid, "/bin/sh", &fa, NULL, (char *const[]) {"sh", "-c", (char *) command, NULL}, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(child);
	if (err != 0) {
		close(parent);
		errno = err;
		return NULL;
	}

	struct gophper_popen *e = malloc(sizeof(*e));
	FILE *f = e != NULL ? fdopen(parent, reading ? "r" : "w") : NULL;
	if (f == NULL) {
		free(e);
		close(parent);
		waitpid(pid, NULL, 0);
		errno = ENOMEM;
		return NULL;
	}
	*e = (struct gophper_popen){.f = f, .pid = pid, .next = gophper_popens};
	gophper_popens = e;
	return f;
}

int pclose(FILE *f)
{
	struct gophper_popen **link = &gophper_popens;
	while (*link != NULL && (*link)->f != f) {
		link = &(*link)->next;
	}
	if (*link == NULL) {
		errno = ECHILD;
		return -1;
	}
	struct gophper_popen *e = *link;
	*link = e->next;
	pid_t pid = e->pid;
	free(e);

	fclose(f);
	int status;
	pid_t r;
	do {
		r = waitpid(pid, &status, 0);
	} while (r < 0 && errno == EINTR);
	return r < 0 ? -1 : status;
}

/* exec(3). WASI cannot replace the running process. The program runs as a
 * child with the same stdio instead, and the instance then exits with its
 * status, which is what the caller of the process would see. */

static int gophper_exec(const char *path, bool search, char *const argv[], char *const envp[])
{
	pid_t pid;
	int err = gophper_spawn(&pid, path, search, NULL, argv, envp);
	if (err != 0) {
		errno = err;
		return -1;
	}
	int status;
	pid_t r;
	do {
		r = waitpid(pid, &status, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0) {
		__wasi_proc_exit(127);
	}
	__wasi_proc_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

int execve(const char *path, char *const argv[], char *const envp[]) { return gophper_exec(path, false, argv, envp); }
int execv(const char *path, char *const argv[]) { return gophper_exec(path, false, argv, environ); }
int execvp(const char *file, char *const argv[]) { return gophper_exec(file, true, argv, environ); }
