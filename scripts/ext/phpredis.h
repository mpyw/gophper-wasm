/* GOPHPER: what PHP 8.6 removed and phpredis 6.3 still uses, mapped to
 * what replaced it. Force-included by scripts/build-ext.sh. */
#define XtOffsetOf(type, field) offsetof(type, field)
#define zval_dtor(zv) zval_ptr_dtor_nogc(zv)
#define ZEND_WRONG_PARAM_COUNT() { zend_wrong_param_count(); return; }
#define php_hash_bin2hex(out, in, len) zend_bin2hex((out), (in), (len))
#define zval_is_true(zv) zend_is_true(zv)
#define EMPTY_SWITCH_DEFAULT_CASE() default: ZEND_UNREACHABLE(); break;
#define INI_INT(name) zend_ini_long((name), sizeof(name) - 1, false)
#define INI_STR(name) ((char *) zend_ini_string_ex((name), sizeof(name) - 1, false, NULL))
#define WRONG_PARAM_COUNT ZEND_WRONG_PARAM_COUNT()
