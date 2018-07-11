dnl $Id$
dnl config.m4 for extension ephpconf

PHP_ARG_ENABLE(ephpconf, whether to enable ephpconf support,
[  --enable-ephpconf           Enable ephpconf support])

if test "$PHP_EPHPCONF" != "no"; then
  PHP_SUBST(EPHPCONF_SHARED_LIBADD)
  PHP_NEW_EXTENSION(ephpconf, ephpconf.c, $ext_shared)
fi
