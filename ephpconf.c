/*
  +----------------------------------------------------------------------+
  | Yet Another Conf                                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Xinchen Hui  <laruence@php.net>                              |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "main/php_scandir.h"
#include "ext/standard/info.h"
#include "php_ephpconf.h"

ZEND_DECLARE_MODULE_GLOBALS(ephpconf);

static zend_array *ini_containers;
static zend_array *parsed_ini_files;
static zval active_ini_file_section;

zend_class_entry *ephpconf_ce;

static void php_ephpconf_zval_persistent(zval *zv, zval *rv);

typedef struct _ephpconf_filenode {
	zend_string *filename;
	time_t mtime;
} ephpconf_filenode;

#define PALLOC_HASHTABLE(ht) do { \
	(ht) = (zend_array*)pemalloc(sizeof(zend_array), 1); \
	if ((ht) == NULL) { \
		zend_error(E_ERROR, "Cannot allocate zend_array"); \
	} \
} while(0);

/* {{{ ARG_INFO
 */
ZEND_BEGIN_ARG_INFO_EX(php_ephpconf_get_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(php_ephpconf_has_arginfo, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ ephpconf_module_entry
 */
zend_module_entry ephpconf_module_entry = {
	STANDARD_MODULE_HEADER,
	"ephpconf",
	NULL,
	PHP_MINIT(ephpconf),
	PHP_MSHUTDOWN(ephpconf),
#ifndef ZTS
	PHP_RINIT(ephpconf),
#else
	NULL,
#endif
	NULL,
	PHP_MINFO(ephpconf),
	EPHPCONF_VERSION,
	PHP_MODULE_GLOBALS(ephpconf),
	PHP_GINIT(ephpconf),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_EPHPCONF
ZEND_GET_MODULE(ephpconf)
#endif

static void php_ephpconf_hash_destroy(zend_array *ht) /* {{{ */ {
	zend_string *key;
	zend_long idx;
	zval *element, rv;

	if (((ht)->u.flags & HASH_FLAG_INITIALIZED)) {
		ZEND_HASH_FOREACH_KEY_VAL(ht, idx, key, element) {
			if (key) {
				free(key);
			}
			switch (Z_TYPE_P(element)) {
				case IS_PTR:
				case IS_STRING:
					free(Z_PTR_P(element));
					break;
				case IS_ARRAY:
					php_ephpconf_hash_destroy(Z_ARRVAL_P(element));
					break;
			}
		} ZEND_HASH_FOREACH_END();
		free(HT_GET_DATA_ADDR(ht));
	}
	free(ht);
} /* }}} */

static void php_ephpconf_hash_copy(zend_array *target, zend_array *source) /* {{{ */ {
	zend_string *key;
	zend_long idx;
	zval *element, rv;

	ZEND_HASH_FOREACH_KEY_VAL(source, idx, key, element) {
		php_ephpconf_zval_persistent(element, &rv);
		if (key) {
			zend_hash_str_update(target, key->val, key->len, &rv);
		} else {
			zend_hash_index_update(target, idx, &rv);
		}
	} ZEND_HASH_FOREACH_END();
} /* }}} */

static void php_ephpconf_zval_persistent(zval *zv, zval *rv) /* {{{ */ {
	switch (Z_TYPE_P(zv)) {
		case IS_CONSTANT:
		case IS_STRING:
			{
				zend_string *str = zend_string_init(Z_STRVAL_P(zv), Z_STRLEN_P(zv), 1);
				GC_FLAGS(str) |= IS_STR_INTERNED | IS_STR_PERMANENT;
				ZVAL_INTERNED_STR(rv, str);
			}
			break;
		case IS_ARRAY:
			{
				zend_array *ht;
				PALLOC_HASHTABLE(ht);
				zend_hash_init(ht, zend_hash_num_elements(Z_ARRVAL_P(zv)), NULL, NULL, 1);
				ZVAL_ARR(rv, ht);
				php_ephpconf_hash_copy(ht, Z_ARRVAL_P(zv));
			}
			break;
		case IS_RESOURCE:
		case IS_OBJECT:
		case _IS_BOOL:
		case IS_LONG:
		case IS_NULL:
			ZEND_ASSERT(0);
			break;
	}
} /* }}} */

static void php_ephpconf_simple_parser_cb(zval *key, zval *value, zval *index, int callback_type, void *arg) /* {{{ */ {
	char       *seg, *skey, *ptr;
	zval       *pzval, *target, rv;
	zval       *arr = (zval *)arg;
	zend_array *ht;

	if (callback_type == ZEND_INI_PARSER_ENTRY) {
		if (value == NULL) {
			return;
		}
		target = arr;
		skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));
		if ((seg = php_strtok_r(skey, ".", &ptr))) {
			do {
				char *real_key = seg;
				seg = php_strtok_r(NULL, ".", &ptr);
				if ((pzval = zend_symtable_str_find(Z_ARRVAL_P(target), real_key, strlen(real_key))) == NULL) {
					if (seg) {
						PALLOC_HASHTABLE(ht);
						zend_hash_init(ht, 8, NULL, NULL, 1);
						ZVAL_ARR(&rv, ht);
						pzval = zend_symtable_str_update(Z_ARRVAL_P(target), real_key, strlen(real_key), &rv);
					} else {
						php_ephpconf_zval_persistent(value, &rv);
						zend_symtable_str_update(Z_ARRVAL_P(target), real_key, strlen(real_key), &rv);
						break;
					}
				} else {
					if (IS_ARRAY != Z_TYPE_P(pzval)) {
						if (seg) {
							PALLOC_HASHTABLE(ht);
							zend_hash_init(ht, 8, NULL, NULL, 1);
							ZVAL_ARR(&rv, ht);
							pzval = zend_symtable_str_update(Z_ARRVAL_P(target), real_key, strlen(real_key), &rv);
						} else {
							php_ephpconf_zval_persistent(value, &rv);
							pzval = zend_symtable_str_update(Z_ARRVAL_P(target), real_key, strlen(real_key), &rv);
						}
					} 
				}
				target = pzval;
			} while (seg);
		}
		efree(skey);
	} else if (callback_type == ZEND_INI_PARSER_POP_ENTRY) {
		if (value == NULL) {
			return;
		}

		if (!(Z_STRLEN_P(key) > 1 && Z_STRVAL_P(key)[0] == '0')
				&& is_numeric_string(Z_STRVAL_P(key), Z_STRLEN_P(key), NULL, NULL, 0) == IS_LONG) {
			zend_long idx = (zend_long)zend_atol(Z_STRVAL_P(key), Z_STRLEN_P(key));
			if ((pzval = zend_hash_index_find(Z_ARRVAL_P(arr), idx)) == NULL) {
				PALLOC_HASHTABLE(ht);
				zend_hash_init(ht, 8, NULL, NULL, 1);
				ZVAL_ARR(&rv, ht);
				pzval = zend_hash_index_update(Z_ARRVAL_P(arr), idx, &rv);
			} 
		} else {
			char *seg, *ptr;
			char *skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));

			target = arr;
			if ((seg = php_strtok_r(skey, ".", &ptr))) {
				do {
					if ((pzval = zend_symtable_str_find(Z_ARRVAL_P(target), seg, strlen(seg))) == NULL) {
						PALLOC_HASHTABLE(ht);
						zend_hash_init(ht, 8, NULL, NULL, 1);
						ZVAL_ARR(&rv, ht);
						pzval = zend_symtable_str_update(Z_ARRVAL_P(target), seg, strlen(seg), &rv);
					}
					target = pzval;
					seg = php_strtok_r(NULL, ".", &ptr);
				} while (seg);
			} else {
				if ((pzval = zend_symtable_str_find(Z_ARRVAL_P(target), seg, strlen(seg))) == NULL) {
					PALLOC_HASHTABLE(ht);
					zend_hash_init(ht, 8, NULL, NULL, 1);
					ZVAL_ARR(&rv, ht);
					pzval = zend_symtable_str_update(Z_ARRVAL_P(target), seg, strlen(seg), &rv);
				} 
			}
			efree(skey);
		}

		if (Z_TYPE_P(pzval) != IS_ARRAY) {
			zval_dtor(pzval);
			PALLOC_HASHTABLE(ht);
			zend_hash_init(ht, 8, NULL, NULL, 1);
			ZVAL_ARR(pzval, ht);
		}

		php_ephpconf_zval_persistent(value, &rv);
		if (index && Z_STRLEN_P(index) > 0) {
			add_assoc_zval_ex(pzval, Z_STRVAL_P(index), Z_STRLEN_P(index), &rv);
		} else {
			add_next_index_zval(pzval, &rv);
		}
	} else if (callback_type == ZEND_INI_PARSER_SECTION) {
	}
}
/* }}} */

