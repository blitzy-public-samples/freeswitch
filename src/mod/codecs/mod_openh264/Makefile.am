include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_openh264

OPENH264_DIR=/usr/local/

mod_LTLIBRARIES = mod_openh264.la
mod_openh264_la_SOURCES  = mod_openh264.cpp
mod_openh264_la_CXXFLAGS   = $(AM_CXXFLAGS) -I$(OPENH264_DIR)/include/wels
mod_openh264_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_openh264_la_LDFLAGS  = -L$(OPENH264_DIR)/lib/ -lopenh264 -avoid-version -module -no-undefined -shared

noinst_LTLIBRARIES = libmodopenh264.la

libmodopenh264_la_SOURCES  = $(mod_openh264_la_SOURCES)
libmodopenh264_la_CXXFLAGS   = $(AM_CXXFLAGS) -I$(OPENH264_DIR)/include/wels

# The two SWITCH_TEST_BASE_DIR defines are what make this suite's file lookups
# absolute instead of cwd-relative, and they are required rather than tidy.
# Without them switch_test.h falls back to "." (switch_test.h:82 and :93), so
# FST_CORE_BEGIN("conf") resolves to ./conf and FST_MODULE_BEGIN looks for the
# module in ./../.libs/ - both relative to wherever the binary was started.
# Automake's test driver starts it from THIS directory, which has no conf/, and a
# missing configuration root makes switch_xml_init() fail; switch_core_init() then
# takes its error branch and calls fspr_terminate(), which destroys the event queue
# while switch_event_dispatch_thread is still waiting on that queue's condition
# variable, and pthread_cond_destroy() never returns.  `make check' therefore did
# not fail, it hung indefinitely with a zero-byte log and no timeout able to catch
# it.  Every other TESTS-declaring module in the tree defines both, which is why
# only this one was affected.
noinst_PROGRAMS = test/test_mod_openh264
test_test_mod_openh264_SOURCES = test/test_mod_openh264.cpp
test_test_mod_openh264_CXXFLAGS = $(AM_CXXFLAGS) -I. -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_openh264_LDFLAGS = -L$(OPENH264_DIR)/lib/ -lopenh264 -avoid-version -module
test_test_mod_openh264_LDADD = libmodopenh264.la $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)
