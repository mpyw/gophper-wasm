/* GOPHPER: wasi-libc's <sys/resource.h> has getrusage only. This adds
 * resource limits, which compat/gophper_compat.c keeps. */
#ifndef _GOPHPER_SYS_RESOURCE_H
#define _GOPHPER_SYS_RESOURCE_H

#include_next <sys/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long rlim_t;

struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);

#define RLIM_INFINITY (~0ULL)
#define RLIM_SAVED_CUR RLIM_INFINITY
#define RLIM_SAVED_MAX RLIM_INFINITY

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9
#define RLIMIT_LOCKS 10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE 13
#define RLIMIT_RTPRIO 14
#define RLIMIT_RTTIME 15
#define RLIMIT_NLIMITS 16
#define RLIM_NLIMITS RLIMIT_NLIMITS

#ifdef __cplusplus
}
#endif

#endif
