/* GOPHPER: WASI has no wait(2). compat/gophper_proc.c waits on the host. */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <stdlib.h> /* W* macros (via gophper_compat.h on WASI) */

#ifdef __cplusplus
extern "C" {
#endif

pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);

#ifdef __cplusplus
}
#endif

#endif