static void php_ephpconf_ini_parser_cb(zval *key, zval *value, zval *index, int callback_type, void *arg) /* {{{ */ {
	zval *arr = (zval *)arg;

	if (callback_type == ZEND_INI_PARSER_SECTION) {
		zend_array *ht;
		zval *parent;
		char *seg, *skey;

		skey = estrndup(Z_STRVAL_P(key), Z_STRLEN_P(key));

		PALLOC_HASHTABLE(ht);
		zend_hash_init(ht, 128, NULL, NULL, 1);
		ZVAL_ARR(&active_ini_file_section, ht);

		if ((seg = strchr(skey, ':'))) {
			char *section;

			while (*(seg) == ' ' || *(seg) == ':') {
				*(seg++) = '\0';
			}

			if ((section = strrchr(seg, ':'))) {
			    /* muilt-inherit */
				do {
					while (*(section) == ' ' || *(section) == ':') {
						*(section++) = '\0';
					}
					if ((parent = zend_symtable_str_find(Z_ARRVAL_P(arr), section, strlen(section)))) {
						php_ephpconf_hash_copy(Z_ARRVAL(active_ini_file_section), Z_ARRVAL_P(parent));
					}
				} while ((section = strrchr(seg, ':')));
			}

			/* remove the tail space, thinking of 'foo : bar : test' */
			section = seg + strlen(seg) - 1;
			while (*section == ' ' || *section == ':') {
				*(section--) = '\0';
			}

			if ((parent = zend_symtable_str_find(Z_ARRVAL_P(arr), seg, strlen(seg)))) {
				php_ephpconf_hash_copy(Z_ARRVAL(active_ini_file_section), Z_ARRVAL_P(parent));
			}
		} 
	    seg = skey + strlen(skey) - 1;
		while (*seg == ' ' || *seg == ':') {
			*(seg--) = '\0';
		}	
		zend_symtable_str_update(Z_ARRVAL_P(arr), skey, strlen(skey), &active_ini_file_section);

		efree(skey);
	} else if (value) {
		zval *active_arr;
		if (!Z_ISUNDEF(active_ini_file_section)) {
			active_arr = &active_ini_file_section;
		} else {
			active_arr = arr;
		}
		php_ephpconf_simple_parser_cb(key, value, index, callback_type, active_arr);
	}
}
/* }}} */

