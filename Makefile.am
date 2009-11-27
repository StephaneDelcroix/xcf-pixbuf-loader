lib_LTLIBRARIES = libioxcf.la

ChangeLog: *
	git log --pretty=medium > $@

INCLUDES =			\
	$(GDKPIXBUF_CFLAGS)	\
	$(GLIB_CFLAGS)		\
	$(GIO_CFLAGS)

AM_CFLAGS = -g

BZ2_DECOMPRESSOR_FILES = yelp-bz2-decompressor.c yelp-bz2-decompressor.h

if GIO_2_23
BZ2_DECOMPRESSOR = $(BZ2_DECOMPRESSOR_FILES)
else
BZ2_DECOMPRESSOR =
endif

libioxcf_la_SOURCES = io-xcf.c $(BZ2_DECOMPRESSOR)
libioxcf_la_LDFLAGS = -export_dynamic -avoid-version -module -no-undefined
libioxcf_la_LIBADD =		\
	$(GDKPIXBUF_LIBS)	\
	$(GLIB_LIBS)		\
	$(GIO_LIBS)

EXTRA_DIST = $(BZ2_DECOMPRESSOR_FILES)
