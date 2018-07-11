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

#ifndef PHP_EPHPCONF_H
#define PHP_EPHPCONF_H

extern zend_module_entry ephpconf_module_entry;
#define phpext_ephpconf_ptr &ephpconf_module_entry

#ifdef PHP_WIN32
#define PHP_EPHPCONF_API __declspec(dllexport)
#else
#define PHP_EPHPCONF_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

#ifdef ZTS
#define EPHPCONF_G(v) TSRMG(yaconf_globals_id, zend_yaconf_globals *, v)
#else
#define EPHPCONF_G(v) (yaconf_globals.v)
#endif

#define EPHPCONF_VERSION  "1.0.0"

#ifdef EPHPCONF_DEBUG
#undef EPHPCONF_DEBUG
#define EPHPCONF_DEBUG(m) fprintf(stderr, "%s\n", m);
#else
#define EPHPCONF_DEBUG(m) 
#endif

ZEND_BEGIN_MODULE_GLOBALS(ephpconf)
	char *directory;
#ifndef ZTS
	long   check_delay;
	time_t last_check;
	time_t directory_mtime;
#endif
ZEND_END_MODULE_GLOBALS(ephpconf)

PHP_MINIT_FUNCTION(ephpconf);
PHP_MSHUTDOWN_FUNCTION(ephpconf);
#ifndef ZTS
PHP_RINIT_FUNCTION(ephpconf);
#endif
PHP_MINFO_FUNCTION(ephpconf);
PHP_GINIT_FUNCTION(ephpconf);

extern ZEND_DECLARE_MODULE_GLOBALS(ephpconf);

BEGIN_EXTERN_C() 
PHPAPI zval *php_ephpconf_get(zend_string *name);
PHPAPI int php_ephpconf_has(zend_string *name);
END_EXTERN_C()

#endif	/* PHP_EPHPCONF_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */