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

# A second compilation of the module sources, linked by the test program to reach the
# module entry points and classes.  With this library on the link line, the test unit
# must not also include ../mod_opal.cpp: that would define mod_opal_module_interface twice.
noinst_LTLIBRARIES = libmodopal.la
libmodopal_la_SOURCES = $(mod_opal_la_SOURCES)
libmodopal_la_CPPFLAGS = $(AM_CPPFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[^ID][^ ]*//g')
libmodopal_la_CXXFLAGS = $(AM_CXXFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[ID][^ ]*//g')

# A per-target _CPPFLAGS replaces AM_CPPFLAGS rather than adding to it, so $(AM_CPPFLAGS)
# is repeated here to keep the core include paths.  The two SWITCH_TEST_BASE_DIR defines
# resolve the suite's FST_CORE_BEGIN("conf_opal") to test/conf_opal in this directory.
# The pkg-config expressions are reused verbatim so the test compiles exactly as the
# module does; that also reproduces the warnings mod_opal.la already emits
# (mod_opal.cpp:173 and :294-295, plus deprecations from the toolkit's own headers).
# They are deliberately not silenced: -Wno-* here would hide real findings and would
# make the test build stop matching the module build.
noinst_PROGRAMS = test/test_mod_opal
test_test_mod_opal_SOURCES = test/test_mod_opal.cpp
test_test_mod_opal_CPPFLAGS = $(AM_CPPFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[^ID][^ ]*//g') -I. -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_opal_CXXFLAGS = $(AM_CXXFLAGS) $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH):$(PKG_DIR) pkg-config opal $(DEBUG_SUFFIX) --cflags | sed 's/-[ID][^ ]*//g')
test_test_mod_opal_LDFLAGS = $(AM_LDFLAGS) -avoid-version -no-undefined $(shell pkg-config opal $(DEBUG_SUFFIX) --libs)
test_test_mod_opal_LDADD = libmodopal.la $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)

# macOS needs the two frameworks on the test program.
if ISMAC
test_test_mod_opal_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
endif