PHPAPI zval *php_ephpconf_get(zend_string *name) /* {{{ */ {
	if (ini_containers) {
		zval *pzval;
		HashTable *target = ini_containers;

		if (zend_memrchr(name->val, '.', name->len)) {
			char *entry, *ptr, *seg;
			entry = estrndup(name->val, name->len);
			if ((seg = php_strtok_r(entry, ".", &ptr))) {
				do {
					if (target == NULL || (pzval = zend_symtable_str_find(target, seg, strlen(seg))) == NULL) {
						efree(entry);
						return NULL;
					}
					if (Z_TYPE_P(pzval) == IS_ARRAY) {
						target = Z_ARRVAL_P(pzval);
					} else {
						target = NULL;
					}
				} while ((seg = php_strtok_r(NULL, ".", &ptr)));
			}
			efree(entry);
		} else {
			pzval = zend_symtable_find(target, name);
		}

		return pzval;
	}
	return NULL;
}
/* }}} */

PHPAPI int php_ephpconf_has(zend_string *name) /* {{{ */ {
	if (ini_containers) {
		zval *pzval;
		zend_array *target = ini_containers;

		if (zend_memrchr(name->val, '.', name->len)) {
			char  *entry, *ptr, *seg;
			entry = estrndup(name->val, name->len);
			if ((seg = php_strtok_r(entry, ".", &ptr))) {
				do {
					if (target == NULL || !(pzval = zend_symtable_str_find(target, seg, strlen(seg)))) {
						efree(entry);
						return 0;
					}
					if (Z_TYPE_P(pzval) == IS_ARRAY) {
						target = Z_ARRVAL_P(pzval);
					} else {
						target = NULL;
					}
				} while ((seg = php_strtok_r(NULL, ".", &ptr)));
				efree(entry);
				return 1;
			}
		} else {
			return zend_symtable_exists(target, name);
		}
	}
	return 0;
}
/* }}} */

/** {{{ proto public Ephpconf::get(string $name, $default = NULL)
*/
PHP_METHOD(ephpconf, get) {
	zend_string *name;
	zval *val, *defv = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|z", &name, &defv) == FAILURE) {
		return;
	} 

	val = php_ephpconf_get(name);
	if (val) {
		RETURN_ZVAL(val, 1, 0);
	} else if (defv) {
		RETURN_ZVAL(defv, 1, 0);
	}

	RETURN_NULL();
}
/* }}} */

/** {{{ proto public Ephpconf::has(string $name)
*/
PHP_METHOD(ephpconf, has) {
	zend_string *name;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &name) == FAILURE) {
		return;
	} 

	RETURN_BOOL(php_ephpconf_has(name));
}
/* }}} */

