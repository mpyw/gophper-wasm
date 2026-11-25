/* GOPHPER: PHP functions written in Go.
 *
 * At startup, patches/0010 calls gophper_fn_register(), which asks the host
 * for the names (fn_names) and registers each as an internal function. All
 * of them share one handler, which encodes the arguments, calls the host
 * (fn_call), and decodes what comes back (fn_take). A Go error becomes a
 * RuntimeException.
 *
 * The encoding is in ABI.md. Like gophper_zend.c, this file needs php.h, so
 * scripts/build.sh compiles it against the configured php-src.
 */
#include "php.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#include "ext/spl/spl_exceptions.h"

#include <stdint.h>

#define GOPHPER_IMPORT(name) __attribute__((import_module("gophper"), import_name(name)))

GOPHPER_IMPORT("fn_names") int32_t host_fn_names(char *out, int32_t cap);
GOPHPER_IMPORT("fn_call") int32_t host_fn_call(const char *name, int32_t name_len, const char *args, int32_t args_len);
GOPHPER_IMPORT("fn_take") int32_t host_fn_take(char *out, int32_t cap);

/* Nesting deeper than this is refused, as a guard against cycles. */
#define GOPHPER_FN_MAX_DEPTH 64

static void gophper_fn_u32(smart_str *s, uint32_t v)
{
	smart_str_appendl(s, (const char *) &v, 4);
}

static void gophper_fn_u64(smart_str *s, uint64_t v)
{
	smart_str_appendl(s, (const char *) &v, 8);
}

static void gophper_fn_str(smart_str *s, const char *p, size_t n)
{
	smart_str_appendc(s, 's');
	gophper_fn_u32(s, (uint32_t) n);
	smart_str_appendl(s, p, n);
}

static bool gophper_fn_encode(smart_str *s, zval *zv, int depth);

