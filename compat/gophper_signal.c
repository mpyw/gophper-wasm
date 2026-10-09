/* GOPHPER: signals for wasm32-wasip1.
 *
 * WASI delivers no signals, and wasi-emulated-signal runs a handler only
 * for raise(). This file keeps the handlers, the blocked mask and the
 * pending set itself, so that sigaction(), sigprocmask() and pcntl work.
 *
 * Signals come from three places: raise() and kill() of the instance
 * itself, alarm(), and the host, which forwards what its own process
 * receives. The host never runs guest code from outside. It marks the
 * signal pending and interrupts the VM, and gophper_signal_interrupt(),
 * which patches/0008 installs as zend_interrupt_function, raises it then.
 * pcntl chains its own interrupt function in front.
 *
 * The host learns which signals have handlers through sig_watch, so that
 * it stops the run, as the default action would, for the others.
 */
#include "gophper_compat.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wasi/api.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* sig_watch tells the host which signals have a handler and which are
 * ignored, as bit sets of signal numbers. sig_take returns the signals the
 * host has marked pending, and forgets them. sig_wait blocks until one in
 * mask is pending, and returns it, or 0 on timeout, or -errno. alarm
 * replaces the alarm, as alarm(2) does. See ABI.md. */
GOPHPER_IMPORT("sig_watch") void host_sig_watch(int64_t handled, int64_t ignored);
GOPHPER_IMPORT("sig_take") int64_t host_sig_take(void);
GOPHPER_IMPORT("sig_wait") int32_t host_sig_wait(int64_t mask, int32_t timeout_ms);
GOPHPER_IMPORT("alarm") int32_t host_alarm(int32_t seconds);

/* Signals 1 to 63 fit in one 64-bit set: bit n is signal n. */
#define GOPHPER_SIGS 64

static struct sigaction gophper_actions[GOPHPER_SIGS];
static uint64_t gophper_blocked;
static uint64_t gophper_pending;

static bool gophper_sig_valid(int sig)
{
	return sig > 0 && sig < GOPHPER_SIGS;
}

static uint64_t gophper_bit(int sig)
{
	return (uint64_t) 1 << sig;
}

/* sigset_t holds bit n-1 for signal n, as musl does. */
static uint64_t gophper_from_set(const sigset_t *set)
{
	uint64_t out = 0;
	for (int sig = 1; sig < GOPHPER_SIGS; sig++) {
		if (sigismember(set, sig) == 1) {
			out |= gophper_bit(sig);
		}
	}
	return out;
}

static void gophper_to_set(uint64_t bits, sigset_t *set)
{
	sigemptyset(set);
	for (int sig = 1; sig < GOPHPER_SIGS; sig++) {
		if (bits & gophper_bit(sig)) {
			sigaddset(set, sig);
		}
	}
}

static void gophper_sig_tell_host(void)
{
	uint64_t handled = 0, ignored = 0;
	for (int sig = 1; sig < GOPHPER_SIGS; sig++) {
		void (*h)(int) = gophper_actions[sig].sa_handler;
		if (h == SIG_IGN) {
			ignored |= gophper_bit(sig);
		} else if (h != SIG_DFL) {
			handled |= gophper_bit(sig);
		}
	}
	host_sig_watch((int64_t) handled, (int64_t) ignored);
}

/* Signals whose default action is to do nothing. */
static bool gophper_default_ignores(int sig)
{
	return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
}

static void gophper_deliver_pending(void);

