AM_CFLAGS = \
        -I$(top_srcdir) \
        $(WARNING_CFLAGS) \
        $(CODE_COVERAGE_CFLAGS)

AM_LDFLAGS = \
        $(CODE_COVERAGE_LIBS)

noinst_LTLIBRARIES = libroaring.la

libroaring_la_SOURCES = \
	roaring.c \
	roaring.h

EXTRA_DIST = AUTHORS LICENSE README.md
