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

# FST test harness.  Everything below is additive: the loadable-module target
# above is untouched, and this module stays absent from a default build because
# build/modules.conf.in ships #endpoints/mod_h323 commented out.
#
# No convenience library is declared on purpose.  The suite compiles the
# production translation unit into itself (test/test_mod_h323.cpp includes
# ../mod_h323.cpp), because mod_h323.h defines FSH323_T38Capability::CreateChannel
# at namespace scope without `inline`, so a second archived copy of mod_h323.cpp
# would risk a duplicate strong definition and - were a linker ever to tolerate
# one - two live copies of the module's state (two h323_process objects and a
# doubly-registered H323CapabilityFactory).  For the same reason neither
# mod_h323.cpp nor $(mod_h323_la_SOURCES) may join _SOURCES below.
noinst_PROGRAMS = test/test_mod_h323
test_test_mod_h323_SOURCES  = test/test_mod_h323.cpp
test_test_mod_h323_CPPFLAGS = $(mod_h323_la_CPPFLAGS) -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_h323_CXXFLAGS = $(mod_h323_la_CXXFLAGS)
# Compile flags are inherited by reference so they can never drift from the
# module's; link flags are hand-composed instead, because the module's own
# _LDFLAGS carry the library-only libtool options -module and -shared.
test_test_mod_h323_LDFLAGS  = $(AM_LDFLAGS) -avoid-version -no-undefined -L/usr/lib -lopenh323 -lpt -lrt
test_test_mod_h323_LDADD    = $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)

if ISMAC
test_test_mod_h323_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
endif
