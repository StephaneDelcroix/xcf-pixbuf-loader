AC_INIT(io-xcf, 0.0.1, stephane@delcroix.org)
AC_CONFIG_SRCDIR(.)
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE

AC_PROG_CXX

AC_PROG_LIBTOOL

PKG_CHECK_MODULES(GLIB, glib-2.0)
PKG_CHECK_MODULES(GMODULE, gmodule-2.0)
PKG_CHECK_MODULES(GDKPIXBUF, gdk-pixbuf-2.0)
PKG_CHECK_MODULES(GIO, gio-2.0 >= 2.23 gio-unix-2.0, old_gio=0, old_gio=1)

if test "x$old_gio" = "x0"; then
	AC_DEFINE(GIO_2_23, 1, [Define if GIO is newer than 2.23])
fi
AM_CONDITIONAL([GIO_2_23],[test "x$old_gio" != "x1"])

AC_CHECK_HEADER(bzlib.h,,AC_MSG_ERROR(Can not find bzlib header))
AC_CHECK_LIB(bz2,BZ2_bzDecompressInit,,AC_MSG_ERROR(Can not find libbz2))

AC_OUTPUT(
Makefile
)
if test "x$old_gio" = "x1"; then
	echo
	echo .xcf.gz support disabled, reason: $GIO_PKG_ERRORS
fi

echo
echo io-xcf successfully configured, type make to build
