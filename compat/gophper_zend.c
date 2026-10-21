/* GOPHPER: lets the Go host stop a running script.
 *
 * On Linux, max_execution_time works through a signal: the handler sets
 * EG(timed_out) and EG(vm_interrupt), and the VM raises the fatal error at
 * its next interrupt check. WASI has no signals. patches/0004 hands the timer
 * to the Go host, which writes the same flags into linear memory when it
 * fires. This file publishes their addresses.
 *
 * It is compiled against the configured php-src (it needs php.h) and linked
 * into php and php-cgi. Each pointer is exported with -Wl,--export. */
#include "php.h"

#define GOPHPER_EXPORT __attribute__((visibility("default"), used))

GOPHPER_EXPORT zend_atomic_bool *gophper_vm_interrupt = &EG(vm_interrupt);
GOPHPER_EXPORT zend_atomic_bool *gophper_timed_out = &EG(timed_out);
/* The extra time shutdown functions get after a timeout (hard_timeout). */
GOPHPER_EXPORT zend_long *gophper_hard_timeout = &EG(hard_timeout);