static bool gophper_fn_encode_hash(smart_str *s, HashTable *ht, int depth)
{
	smart_str_appendc(s, 'a');
	gophper_fn_u32(s, zend_hash_num_elements(ht));
	zend_ulong index;
	zend_string *key;
	zval *val;
	ZEND_HASH_FOREACH_KEY_VAL(ht, index, key, val) {
		if (key) {
			gophper_fn_str(s, ZSTR_VAL(key), ZSTR_LEN(key));
		} else {
			smart_str_appendc(s, 'i');
			gophper_fn_u64(s, (uint64_t) index);
		}
		if (!gophper_fn_encode(s, val, depth + 1)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static bool gophper_fn_encode(smart_str *s, zval *zv, int depth)
{
	if (depth > GOPHPER_FN_MAX_DEPTH) {
		zend_throw_exception(spl_ce_RuntimeException, "Value nested too deeply for a Go function", 0);
		return false;
	}
	ZVAL_DEREF(zv);
	switch (Z_TYPE_P(zv)) {
	case IS_FALSE:
		smart_str_appendc(s, 'F');
		return true;
	case IS_TRUE:
		smart_str_appendc(s, 'T');
		return true;
	case IS_LONG:
		smart_str_appendc(s, 'i');
		gophper_fn_u64(s, (uint64_t) Z_LVAL_P(zv));
		return true;
	case IS_DOUBLE: {
		double d = Z_DVAL_P(zv);
		uint64_t bits;
		memcpy(&bits, &d, 8);
		smart_str_appendc(s, 'd');
		gophper_fn_u64(s, bits);
		return true;
	}
	case IS_STRING:
		gophper_fn_str(s, Z_STRVAL_P(zv), Z_STRLEN_P(zv));
		return true;
	case IS_ARRAY:
		return gophper_fn_encode_hash(s, Z_ARRVAL_P(zv), depth);
	case IS_OBJECT: {
		/* An object goes as its properties, as (array) would give them. */
		HashTable *props = zend_get_properties_for(zv, ZEND_PROP_PURPOSE_ARRAY_CAST);
		bool ok = props ? gophper_fn_encode_hash(s, props, depth) : (smart_str_appendc(s, 'N'), true);
		if (props) {
			zend_release_properties(props);
		}
		return ok;
	}
	default:
		smart_str_appendc(s, 'N');
		return true;
	}
}

static bool gophper_fn_decode(const char **p, const char *end, zval *out, int depth)
{
	if (*p >= end || depth > GOPHPER_FN_MAX_DEPTH) {
		return false;
	}
	char tag = *(*p)++;
	switch (tag) {
	case 'N':
		ZVAL_NULL(out);
		return true;
	case 'F':
		ZVAL_FALSE(out);
		return true;
	case 'T':
		ZVAL_TRUE(out);
		return true;
	case 'i': {
		int64_t v;
		if (end - *p < 8) return false;
		memcpy(&v, *p, 8);
		*p += 8;
		ZVAL_LONG(out, (zend_long) v);
		return true;
	}
	case 'd': {
		double v;
		if (end - *p < 8) return false;
		memcpy(&v, *p, 8);
		*p += 8;
		ZVAL_DOUBLE(out, v);
		return true;
	}
	case 's': {
		uint32_t n;
		if (end - *p < 4) return false;
		memcpy(&n, *p, 4);
		*p += 4;
		if ((uint32_t) (end - *p) < n) return false;
		ZVAL_STRINGL(out, *p, n);
		*p += n;
		return true;
	}
	case 'a': {
		uint32_t n;
		if (end - *p < 4) return false;
		memcpy(&n, *p, 4);
		*p += 4;
		array_init_size(out, n);
		for (uint32_t i = 0; i < n; i++) {
			zval key, val;
			if (!gophper_fn_decode(p, end, &key, depth + 1)) {
				return false;
			}
			if (!gophper_fn_decode(p, end, &val, depth + 1)) {
				zval_ptr_dtor(&key);
				return false;
			}
			if (Z_TYPE(key) == IS_LONG) {
				zend_hash_index_update(Z_ARRVAL_P(out), Z_LVAL(key), &val);
			} else if (Z_TYPE(key) == IS_STRING) {
				zend_symtable_update(Z_ARRVAL_P(out), Z_STR(key), &val);
			} else {
				zval_ptr_dtor(&val);
			}
			zval_ptr_dtor(&key);
		}
		return true;
	}
	}
	return false;
}

static ZEND_FUNCTION(gophper_fn_call)
{
	zend_string *name = EX(func)->common.function_name;
	uint32_t argc = ZEND_NUM_ARGS();
	zval *args = ZEND_CALL_ARG(execute_data, 1);

	smart_str s = {0};
	smart_str_appendc(&s, 'a');
	gophper_fn_u32(&s, argc);
	for (uint32_t i = 0; i < argc; i++) {
		smart_str_appendc(&s, 'i');
		gophper_fn_u64(&s, i);
		if (!gophper_fn_encode(&s, &args[i], 0)) {
			smart_str_free(&s);
			RETURN_THROWS();
		}
	}
	smart_str_0(&s);
	int32_t n = host_fn_call(ZSTR_VAL(name), (int32_t) ZSTR_LEN(name), ZSTR_VAL(s.s), (int32_t) ZSTR_LEN(s.s));
	smart_str_free(&s);

	bool failed = n < 0;
	int32_t len = failed ? -1 - n : n;
	char *buf = emalloc((size_t) len + 1);
	host_fn_take(buf, len);
	buf[len] = '\0';
	if (failed) {
		zend_throw_exception(spl_ce_RuntimeException, buf, 0);
		efree(buf);
		RETURN_THROWS();
	}
	const char *p = buf;
	if (!gophper_fn_decode(&p, buf + len, return_value, 0) || p != buf + len) {
		zval_ptr_dtor(return_value);
		ZVAL_NULL(return_value);
		efree(buf);
		zend_throw_exception(spl_ce_RuntimeException, "Bad value from a Go function", 0);
		RETURN_THROWS();
	}
	efree(buf);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(gophper_fn_arginfo, 0, 0, IS_MIXED, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, args, IS_MIXED, 0)
ZEND_END_ARG_INFO()

/* gophper_fn_register registers the host's functions. patches/0010 calls it
 * once the extensions have started, so a clash with one of theirs is
 * reported, and the others are still registered. */
void gophper_fn_register(void)
{
	int32_t n = host_fn_names(NULL, 0);
	if (n <= 0) {
		return;
	}
	char *names = pemalloc((size_t) n, 1);
	host_fn_names(names, n);
	for (char *p = names; p < names + n; p += strlen(p) + 1) {
		zend_function_entry fe[2] = {
			{p, ZEND_FN(gophper_fn_call), gophper_fn_arginfo,
				(uint32_t) (sizeof(gophper_fn_arginfo) / sizeof(struct _zend_internal_arg_info) - 1), 0, NULL, NULL},
			{0},
		};
		zend_register_functions(NULL, fe, NULL, MODULE_PERSISTENT);
	}
	/* The names stay: the function table refers to them until shutdown. */
}
