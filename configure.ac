AC_INIT(io-xcf, 0.0.1, stephane@delcroix.org)
AC_CONFIG_SRCDIR(.)

AM_INIT_AUTOMAKE

AC_PROG_CXX

AC_PROG_LIBTOOL

PKG_CHECK_MODULES(GLIB, glib-2.0)
PKG_CHECK_MODULES(GDKPIXBUF, gdk-pixbuf-2.0)

AC_CHECK_HEADER(bzlib.h,,AC_MSG_ERROR(Can not find bzlib header))
AC_CHECK_LIB(bz2,BZ2_bzDecompressInit,,AC_MSG_ERROR(Can not find libbz2))

AC_OUTPUT(
Makefile
)
