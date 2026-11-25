/* GOPHPER: res_search(3) for wasm32-wasip1, so that dns_get_record(),
 * checkdnsrr() and getmxrr() work.
 *
 * The host sends the query and returns the raw answer, which PHP parses
 * with dn_expand() and the ns_get*() helpers below. These follow musl.
 */
#include "gophper_compat.h"

#include <arpa/nameser.h>
#include <errno.h>
#include <netdb.h>
#include <resolv.h>
#include <stdint.h>
#include <string.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

/* Returns the answer's length, or -h_errno. */
GOPHPER_IMPORT("dns_query") int32_t host_dns_query(const char *name, int32_t name_len, int32_t class, int32_t type, unsigned char *out, int32_t cap);

int res_init(void) { return 0; }

int res_search(const char *name, int class, int type, unsigned char *answer, int len)
{
	int32_t n = host_dns_query(name, (int32_t) strlen(name), class, type, answer, len);
	if (n < 0) {
		h_errno = -n;
		return -1;
	}
	return n;
}

int res_query(const char *name, int class, int type, unsigned char *answer, int len)
{
	return res_search(name, class, type, answer, len);
}

int dn_expand(const unsigned char *base, const unsigned char *end, const unsigned char *src, char *dest, int space)
{
	const unsigned char *p = src;
	char *dbegin = dest;
	char *dend = dest + (space > 254 ? 254 : space);
	int len = -1;

	if (p == end || space <= 0) {
		return -1;
	}
	/* Detect reference loops with a step counter. */
	for (int i = 0; i < end - base; i += 2) {
		if (*p & 0xc0) {
			if (p + 1 == end) {
				return -1;
			}
			int j = ((p[0] & 0x3f) << 8) | p[1];
			if (len < 0) {
				len = (int) (p + 2 - src);
			}
			if (j >= end - base) {
				return -1;
			}
			p = base + j;
		} else if (*p) {
			if (dest != dbegin) {
				*dest++ = '.';
			}
			int j = *p++;
			if (j >= end - p || j >= dend - dest) {
				return -1;
			}
			while (j--) {
				*dest++ = (char) *p++;
			}
		} else {
			*dest = '\0';
			if (len < 0) {
				len = (int) (p + 1 - src);
			}
			return len;
		}
	}
	return -1;
}

int dn_skipname(const unsigned char *s, const unsigned char *end)
{
	const unsigned char *p = s;
	while (p < end) {
		if (!*p) {
			return (int) (p - s + 1);
		}
		if (*p >= 192) {
			return p + 1 < end ? (int) (p - s + 2) : -1;
		}
		if (end - p < *p + 1) {
			break;
		}
		p += *p + 1;
	}
	return -1;
}

unsigned ns_get16(const unsigned char *cp)
{
	return (unsigned) (cp[0] << 8 | cp[1]);
}

unsigned long ns_get32(const unsigned char *cp)
{
	return (unsigned long) cp[0] << 24 | (unsigned long) cp[1] << 16 | (unsigned long) cp[2] << 8 | cp[3];
}

void ns_put16(unsigned s, unsigned char *cp)
{
	*cp++ = (unsigned char) (s >> 8);
	*cp = (unsigned char) s;
}

void ns_put32(unsigned long l, unsigned char *cp)
{
	*cp++ = (unsigned char) (l >> 24);
	*cp++ = (unsigned char) (l >> 16);
	*cp++ = (unsigned char) (l >> 8);
	*cp = (unsigned char) l;
}