/* {{{  ephpconf_methods */
zend_function_entry ephpconf_methods[] = {
	PHP_ME(ephpconf, get, php_ephpconf_get_arginfo, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(ephpconf, has, php_ephpconf_has_arginfo, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("ephpconf.directory", "", PHP_INI_SYSTEM, OnUpdateString, directory, zend_ephpconf_globals, ephpconf_globals)
#ifndef ZTS
	STD_PHP_INI_ENTRY("ephpconf.check_delay", "300", PHP_INI_SYSTEM, OnUpdateLong, check_delay, zend_ephpconf_globals, ephpconf_globals)
#endif
PHP_INI_END()
/* }}} */

/* {{{ PHP_GINIT_FUNCTION
*/
PHP_GINIT_FUNCTION(ephpconf)
{
	ephpconf_globals->directory = NULL;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(ephpconf)
{
	const char *dirname;
	size_t dirlen;
	zend_class_entry ce;
	struct stat dir_sb = {0};

	REGISTER_INI_ENTRIES();

	INIT_CLASS_ENTRY(ce, "Ephpconf", ephpconf_methods);

	ephpconf_ce = zend_register_internal_class_ex(&ce, NULL);

	if ((dirname = EPHPCONF_G(directory)) && (dirlen = strlen(dirname)) 
#ifndef ZTS
			&& !VCWD_STAT(dirname, &dir_sb) && S_ISDIR(dir_sb.st_mode)
#endif
			) {
		zend_array *ht;
		zval result;
		int ndir;
		struct dirent **namelist;
		char *p, ini_file[MAXPATHLEN];

#ifndef ZTS
		EPHPCONF_G(directory_mtime) = dir_sb.st_mtime;
#endif

		if ((ndir = php_scandir(dirname, &namelist, 0, php_alphasort)) > 0) {
			int i;
			struct stat sb;
	        zend_file_handle fh = {0};

			PALLOC_HASHTABLE(ini_containers);
			zend_hash_init(ini_containers, ndir, NULL, NULL, 1);

			PALLOC_HASHTABLE(parsed_ini_files);
			zend_hash_init(parsed_ini_files, ndir, NULL, NULL, 1);

			for (i = 0; i < ndir; i++) {
				if (!(p = strrchr(namelist[i]->d_name, '.')) || strcmp(p, ".ini")) {
					free(namelist[i]);
					continue;
				}

				snprintf(ini_file, MAXPATHLEN, "%s%c%s", dirname, DEFAULT_SLASH, namelist[i]->d_name);

				PALLOC_HASHTABLE(ht);
				zend_hash_init(ht, 128, NULL, NULL, 1);
				ZVAL_ARR(&result, ht);

				zend_symtable_str_update(ini_containers, namelist[i]->d_name, p - namelist[i]->d_name, &result);

				if (VCWD_STAT(ini_file, &sb) == 0) {
					if (S_ISREG(sb.st_mode)) {
						ephpconf_filenode node;
						if ((fh.handle.fp = VCWD_FOPEN(ini_file, "r"))) {
							fh.filename = ini_file;
							fh.type = ZEND_HANDLE_FP;
				            ZVAL_UNDEF(&active_ini_file_section);
							if (zend_parse_ini_file(&fh, 0, 0 /* ZEND_INI_SCANNER_NORMAL */,
									php_ephpconf_ini_parser_cb, (void *)&result) == FAILURE) {
								free(namelist[i]);
								php_error(E_WARNING, "Parsing '%s' failed", ini_file);
								continue;
							}
						}
						
						node.filename = zend_string_init(namelist[i]->d_name, strlen(namelist[i]->d_name), 1);
						node.mtime = sb.st_mtime;
						zend_hash_update_mem(parsed_ini_files, node.filename, &node, sizeof(ephpconf_filenode));
					}
				} else {
					php_error(E_ERROR, "Could not stat '%s'", ini_file);
				}
				free(namelist[i]);
			}
#ifndef ZTS
			EPHPCONF_G(last_check) = time(NULL);
#endif
			free(namelist);
		} else {
			php_error(E_ERROR, "Couldn't opendir '%s'", dirname);
		}
	}

	return SUCCESS;
}
/* }}} */

#ifndef ZTS
/* {{{ PHP_RINIT_FUNCTION(ephpconf)
*/
PHP_RINIT_FUNCTION(ephpconf)
{
	if (EPHPCONF_G(check_delay) && (time(NULL) - EPHPCONF_G(last_check) < EPHPCONF_G(check_delay))) {
		EPHPCONF_DEBUG("config check delay doesn't execceed, ignore");
		return SUCCESS;
	} else {
		char *dirname;
		struct stat dir_sb = {0};
		zend_array *ht;

		EPHPCONF_G(last_check) = time(NULL);

		if ((dirname = EPHPCONF_G(directory)) && !VCWD_STAT(dirname, &dir_sb) && S_ISDIR(dir_sb.st_mode)) {
			if (dir_sb.st_mtime == EPHPCONF_G(directory_mtime)) {
				EPHPCONF_DEBUG("config directory is not modefied");
				return SUCCESS;
			} else {
				zval result;
				int i, ndir;
				struct dirent **namelist;
				char *p, ini_file[MAXPATHLEN];

				EPHPCONF_G(directory_mtime) = dir_sb.st_mtime;

				if ((ndir = php_scandir(dirname, &namelist, 0, php_alphasort)) > 0) {
					struct stat sb;
					zend_file_handle fh = {0};
					ephpconf_filenode *node = NULL;

					for (i = 0; i < ndir; i++) {
						zval *orig_ht = NULL;
						if (!(p = strrchr(namelist[i]->d_name, '.')) || (p && strcmp(p, ".ini"))) {
							free(namelist[i]);
							continue;
						}

						snprintf(ini_file, MAXPATHLEN, "%s%c%s", dirname, DEFAULT_SLASH, namelist[i]->d_name);
						if (VCWD_STAT(ini_file, &sb) || !S_ISREG(sb.st_mode)) {
							free(namelist[i]);
							continue;
						}

						if ((node = (ephpconf_filenode*)zend_hash_str_find_ptr(parsed_ini_files, namelist[i]->d_name, strlen(namelist[i]->d_name))) == NULL) {
							EPHPCONF_DEBUG("new configure file found");
						} else if (node->mtime == sb.st_mtime) {
							free(namelist[i]);
							continue;
						}

						PALLOC_HASHTABLE(ht);
						zend_hash_init(ht, 128, NULL, NULL, 1);
						ZVAL_ARR(&result, ht);

						if ((fh.handle.fp = VCWD_FOPEN(ini_file, "r"))) {
							fh.filename = ini_file;
							fh.type = ZEND_HANDLE_FP;
							ZVAL_UNDEF(&active_ini_file_section);
							if (zend_parse_ini_file(&fh, 0, 0 /* ZEND_INI_SCANNER_NORMAL */,
									php_ephpconf_ini_parser_cb, (void *)&result) == FAILURE) {
								free(namelist[i]);
								zval_dtor(&result);
								php_error(E_NOTICE, "Parsing '%s' failed, ignored", ini_file);
								continue;
							}
						}

						if ((orig_ht = zend_symtable_str_find(ini_containers,
										namelist[i]->d_name, p - namelist[i]->d_name)) != NULL) {
							php_ephpconf_hash_destroy(Z_ARRVAL_P(orig_ht));
							ZVAL_COPY_VALUE(orig_ht, &result);
						} else {
							zend_symtable_str_update(ini_containers,
									namelist[i]->d_name, p - namelist[i]->d_name, &result);
						}

						if (node) {
							node->mtime = sb.st_mtime;
						} else {
							ephpconf_filenode n = {0};
							n.filename = zend_string_init(namelist[i]->d_name, strlen(namelist[i]->d_name), 1);
							n.mtime = sb.st_mtime;
							zend_hash_update_mem(parsed_ini_files, n.filename, &n, sizeof(ephpconf_filenode));
						}
						free(namelist[i]);
					}
					free(namelist);
				}
				return SUCCESS;
			}
		} 
		EPHPCONF_DEBUG("stat config directory failed");
	}

	return SUCCESS;
}
/* }}} */
#endif

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(ephpconf)
{
	UNREGISTER_INI_ENTRIES();

	if (parsed_ini_files) {
		php_ephpconf_hash_destroy(parsed_ini_files);
	}

	if (ini_containers) {
		php_ephpconf_hash_destroy(ini_containers);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(ephpconf)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "ephpconf support", "enabled");
	php_info_print_table_row(2, "version", EPHPCONF_VERSION);
#ifndef ZTS
	php_info_print_table_row(2, "ephpconf config last check time",  ctime(&(EPHPCONF_G(last_check))));
#endif
	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_header(2, "parsed filename", "mtime");
	if (parsed_ini_files && zend_hash_num_elements(parsed_ini_files)) {
		ephpconf_filenode *node;
		ZEND_HASH_FOREACH_PTR(parsed_ini_files, node) {
			php_info_print_table_row(2, node->filename,  ctime(&node->mtime));
		} ZEND_HASH_FOREACH_END();
	}
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