static void gophper_deliver(int sig)
{
	struct sigaction *a = &gophper_actions[sig];
	void (*h)(int) = a->sa_handler;
	if (h == SIG_IGN || (h == SIG_DFL && gophper_default_ignores(sig))) {
		return;
	}
	if (h == SIG_DFL) {
		/* Terminated by the signal, as a shell reports it. */
		__wasi_proc_exit(128 + sig);
	}

	uint64_t saved = gophper_blocked;
	gophper_blocked |= gophper_from_set(&a->sa_mask);
	if (!(a->sa_flags & SA_NODEFER)) {
		gophper_blocked |= gophper_bit(sig);
	}
	if (a->sa_flags & SA_SIGINFO) {
		siginfo_t info = {.si_signo = sig, .si_code = SI_USER, .si_pid = getpid(), .si_uid = getuid()};
		void (*action)(int, siginfo_t *, void *) = a->sa_sigaction;
		if (a->sa_flags & SA_RESETHAND) {
			a->sa_handler = SIG_DFL;
			gophper_sig_tell_host();
		}
		action(sig, &info, NULL);
	} else {
		if (a->sa_flags & SA_RESETHAND) {
			a->sa_handler = SIG_DFL;
			gophper_sig_tell_host();
		}
		h(sig);
	}
	gophper_blocked = saved;
	gophper_deliver_pending();
}

/* gophper_deliver_pending delivers the pending signals no longer blocked. */
static void gophper_deliver_pending(void)
{
	for (int sig = 1; sig < GOPHPER_SIGS; sig++) {
		uint64_t bit = gophper_bit(sig);
		if ((gophper_pending & bit) && !(gophper_blocked & bit)) {
			gophper_pending &= ~bit;
			gophper_deliver(sig);
		}
	}
}

/* gophper_signal_poll takes what the host has pending. */
static void gophper_signal_poll(void)
{
	uint64_t taken = (uint64_t) host_sig_take();
	if (taken) {
		gophper_pending |= taken;
		gophper_deliver_pending();
	}
}

/* zend_interrupt_function, installed by patches/0008. The host interrupts
 * the VM when it marks a signal pending.
 *
 * pcntl puts its own function in front in MINIT, and calls this one after
 * it dispatches. A signal raised here would then wait for the next check.
 * So sigaction(), which pcntl_signal() calls, puts this one in front again,
 * and the call back from pcntl is skipped. */
extern void (*zend_interrupt_function)(void *execute_data);
static void (*gophper_next_interrupt)(void *execute_data);
static bool gophper_in_interrupt;

void gophper_signal_interrupt(void *execute_data);
void gophper_signal_interrupt(void *execute_data)
{
	if (gophper_in_interrupt) {
		return;
	}
	gophper_in_interrupt = true;
	gophper_signal_poll();
	if (gophper_next_interrupt) {
		gophper_next_interrupt(execute_data);
	}
	gophper_in_interrupt = false;
}

static void gophper_interrupt_first(void)
{
	if (zend_interrupt_function != gophper_signal_interrupt) {
		gophper_next_interrupt = zend_interrupt_function;
		zend_interrupt_function = gophper_signal_interrupt;
	}
}

int __wrap_raise(int sig)
{
	if (!gophper_sig_valid(sig)) {
		errno = EINVAL;
		return -1;
	}
	gophper_pending |= gophper_bit(sig);
	gophper_deliver_pending();
	return 0;
}

int sigaction(int sig, const struct sigaction *__restrict act, struct sigaction *__restrict oact)
{
	if (!gophper_sig_valid(sig) || (act && (sig == SIGKILL || sig == SIGSTOP))) {
		errno = EINVAL;
		return -1;
	}
	if (oact) {
		*oact = gophper_actions[sig];
	}
	if (act) {
		gophper_actions[sig] = *act;
		gophper_sig_tell_host();
		gophper_interrupt_first();
	}
	return 0;
}

int sigemptyset(sigset_t *set) { memset(set, 0, sizeof(*set)); return 0; }
int sigfillset(sigset_t *set) { memset(set, 0xff, sizeof(*set)); return 0; }

static int gophper_set_check(int sig)
{
	if (sig <= 0 || (size_t) sig > sizeof(sigset_t) * 8) {
		errno = EINVAL;
		return 0;
	}
	return 1;
}

int sigaddset(sigset_t *set, int sig)
{
	if (!gophper_set_check(sig)) return -1;
	((unsigned char *) set)[(sig - 1) / 8] |= 1u << ((sig - 1) % 8);
	return 0;
}

