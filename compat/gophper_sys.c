/* GOPHPER: users, groups, file metadata, locks and the host name, from the host.
 *
 * WASI has none of these. The host answers with its own: the uid gophper
 * runs as, its user database, flock(2) on the host file, and its name.
 * Scripts then see what native PHP would, such as the user Composer warns
 * about when it is root.
 */
#include "gophper_compat.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* What user_get looks up. Shared with gophper's internal/hostsys. See ABI.md. */
enum {
	GOPHPER_USER_BY_ID = 1,
	GOPHPER_USER_BY_NAME = 2,
	GOPHPER_GROUP_BY_ID = 3,
	GOPHPER_GROUP_BY_NAME = 4,
};

/* user_get returns the length of the entry, more than cap to ask for more
 * room, or -errno. user_ids writes uid, gid and then up to cap groups, and
 * returns the number of groups. */
GOPHPER_IMPORT("user_ids") int32_t host_user_ids(int32_t *ids, int32_t cap);
GOPHPER_IMPORT("user_get") int32_t host_user_get(int32_t kind, int32_t id, const char *name, int32_t name_len, char *out, int32_t cap);
GOPHPER_IMPORT("file_lock") int32_t host_file_lock(int32_t fd, const char *path, int32_t path_len, int32_t op);
GOPHPER_IMPORT("file_unlock") int32_t host_file_unlock(int32_t fd);
GOPHPER_IMPORT("host_name") int32_t host_host_name(char *out, int32_t cap);
GOPHPER_IMPORT("path_stat") int32_t host_path_stat(const char *path, int32_t path_len, int32_t follow, uint32_t *out);
GOPHPER_IMPORT("path_access") int32_t host_path_access(const char *path, int32_t path_len, int32_t mode);
GOPHPER_IMPORT("path_chmod") int32_t host_path_chmod(const char *path, int32_t path_len, int32_t mode);
GOPHPER_IMPORT("path_chown") int32_t host_path_chown(const char *path, int32_t path_len, int32_t uid, int32_t gid, int32_t follow);

int gophper_fd_resolve(int fd);
const char *gophper_fd_path(int fd, int *flags);
int __real_uname(struct utsname *);
int __real_stat(const char *__restrict, struct stat *__restrict);
int __real_lstat(const char *__restrict, struct stat *__restrict);
int __real_access(const char *, int);

/* Users and groups */

#define GOPHPER_GROUPS_MAX 64

static bool gophper_ids_loaded;
static int32_t gophper_ids[2 + GOPHPER_GROUPS_MAX];
static int gophper_ngroups;

static void gophper_load_ids(void)
{
	if (!gophper_ids_loaded) {
		int32_t n = host_user_ids(gophper_ids, GOPHPER_GROUPS_MAX);
		gophper_ngroups = n < 0 ? 0 : n > GOPHPER_GROUPS_MAX ? GOPHPER_GROUPS_MAX : n;
		gophper_ids_loaded = true;
	}
}

uid_t getuid(void) { gophper_load_ids(); return (uid_t) gophper_ids[0]; }
uid_t geteuid(void) { return getuid(); }
gid_t getgid(void) { gophper_load_ids(); return (gid_t) gophper_ids[1]; }
gid_t getegid(void) { return getgid(); }

int getgroups(int size, gid_t list[])
{
	gophper_load_ids();
	if (size == 0) {
		return gophper_ngroups;
	}
	if (size < gophper_ngroups) {
		errno = EINVAL;
		return -1;
	}
	for (int i = 0; i < gophper_ngroups; i++) {
		list[i] = (gid_t) gophper_ids[2 + i];
	}
	return gophper_ngroups;
}

/* gophper_lookup asks the host for an entry: NUL-separated fields, in
 * buf. It returns the number of bytes, or -errno. */
static int gophper_lookup(int kind, int32_t id, const char *name, char *buf, size_t len)
{
	int32_t n = host_user_get(kind, id, name, name ? (int32_t) strlen(name) : 0, buf, (int32_t) len);
	if (n < 0) {
		return n;
	}
	return (size_t) n > len ? -ERANGE : n;
}

/* gophper_fields splits buf into count NUL-terminated fields. */
static bool gophper_fields(char *buf, int n, char **fields, int count)
{
	char *p = buf, *end = buf + n;
	for (int i = 0; i < count; i++) {
		if (p >= end) {
			return false;
		}
		fields[i] = p;
		p += strlen(p) + 1;
	}
	return true;
}

