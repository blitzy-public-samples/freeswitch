include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_h323

mod_LTLIBRARIES = mod_h323.la
mod_h323_la_SOURCES  = mod_h323.cpp
mod_h323_la_CPPFLAGS = $(AM_CPPFLAGS) -I/usr/include/openh323  -I. -DPTRACING=1 -D_REENTRANT
mod_h323_la_CPPFLAGS += -DPTRACING=1 -D_REENTRANT
mod_h323_la_CXXFLAGS = $(AM_CXXFLAGS) -fno-exceptions
mod_h323_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_h323_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -L/usr/lib -lopenh323 -lpt -lrt 

if IS64BITLINUX
mod_h323_la_CPPFLAGS += -DP_64BIT
endif
# No convenience library is declared here, and that is a measured constraint
# rather than a preference.  mod_h323.h:588 defines
# FSH323_T38Capability::CreateChannel() at namespace scope without `inline', so
# every translation unit that includes the header emits a strong definition of
# it.  A noinst_LTLIBRARIES + _LDADD graph necessarily has two such units - the
# archived mod_h323.cpp and the suite itself, which must include the header to
# see FSH323EndPoint - and the link then fails with "multiple definition of
# FSH323_T38Capability::CreateChannel".  Curing that at the root means adding
# `inline' to a production header this engagement freezes, and forcing the link
# instead would leave two live copies of the module's state: the file-static
# H323CapabilityFactory workers H323_REGISTER_CAPABILITY emits would register
# their keys twice, and h323_process would exist twice.
#
# test/test_mod_h323.cpp therefore compiles the production translation unit into
# itself (#include "../mod_h323.cpp").  Consequently neither mod_h323.cpp nor
# $(mod_h323_la_SOURCES) may join _SOURCES below, and _LDADD links only
# libfreeswitch.la: a second compiled copy of the module must never reach the
# link line.
noinst_PROGRAMS = test/test_mod_h323
test_test_mod_h323_SOURCES  = test/test_mod_h323.cpp
test_test_mod_h323_CPPFLAGS = $(mod_h323_la_CPPFLAGS) -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_h323_CXXFLAGS = $(mod_h323_la_CXXFLAGS)
# Compile flags are inherited by reference so they can never drift from the
# module's - a test binary built with different flags is not testing the module
# under test.  One visible consequence: compiling the production unit here
# reproduces the printf warnings mod_h323.la already emits at mod_h323.cpp:2085
# and :2156-2157.  They are not silenced, because -Wno-format would hide a real
# production defect and would make this build stop matching the module build.
# Link flags are hand-composed instead, because the module's own _LDFLAGS carry
# the library-only libtool options -module and -shared.
test_test_mod_h323_LDFLAGS  = $(AM_LDFLAGS) -avoid-version -no-undefined -L/usr/lib -lopenh323 -lpt -lrt
test_test_mod_h323_LDADD    = $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)

if ISMAC
test_test_mod_h323_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
endif
