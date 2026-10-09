/* GOPHPER: dlopen(3) for wasm extensions, backed by the Go host's dynamic
 * linker. dlopen reads the file here, so the host needs no path mapping, and
 * hands the bytes over. A handle is the host's id for the loaded module.
 *
 * dlsym returns what C expects: for a function, its index in the shared
 * function table; for data, its address in linear memory. */
#include "gophper_compat.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* Return 0 and a message through dl_error on failure. */
GOPHPER_IMPORT("dl_open") int32_t host_dl_open(const void *wasm, int32_t len);
GOPHPER_IMPORT("dl_sym") int32_t host_dl_sym(int32_t handle, const char *name, int32_t name_len, uint32_t *value);
GOPHPER_IMPORT("dl_close") int32_t host_dl_close(int32_t handle);
GOPHPER_IMPORT("dl_error") int32_t host_dl_error(char *buf, int32_t cap);

static char gophper_dl_message[512];
static int gophper_dl_failed;

static void *gophper_dl_fail(const char *fmt, const char *arg)
{
	snprintf(gophper_dl_message, sizeof(gophper_dl_message), fmt, arg);
	gophper_dl_failed = 1;
	return NULL;
}

static void *gophper_dl_host_fail(void)
{
	int32_t n = host_dl_error(gophper_dl_message, sizeof(gophper_dl_message) - 1);
	gophper_dl_message[n > 0 ? n : 0] = '\0';
	gophper_dl_failed = 1;
	return NULL;
}

void *dlopen(const char *path, int mode)
{
	(void) mode;
	if (path == NULL) {
		return gophper_dl_fail("dlopen(NULL) is not supported%s", "");
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return gophper_dl_fail("%s: cannot open shared object file", path);
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return gophper_dl_fail("%s: cannot read shared object file", path);
	}
	unsigned char *buf = malloc(st.st_size);
	if (buf == NULL) {
		close(fd);
		return gophper_dl_fail("%s: out of memory", path);
	}
	off_t done = 0;
	while (done < st.st_size) {
		ssize_t n = read(fd, buf + done, st.st_size - done);
		if (n <= 0) {
			free(buf);
			close(fd);
			return gophper_dl_fail("%s: cannot read shared object file", path);
		}
		done += n;
	}
	close(fd);
	int32_t handle = host_dl_open(buf, (int32_t) st.st_size);
	free(buf);
	if (handle == 0) {
		return gophper_dl_host_fail();
	}
	return (void *) (uintptr_t) handle;
}

void *dlsym(void *__restrict handle, const char *__restrict name)
{
	uint32_t value = 0;
	if (!host_dl_sym((int32_t) (uintptr_t) handle, name, (int32_t) strlen(name), &value)) {
		return gophper_dl_host_fail();
	}
	return (void *) (uintptr_t) value;
}

int dlclose(void *handle)
{
	return host_dl_close((int32_t) (uintptr_t) handle) ? 0 : -1;
}

char *dlerror(void)
{
	if (!gophper_dl_failed) {
		return NULL;
	}
	gophper_dl_failed = 0;
	return gophper_dl_message;
}