int sigdelset(sigset_t *set, int sig)
{
	if (!gophper_set_check(sig)) return -1;
	((unsigned char *) set)[(sig - 1) / 8] &= ~(1u << ((sig - 1) % 8));
	return 0;
}

int sigismember(const sigset_t *set, int sig)
{
	if (!gophper_set_check(sig)) return -1;
	return (((const unsigned char *) set)[(sig - 1) / 8] >> ((sig - 1) % 8)) & 1;
}

int sigprocmask(int how, const sigset_t *__restrict set, sigset_t *__restrict oset)
{
	if (oset) {
		gophper_to_set(gophper_blocked, oset);
	}
	if (set) {
		uint64_t bits = gophper_from_set(set) & ~(gophper_bit(SIGKILL) | gophper_bit(SIGSTOP));
		switch (how) {
		case SIG_BLOCK:
			gophper_blocked |= bits;
			break;
		case SIG_UNBLOCK:
			gophper_blocked &= ~bits;
			break;
		case SIG_SETMASK:
			gophper_blocked = bits;
			break;
		default:
			errno = EINVAL;
			return -1;
		}
		gophper_signal_poll();
		gophper_deliver_pending();
	}
	return 0;
}

int pthread_sigmask(int how, const sigset_t *__restrict set, sigset_t *__restrict oset)
{
	return sigprocmask(how, set, oset) == 0 ? 0 : errno;
}

int sigpending(sigset_t *set)
{
	gophper_pending |= (uint64_t) host_sig_take();
	gophper_to_set(gophper_pending & gophper_blocked, set);
	return 0;
}

/* gophper_sigwait waits up to timeout_ms (-1: forever) for a signal in set,
 * and takes it without running its handler. */
static int gophper_sigwait(const sigset_t *set, int timeout_ms)
{
	uint64_t mask = gophper_from_set(set);
	for (;;) {
		gophper_pending |= (uint64_t) host_sig_take();
		for (int sig = 1; sig < GOPHPER_SIGS; sig++) {
			if (gophper_pending & mask & gophper_bit(sig)) {
				gophper_pending &= ~gophper_bit(sig);
				return sig;
			}
		}
		int32_t r = host_sig_wait((int64_t) mask, timeout_ms);
		if (r < 0) {
			errno = -r;
			return -1;
		}
		if (r == 0) {
			errno = EAGAIN;
			return -1;
		}
		/* The host has it pending now. Take it on the next pass. */
		timeout_ms = 0;
	}
}

int sigwait(const sigset_t *__restrict set, int *__restrict sig)
{
	int r = gophper_sigwait(set, -1);
	if (r < 0) {
		return errno;
	}
	*sig = r;
	return 0;
}

int sigwaitinfo(const sigset_t *__restrict set, siginfo_t *__restrict info)
{
	int r = gophper_sigwait(set, -1);
	if (r > 0 && info) {
		*info = (siginfo_t){.si_signo = r, .si_code = SI_USER};
	}
	return r;
}

int sigtimedwait(const sigset_t *__restrict set, siginfo_t *__restrict info, const struct timespec *__restrict timeout)
{
	int ms = -1;
	if (timeout) {
		long long t = (long long) timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
		ms = t > INT32_MAX ? INT32_MAX : (int) t;
	}
	int r = gophper_sigwait(set, ms);
	if (r > 0 && info) {
		*info = (siginfo_t){.si_signo = r, .si_code = SI_USER};
	}
	return r;
}

int sigqueue(pid_t pid, int sig, union sigval value)
{
	(void) value;
	return kill(pid, sig);
}

unsigned alarm(unsigned seconds)
{
	int32_t left = host_alarm(seconds > INT32_MAX ? INT32_MAX : (int32_t) seconds);
	return left < 0 ? 0 : (unsigned) left;
}

int pause(void)
{
	sigset_t all;
	sigfillset(&all);
	int sig = gophper_sigwait(&all, -1);
	if (sig > 0) {
		gophper_pending |= gophper_bit(sig);
		gophper_deliver_pending();
	}
	errno = EINTR;
	return -1;
}
