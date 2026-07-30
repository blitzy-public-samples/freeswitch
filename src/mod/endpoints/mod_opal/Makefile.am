include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_opal

PKG_DIR=/usr/local/lib/pkgconfig

#DEBUG_SUFFIX:=--define-variable=suffix=_d
mod_LTLIBRARIES = mod_opal.la
mod_opal_la_SOURCES  = mod_opal.cpp
mod_opal_la_CPPFLAGS   = $(AM_CPPFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[^ID][^ ]*//g')
mod_opal_la_CXXFLAGS   = $(AM_CXXFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[ID][^ ]*//g')
mod_opal_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_opal_la_LDFLAGS  = -avoid-version -module -no-undefined -shared $(shell pkg-config opal $(DEBUG_SUFFIX) --libs)

# Test harness. Everything below this line is additive: the production
# mod_opal.la target above is unchanged, and nothing here alters module
# behaviour. The suite is only built and collected when endpoints/mod_opal is
# enabled in modules.conf, because src/mod/Makefile.am gates the whole
# per-module recipe on that, so an absent OPAL toolkit leaves it unbuilt
# instead of failing the build.

# Convenience library: a second compilation of the module sources that the test
# program links, so mod_opal_load(), mod_opal_shutdown() and the FSProcess /
# FSListener / FSManager classes are reachable without pulling the module
# translation unit into the test unit (which would define
# mod_opal_module_interface twice). Both flag expressions are the production
# target's own, reused verbatim so the code under test is compiled exactly as it
# is compiled for release: the _CPPFLAGS filter keeps only the -I and -D flags
# pkg-config reports, the _CXXFLAGS filter keeps everything except those.
noinst_LTLIBRARIES = libmodopal.la
libmodopal_la_SOURCES = $(mod_opal_la_SOURCES)
libmodopal_la_CPPFLAGS = $(AM_CPPFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[^ID][^ ]*//g')
libmodopal_la_CXXFLAGS = $(AM_CXXFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[ID][^ ]*//g')

# The FreeSWITCH Test suite. $(AM_CPPFLAGS) has to lead the per-target
# _CPPFLAGS: a per-target value replaces the global one rather than adding to
# it, and build/modmake.rulesam carries the core include paths on AM_CPPFLAGS
# (AM_CXXFLAGS is assigned nowhere in the tree), so leaving it out would put
# <switch.h> beyond reach. The two SWITCH_TEST_BASE_DIR defines are what make
# the suite's FST_CORE_BEGIN("conf_opal") resolve to the conf_opal fixture root
# in this directory instead of ./conf_opal in the working directory, per
# src/include/test/switch_test.h.
noinst_PROGRAMS = test/test_mod_opal
test_test_mod_opal_SOURCES = test/test_mod_opal.cpp
test_test_mod_opal_CPPFLAGS = $(AM_CPPFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[^ID][^ ]*//g') -I. -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_opal_CXXFLAGS = $(AM_CXXFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[ID][^ ]*//g')
test_test_mod_opal_LDFLAGS = $(AM_LDFLAGS) -avoid-version -no-undefined $(shell pkg-config opal $(DEBUG_SUFFIX) --libs)
test_test_mod_opal_LDADD = libmodopal.la $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)

# macOS needs the two frameworks on the test program as well. The production
# module target is not macOS conditioned today and is deliberately left alone.
if ISMAC
test_test_mod_opal_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
endif
