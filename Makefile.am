lib_LTLIBRARIES = libioxcf.la

ChangeLog: *
	git log --pretty=medium > $@

INCLUDES =			\
	$(GDKPIXBUF_CFLAGS)	\
	$(GLIB_CFLAGS)		\
	$(GIO_CFLAGS)

AM_CFLAGS = -g

libioxcf_la_SOURCES = io-xcf.c
libioxcf_la_LDFLAGS = -export_dynamic -avoid-version -module -no-undefined
libioxcf_la_LIBADD =		\
	$(GDKPIXBUF_LIBS)	\
	$(GLIB_LIBS)		\
	$(GIO_LIBS)
