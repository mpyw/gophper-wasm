/* GOPHPER: posix_spawn(3) for wasm32-wasip1. compat/gophper_proc.c runs the
 * child on the host. Only file actions are kept: attributes are accepted
 * and ignored. */
#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSIX_SPAWN_RESETIDS 1
#define POSIX_SPAWN_SETPGROUP 2
#define POSIX_SPAWN_SETSIGDEF 4
#define POSIX_SPAWN_SETSIGMASK 8
#define POSIX_SPAWN_SETSCHEDPARAM 16
#define POSIX_SPAWN_SETSCHEDULER 32
#define POSIX_SPAWN_USEVFORK 64
#define POSIX_SPAWN_SETSID 128

typedef struct {
	short __flags;
} posix_spawnattr_t;

struct __gophper_spawn_action;

typedef struct {
	int __n;
	int __cap;
	struct __gophper_spawn_action *__actions;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *__restrict, const char *__restrict, const posix_spawn_file_actions_t *,
	const posix_spawnattr_t *__restrict, char *const[], char *const[]);
int posix_spawnp(pid_t *__restrict, const char *__restrict, const posix_spawn_file_actions_t *,
	const posix_spawnattr_t *__restrict, char *const[], char *const[]);

int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict, short *__restrict);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict, int, const char *__restrict, int, mode_t);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *__restrict, const char *__restrict);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *__restrict, const char *__restrict);

#ifdef __cplusplus
}
#endif

#endif
