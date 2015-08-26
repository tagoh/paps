dnl Process this file with autoconf to produce a configuration script
AC_INIT(paps, 0.7.0, dov.grobgeld@gmail.com)
AC_CONFIG_AUX_DIR(config)
AC_CONFIG_SRCDIR(src/paps.c)

AM_INIT_AUTOMAKE

AC_PROG_CC
AX_COMPILER_FLAGS
WARN_CFLAGS="$WARN_CFLAGS -Wno-format-y2k"

dnl Requires to declare wcwidth()
AC_GNU_SOURCE

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([PANGO], [pangocairo pangoft2])

dnl ======================================================
dnl check for CUPS
dnl ======================================================
AC_PATH_PROG(CUPS_CONFIG, cups-config, no)
if test "x$CUPS_CONFIG" != "xno"; then
	CUPS_CFLAGS=`$CUPS_CONFIG --cflags | sed -e 's/-O[0-9]*//' -e 's/-m[^\t]*//g'`
	CUPS_LIBS=`$CUPS_CONFIG --libs`
	AC_DEFINE_UNQUOTED([HAVE_CUPS], [1], [CUPS support])
fi
AC_SUBST(CUPS_CFLAGS)
AC_SUBST(CUPS_LIBS)

AC_OUTPUT(Makefile src/Makefile)