static int gophper_passwd(int kind, uid_t uid, const char *name, struct passwd *pw, char *buf, size_t len, struct passwd **result)
{
	*result = NULL;
	int n = gophper_lookup(kind, (int32_t) uid, name, buf, len);
	if (n < 0) {
		/* Not found is no error, as with getpwuid_r(3). */
		return n == -ENOENT ? 0 : -n;
	}
	char *f[7];
	if (!gophper_fields(buf, n, f, 7)) {
		return EIO;
	}
	*pw = (struct passwd){
		.pw_name = f[0],
		.pw_passwd = f[1],
		.pw_uid = (uid_t) strtoul(f[2], NULL, 10),
		.pw_gid = (gid_t) strtoul(f[3], NULL, 10),
		.pw_gecos = f[4],
		.pw_dir = f[5],
		.pw_shell = f[6],
	};
	*result = pw;
	return 0;
}

static int gophper_group(int kind, gid_t gid, const char *name, struct group *gr, char *buf, size_t len, struct group **result)
{
	*result = NULL;
	/* The members are written after the entry, as a NULL-terminated array. */
	size_t room = len / 2;
	int n = gophper_lookup(kind, (int32_t) gid, name, buf, room);
	if (n < 0) {
		return n == -ENOENT ? 0 : -n;
	}
	char *f[4];
	if (!gophper_fields(buf, n, f, 4)) {
		return EIO;
	}
	size_t count = 0;
	for (char *p = f[3]; *p; p++) {
		count += *p == ',';
	}
	count += f[3][0] != '\0';
	uintptr_t at = ((uintptr_t) (buf + n) + sizeof(char *) - 1) & ~(uintptr_t) (sizeof(char *) - 1);
	char **mem = (char **) at;
	if ((char *) (mem + count + 1) > buf + len) {
		return ERANGE;
	}
	size_t i = 0;
	for (char *p = f[3]; f[3][0] != '\0' && p != NULL; i++) {
		mem[i] = p;
		p = strchr(p, ',');
		if (p != NULL) {
			*p++ = '\0';
		}
	}
	mem[i] = NULL;
	*gr = (struct group){
		.gr_name = f[0],
		.gr_passwd = f[1],
		.gr_gid = (gid_t) strtoul(f[2], NULL, 10),
		.gr_mem = mem,
	};
	*result = gr;
	return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t len, struct passwd **result)
{
	return gophper_passwd(GOPHPER_USER_BY_ID, uid, NULL, pw, buf, len, result);
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t len, struct passwd **result)
{
	return gophper_passwd(GOPHPER_USER_BY_NAME, 0, name, pw, buf, len, result);
}

int getgrgid_r(gid_t gid, struct group *gr, char *buf, size_t len, struct group **result)
{
	return gophper_group(GOPHPER_GROUP_BY_ID, gid, NULL, gr, buf, len, result);
}

int getgrnam_r(const char *name, struct group *gr, char *buf, size_t len, struct group **result)
{
	return gophper_group(GOPHPER_GROUP_BY_NAME, 0, name, gr, buf, len, result);
}

static struct passwd gophper_pw;
static char gophper_pw_buf[4096];
static struct group gophper_gr;
static char gophper_gr_buf[16384];

struct passwd *getpwuid(uid_t uid)
{
	struct passwd *r;
	errno = getpwuid_r(uid, &gophper_pw, gophper_pw_buf, sizeof(gophper_pw_buf), &r);
	return r;
}

struct passwd *getpwnam(const char *name)
{
	struct passwd *r;
	errno = getpwnam_r(name, &gophper_pw, gophper_pw_buf, sizeof(gophper_pw_buf), &r);
	return r;
}

struct group *getgrgid(gid_t gid)
{
	struct group *r;
	errno = getgrgid_r(gid, &gophper_gr, gophper_gr_buf, sizeof(gophper_gr_buf), &r);
	return r;
}

struct group *getgrnam(const char *name)
{
	struct group *r;
	errno = getgrnam_r(name, &gophper_gr, gophper_gr_buf, sizeof(gophper_gr_buf), &r);
	return r;
}

char *getlogin(void)
{
	struct passwd *pw = getpwuid(getuid());
	return pw ? pw->pw_name : NULL;
}

/* flock(2) */

/* Which fds hold a host lock, so that close() releases it. */
static unsigned char gophper_locked[1024 / 8];

int flock(int fd, int op)
{
	int t = gophper_fd_resolve(fd);
	if (t < 0) {
		return -1;
	}
	int flags;
	const char *path = gophper_fd_path(t, &flags);
	if (path == NULL) {
		/* Not a file from open(), such as a socket. */
		errno = EINVAL;
		return -1;
	}
	int err = host_file_lock(t, path, (int32_t) strlen(path), op);
	if (err != 0) {
		errno = err;
		return -1;
	}
	if (t < 1024) {
		if (op & LOCK_UN) {
			gophper_locked[t / 8] &= (unsigned char) ~(1u << (t % 8));
		} else {
			gophper_locked[t / 8] |= (unsigned char) (1u << (t % 8));
		}
	}
	return 0;
}

