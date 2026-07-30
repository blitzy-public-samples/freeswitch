include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_xml_curl

mod_LTLIBRARIES = mod_xml_curl.la
mod_xml_curl_la_SOURCES  = mod_xml_curl.c
mod_xml_curl_la_CFLAGS   = $(AM_CFLAGS)
mod_xml_curl_la_CPPFLAGS = $(CURL_CFLAGS) $(AM_CPPFLAGS)
mod_xml_curl_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_xml_curl_la_LDFLAGS  = $(CURL_LIBS) -avoid-version -module -no-undefined -shared

# No convenience library is declared, deliberately.  Every subject under test
# is a file-static function of mod_xml_curl.c -- xml_url_fetch(), do_config()
# and the xml_curl_json_* translator family -- and an archive cannot export a
# file-static symbol.  The suite instead compiles the production translation
# unit into itself (test/test_mod_xml_curl.c includes ../mod_xml_curl.c), the
# white-box technique already used in-tree by
# src/mod/applications/mod_http_cache/test/test_aws.c.  That is also why
# mod_xml_curl.c must not join the sources below, and why the program needs
# nothing extra to link against: a second, separately archived copy of the
# module source would buy nothing.
noinst_PROGRAMS = test/test_mod_xml_curl

# A per-target flag variable REPLACES the global AM_ one rather than adding to
# it, so each of the three below re-states the variable it overrides.  The
# compile flags are split the same way the module target above splits them, so
# that the code under test is compiled exactly as it is compiled for release:
# $(CURL_CFLAGS) belongs on _CPPFLAGS to match the module target's own
# _CPPFLAGS above, and it is load-bearing rather than decorative because
# <switch_curl.h> includes <curl/curl.h>.
# $(AM_CPPFLAGS) is where build/modmake.rulesam routes the core include paths,
# so leaving it out would put <switch.h> beyond reach.  The two
# test-base-directory defines are what make the suite's FST_CORE_BEGIN("conf")
# resolve to this directory's test/conf tree, and its fixture reads to
# test/fixtures, instead of ./conf in whatever directory it is run from.
test_test_mod_xml_curl_SOURCES = test/test_mod_xml_curl.c
test_test_mod_xml_curl_CFLAGS = $(AM_CFLAGS) -I. -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_test_mod_xml_curl_CPPFLAGS = $(CURL_CFLAGS) $(AM_CPPFLAGS)

# Link flags are hand-composed rather than reused from the module target,
# whose LDFLAGS set carries libtool's -module and -shared: library-only
# options that are wrong for an executable.  $(CURL_LIBS) is required rather
# than merely mirrored -- switch_curl.h maps switch_curl_easy_setopt() straight
# onto curl_easy_setopt(), so the module code linked in here calls libcurl
# directly.  Only variables that are actually defined in this Makefile's own
# scope are named: $(freeswitch_LDFLAGS), $(CORE_LIBS) and $(APR_LIBS) are
# defined by the top-level Makefile.am and expand to nothing here, so naming
# them would suggest a link input that does not exist.  $(switch_builddir)/
# libfreeswitch.la carries the core, and $(AM_CPPFLAGS) above already routes
# the matching include paths.
#
# Naming libfreeswitch.la in _LDFLAGS alone would put it on the link line
# without also putting it in the target's prerequisite list, because automake
# derives that list from _LDADD/_LIBADD only.  The test target deliberately
# has no _LDADD (see the no-convenience-library note above), so the build
# order and the rebuild-on-change edge are declared explicitly instead: the
# EXTRA_..._DEPENDENCIES variable is the automake-sanctioned way to add a
# prerequisite that is not a link input of its own, and it is the same core
# library the module target picks up through its _LIBADD.
test_test_mod_xml_curl_LDFLAGS = $(CURL_LIBS) $(AM_LDFLAGS) -avoid-version -no-undefined $(switch_builddir)/libfreeswitch.la
EXTRA_test_test_mod_xml_curl_DEPENDENCIES = $(switch_builddir)/libfreeswitch.la

TESTS = $(noinst_PROGRAMS)
