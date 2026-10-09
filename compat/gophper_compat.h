/* GOPHPER: force-included into every php-src translation unit (-include).
 * Declares POSIX APIs that wasi-libc hides on wasm32-wasip1. They are
 * implemented as failing stubs in gophper_compat.c. */
#ifndef GOPHPER_COMPAT_H
#define GOPHPER_COMPAT_H

#ifdef __wasi__

/* wasi-libc's sigset_t is one byte, a placeholder that holds signals 1 to
 * 8 only. This is musl's, before any header defines the placeholder.
 * compat/gophper_signal.c implements every function that takes one. */
#ifndef __wasilibc___typedef_sigset_t_h
#define __wasilibc___typedef_sigset_t_h
typedef struct __sigset_t {
	unsigned long __bits[128 / sizeof(long)];
} sigset_t;
#endif

#include <sys/types.h>
#include <stdio.h>
#include <sys/socket.h>
#include <signal.h>

/* php-src includes these only when configure saw them (HAVE_*_H), which it
 * does not, since compat/include is not used during configure. */
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <sys/wait.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LLVM's wasm sjlj lowering only understands setjmp()/longjmp(). wasi-libc
 * declares sigsetjmp()/siglongjmp() but does not implement them, so map them
 * here. (WASI has no signal masks to save anyway.) Defined before <setjmp.h>,
 * so its prototypes turn into compatible redeclarations of setjmp/longjmp. */
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)

/* Users and groups: everything runs as uid/gid 0. */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int, gid_t[]);
int chown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int lchown(const char *, uid_t, gid_t);
mode_t umask(mode_t);

/* Time: wasi-libc's C library time is always UTC. PHP keeps its own tz database. */
void tzset(void);

/* Processes. */
pid_t getppid(void);
int getdtablesize(void);
FILE *popen(const char *, const char *);
FILE *tmpfile(void);
int pclose(FILE *);
int mkstemp(char *);
char *mktemp(char *);
pid_t fork(void);
int kill(pid_t, int);
int setuid(uid_t);
int setgid(gid_t);
int initgroups(const char *, gid_t);

pid_t getpgrp(void);
pid_t setsid(void);
int setpgid(pid_t, pid_t);
pid_t getpgid(pid_t);
pid_t getsid(pid_t);
int seteuid(uid_t);
int setegid(gid_t);
int nice(int);
int getloadavg(double[], int);
char *ctermid(char *);
int mkfifo(const char *, mode_t);
int mknod(const char *, mode_t, dev_t);
char *ttyname(int);

/* Signals: wasi-emulated-signal only has signal() and raise(), and
 * <signal.h> leaves out siginfo_t and struct sigaction. compat/gophper_signal.c
 * keeps its own handlers, masks and pending set, and the host delivers
 * signals it receives. The fields follow musl's names. */
union sigval {
	int sival_int;
	void *sival_ptr;
};
typedef struct {
	int si_signo, si_errno, si_code;
	pid_t si_pid;
	uid_t si_uid;
	int si_status;
	void *si_addr;
	long si_band;
	int si_fd;
	union sigval si_value;
} siginfo_t;
struct sigaction {
	union {
		void (*sa_handler)(int);
		void (*sa_sigaction)(int, siginfo_t *, void *);
	} __sa_handler;
	sigset_t sa_mask;
	int sa_flags;
};
#define sa_handler __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction
#ifndef SI_USER
#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_QUEUE (-1)
#define SI_TIMER (-2)
#define SI_MESGQ (-3)
#define SI_ASYNCIO (-4)
#define SI_SIGIO (-5)
#define SI_TKILL (-6)
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
#endif
#ifndef SA_RESTART
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#endif
int sigaction(int, const struct sigaction *__restrict, struct sigaction *__restrict);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *__restrict, sigset_t *__restrict);
int pthread_sigmask(int, const sigset_t *__restrict, sigset_t *__restrict);
int sigpending(sigset_t *);
int sigwait(const sigset_t *__restrict, int *__restrict);
int sigwaitinfo(const sigset_t *__restrict, siginfo_t *__restrict);
int sigtimedwait(const sigset_t *__restrict, siginfo_t *__restrict, const struct timespec *__restrict);
int sigqueue(pid_t, int, union sigval);
unsigned alarm(unsigned);
/* exec*: compat/gophper_proc.c runs the program as a child and exits with
 * its status. WASI cannot replace a process. */
int execve(const char *, char *const[], char *const[]);
int execv(const char *, char *const[]);
int execvp(const char *, char *const[]);
int pause(void);
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#ifndef WEXITSTATUS
#define WNOHANG    1
#define WUNTRACED  2
#define WEXITSTATUS(s) (((s) & 0xff00) >> 8)
#define WTERMSIG(s) ((s) & 0x7f)
#define WSTOPSIG(s) WEXITSTATUS(s)
#define WIFEXITED(s) (!WTERMSIG(s))
#define WIFSTOPPED(s) ((short)((((s)&0xffff)*0x10001U)>>8) > 0x7f00)
#define WIFSIGNALED(s) (((s)&0xffff)-1U < 0xffu)
#endif

#ifdef __wasip1__
/* Sockets: wasip1 only has accept/recv/send/shutdown on preopened fds. */
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

#ifndef h_errno
extern int h_errno;
#endif
#endif

/* Socket constants that wasip1 leaves out (values follow wasip2). */
#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif
#ifndef SOL_IP
#define SOL_IP 0
#define SOL_TCP 6
#define SOL_UDP 17
#define SOL_IPV6 41
#endif
/* errno values wasi-libc does not define. WASI never returns them, so any
 * value outside its range works; code may switch on them beside the rest. */
#ifndef EHOSTDOWN
#define EHOSTDOWN 0x10000
#endif

#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST 6
#endif
#ifndef SO_LINGER
#define SO_LINGER 13
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 66
#define SO_SNDTIMEO 67
#endif
#ifndef MSG_OOB
#define MSG_OOB 0x0001
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x0040
#endif

#ifdef __cplusplus
}
#endif

#endif /* __wasi__ */

#endif /* GOPHPER_COMPAT_H */