/* gophper_lock_release drops the host lock of fd, which is being closed. */
void gophper_lock_release(int fd)
{
	if (fd >= 0 && fd < 1024 && (gophper_locked[fd / 8] & (1u << (fd % 8)))) {
		gophper_locked[fd / 8] &= (unsigned char) ~(1u << (fd % 8));
		host_file_unlock(fd);
	}
}

/* The host name */

int __wrap_gethostname(char *name, size_t len)
{
	char buf[256];
	int32_t n = host_host_name(buf, sizeof(buf) - 1);
	if (n < 0) {
		errno = -n;
		return -1;
	}
	buf[n] = '\0';
	if ((size_t) n >= len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(name, buf, (size_t) n + 1);
	return 0;
}

int __wrap_uname(struct utsname *u)
{
	if (__real_uname(u) != 0) {
		return -1;
	}
	char buf[256];
	if (__wrap_gethostname(buf, sizeof(buf)) == 0) {
		strncpy(u->nodename, buf, sizeof(u->nodename) - 1);
		u->nodename[sizeof(u->nodename) - 1] = '\0';
	}
	return 0;
}

/* File metadata. WASI reports no permissions or owners, and cannot change
 * them. The host does, on the file behind the path. A path with no host
 * file, such as the host's mounts of php.ini, keeps what WASI reports. */

/* gophper_abs makes path absolute, against the working directory. */
static const char *gophper_abs(const char *path, char *buf, size_t len)
{
	if (path[0] == '/') {
		return path;
	}
	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		return path;
	}
	snprintf(buf, len, "%s/%s", cwd, path);
	return buf;
}

/* gophper_stat_fill adds the host's permissions and owner to st. */
void gophper_stat_fill(const char *path, struct stat *st, int follow)
{
	char buf[PATH_MAX];
	const char *abs = gophper_abs(path, buf, sizeof(buf));
	uint32_t out[3];
	if (host_path_stat(abs, (int32_t) strlen(abs), follow, out) != 0) {
		return;
	}
	st->st_mode = (st->st_mode & S_IFMT) | (mode_t) out[0];
	st->st_uid = (uid_t) out[1];
	st->st_gid = (gid_t) out[2];
}

int __wrap_stat(const char *__restrict path, struct stat *__restrict st)
{
	if (__real_stat(path, st) != 0) {
		return -1;
	}
	gophper_stat_fill(path, st, 1);
	return 0;
}

int __wrap_lstat(const char *__restrict path, struct stat *__restrict st)
{
	if (__real_lstat(path, st) != 0) {
		return -1;
	}
	gophper_stat_fill(path, st, 0);
	return 0;
}

int __wrap_access(const char *path, int mode)
{
	if (__real_access(path, F_OK) != 0) {
		return -1;
	}
	if (mode == F_OK) {
		return 0;
	}
	char buf[PATH_MAX];
	const char *abs = gophper_abs(path, buf, sizeof(buf));
	int err = host_path_access(abs, (int32_t) strlen(abs), mode);
	if (err == ENOENT) {
		/* No host file: WASI's answer stands. */
		return __real_access(path, mode);
	}
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

static int gophper_result(int err)
{
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

int __wrap_chmod(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	const char *abs = gophper_abs(path, buf, sizeof(buf));
	return gophper_result(host_path_chmod(abs, (int32_t) strlen(abs), (int32_t) mode));
}

int __wrap_fchmod(int fd, mode_t mode)
{
	int flags;
	const char *path = gophper_fd_path(fd, &flags);
	if (path == NULL) {
		errno = EBADF;
		return -1;
	}
	return gophper_result(host_path_chmod(path, (int32_t) strlen(path), (int32_t) mode));
}

static int gophper_chown(const char *path, uid_t owner, gid_t group, int follow)
{
	char buf[PATH_MAX];
	const char *abs = gophper_abs(path, buf, sizeof(buf));
	return gophper_result(host_path_chown(abs, (int32_t) strlen(abs), (int32_t) owner, (int32_t) group, follow));
}

int chown(const char *path, uid_t owner, gid_t group) { return gophper_chown(path, owner, group, 1); }
int lchown(const char *path, uid_t owner, gid_t group) { return gophper_chown(path, owner, group, 0); }

int fchown(int fd, uid_t owner, gid_t group)
{
	int flags;
	const char *path = gophper_fd_path(fd, &flags);
	if (path == NULL) {
		errno = EBADF;
		return -1;
	}
	return gophper_chown(path, owner, group, 1);
}
