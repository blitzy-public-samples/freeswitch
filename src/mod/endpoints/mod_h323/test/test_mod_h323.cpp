/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2024, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Blitzy Agent <agent@blitzy.com>
 *
 * mod_h323_test -- mod_h323 tests
 *
 */

/*
 * ---------------------------------------------------------------------------
 * HARNESS ARCHITECTURE
 * ---------------------------------------------------------------------------
 *
 * This suite is the first automated test coverage for mod_h323.  It observes
 * the production module; it does not alter it.  Not one line of mod_h323.cpp,
 * mod_h323.h or h323.conf.xml is modified by this work.
 *
 * Subjects under test
 * -------------------
 * The two module entry points, which are the COMPLETE entry-point set for this
 * module: SWITCH_MODULE_DEFINITION(mod_h323, mod_h323_load, mod_h323_shutdown,
 * NULL) passes NULL as its fourth (runtime) argument, so mod_h323 declares no
 * runtime function and no test case here starts a runtime thread.  Alongside
 * those, the suite drives FSH323EndPoint::ReadConfig() and
 * FSH323EndPoint::Initialise() directly.
 *
 * WHY THE PRODUCTION TRANSLATION UNIT IS INCLUDED (and not just the header)
 * ------------------------------------------------------------------------
 * The obvious shape for a C++ module test, and the one the tree's only
 * precedent uses (src/mod/codecs/mod_openh264/Makefile.am:12-21), is a
 * convenience library - noinst_LTLIBRARIES = libmodh323.la - linked in through
 * test_test_mod_h323_LDADD, with the suite including only "../mod_h323.h".
 * That arrangement was prototyped for this module and then measured, and it
 * DOES NOT LINK on this toolchain:
 *
 *     libmodh323.la(mod_h323.o): in function
 *         `FSH323_T38Capability::CreateChannel(...)':
 *     mod_h323.h:593: multiple definition of
 *         `FSH323_T38Capability::CreateChannel(...)';
 *     test_mod_h323.o:mod_h323.h:593: first defined here
 *
 * The cause is in the header, not in this file: mod_h323.h declares
 * FSH323_T38Capability::CreateChannel inside the class (mod_h323.h:568-572)
 * but DEFINES it at namespace scope (mod_h323.h:588-601) without `inline`, so
 * every translation unit that includes the header emits a strong definition of
 * it.  FSH323EndPoint::Initialise() constructs FSH323_T38Capability
 * (mod_h323.cpp:426 and :429), so the archive member carrying the second
 * strong definition is always pulled in to satisfy the vtable.
 *
 * The sanctioned remedy is to compile the module source INTO the test
 * translation unit, which yields exactly one definition of that member and no
 * archive member to collide with.  It also removes a second, quieter hazard:
 * mod_h323.h instantiates H323_REGISTER_CAPABILITY nine times at namespace
 * scope (mod_h323.h:620-628), and two translation units would register the
 * same capability NAME STRINGS into the global H323CapabilityFactory twice,
 * making AddAllCapabilities() (mod_h323.cpp:404) add duplicates.  One
 * translation unit registers each name once.
 *
 * ==> ADOPTED BUILD CONTRACT - REQUIRED OF THE PARENT Makefile.am
 *     ------------------------------------------------------------
 *     White-box source inclusion is formally ADOPTED here, not tolerated as a
 *     workaround.  It is the AAP's own documented contingency for exactly this
 *     situation: the suite "switches to white-box source inclusion ... and its
 *     convenience library declaration is dropped" (AAP 0.3.1), described there
 *     as "a local change to one Makefile.am and one #include line".  The
 *     #include line is the `#include "../mod_h323.cpp"` below.  The Makefile.am
 *     change is a separate work boundary that this file must not edit, so the
 *     contract it has to satisfy is stated here as a REQUIREMENT rather than
 *     being left implicit:
 *
 *       R1. src/mod/endpoints/mod_h323/Makefile.am must NOT declare a
 *           convenience library for this module.  There must be no
 *           `noinst_LTLIBRARIES = libmodh323.la`.
 *       R2. libmodh323.la must NOT appear in test_test_mod_h323_LDADD, and
 *           neither mod_h323.cpp nor $(mod_h323_la_SOURCES) may appear in
 *           test_test_mod_h323_SOURCES.  The sole source of this program is
 *           test/test_mod_h323.cpp, which already carries the module.
 *
 *     Together R1 and R2 guarantee the property this architecture depends on:
 *     EXACTLY ONE production copy of mod_h323 is compiled into, and linked
 *     into, the test program.  Violating either requirement reintroduces the
 *     mod_h323.h:593 collision quoted above; and if a linker were ever
 *     persuaded to tolerate that collision, the result would be two live
 *     copies of the module's state - two `h323_process` objects and a
 *     doubly-populated H323CapabilityFactory - which is a worse failure than a
 *     link error because it is silent.
 *
 *     Everything else the production target carries must still be carried by
 *     the test target, because a binary built with different flags is not
 *     testing the same code: the openh323 include path, -DPTRACING=1,
 *     -D_REENTRANT, -fno-exceptions, the -D_64BIT/-DP_64BIT variant under the
 *     64-bit Linux conditional, the openh323/PTLib link flags
 *     (-L/usr/lib -lopenh323 -lpt -lrt), and
 *     $(switch_builddir)/libfreeswitch.la.
 *
 * WHAT THE INCLUSION IS AND IS NOT USED FOR
 * -----------------------------------------
 * Compiling the module translation unit in is a LINKING necessity, established
 * above.  It is deliberately NOT used as a licence to assert on the module's
 * internal state.  Single-translation-unit compilation does make the
 * header-static `h323_process` (mod_h323.h:630) and the .cpp-file static
 * `mod_h323_globals` (mod_h323.cpp:42) reachable from here, but no verdict in
 * this suite rests on either of them, because an assertion that depends on
 * internal linkage breaks on refactors that change nothing observable.
 *
 * Every case's verdict is instead taken from a public seam:
 *
 *   - FSH323EndPoint::ReadConfig()'s returned switch_status_t;
 *   - the public FSH323EndPoint::m_listeners list (mod_h323.h:270);
 *   - the public m_ai / m_pi / m_endpointname members (mod_h323.h:271-278);
 *   - protected members reached legitimately through the test-local subclass
 *     FSH323TestEndPoint, per rule R2-2 - no friend declaration, no
 *     de-staticising, no production edit;
 *   - PProcess::IsInitialised() and PProcess::Current(), PTLib's own public
 *     singleton interface, with the FSProcess handle recovered by the POINTER
 *     form of dynamic_cast so it returns NULL rather than throwing under
 *     -fno-exceptions;
 *   - FSProcess::GetH323EndPoint() (mod_h323.h:230-232) and
 *     FSH323EndPoint::GetSwitchInterface() (mod_h323.h:266-268);
 *   - the switch_loadable_module_interface_t the load function returns, and its
 *     endpoint_interface.
 *
 * Where a property exists only behind internal linkage - the four global
 * strings, the context and dialplan defaults - it is left unasserted and the
 * reason is recorded at the site, rather than reached for because it happens to
 * be visible.
 *
 * BOOTSTRAP TIER
 * --------------
 * FST_CORE_BEGIN + FST_SUITE_BEGIN, never the module-loading bootstrap tier
 * (switch_test.h:346-364).  A real core is
 * mandatory because switch_xml_open_cfg() asserts MAIN_XML_ROOT != NULL
 * (src/switch_xml.c:2541), and FSH323EndPoint::ReadConfig() reaches it.
 * That tier is unusable here for two independent reasons: it dlopens the
 * module from <confdir>/../.libs/, which would load a second copy of code
 * already linked into this binary, and its matching end macro
 * (switch_test.h:379-387) performs an UNASSERTED unload - whereas this suite
 * must assert the shutdown status.  For
 * the same reason the teardown hook never unloads anything: the harness never
 * dlopens or dlcloses the module at all.
 *
 * PProcess SINGLETON DISCIPLINE
 * -----------------------------
 * PTLib allows at most one live PProcess-derived object per process, and
 * H323EndPoint's constructor calls PProcess::Current().GetUserName()
 * (h323ep.cxx:713), which _exit(1)s the process when no PProcess exists.  A
 * single FSProcess is therefore shared by the cases that construct an endpoint
 * directly and released by the last of them, so two are never alive at once.
 * The reason it is SHARED rather than per-case is a measured, irreversible
 * PTLib property spelt out in full beside the ownership helpers below.
 *
 * NETWORK SIDE-EFFECT CONTAINMENT
 * -------------------------------
 * FSH323EndPoint::Initialise() starts a listener per configured address
 * (mod_h323.cpp:441-448) and, when the gatekeeper address is non-empty, a live
 * RAS registration thread (mod_h323.cpp:451-455).  Critically, an EMPTY
 * listener list makes it fall back to StartListener("") - a WILDCARD bind on
 * port 1720.  Every injected configuration in this file therefore carries an
 * explicit <listeners> stanza bound to 127.0.0.1 on a fixed high port, so that
 * whichever document a case happens to reach Initialise() with, the wildcard
 * fallback is unreachable.  The shipped sample's $${local_ip_v4} and port 1720
 * (h323.conf.xml:23-28) are reference values only and are never used as-is.
 *
 * The gatekeeper thread is contained by a narrower rule: EVERY configuration
 * this suite lets reach Initialise(), whether called directly or through module
 * load, carries an empty gk-address, so the guard at mod_h323.cpp:451 is false
 * and no RAS thread is ever constructed.  Exactly one document departs from
 * that - fst_h323_conf_gk_lan_search, with gk-address="*" - and it is
 * deliberately confined to ReadConfig(), which only stores the value
 * (mod_h323.cpp:537-538).  Its case asserts the stored string and never calls
 * Initialise(), so "*" is observed but never acted on.
 * No case performs third-party network I/O, opens a random port, or depends on
 * wall-clock time.
 */

#include <switch.h>
#include <test/switch_test.h>
#include "../mod_h323.cpp"

/*
 * The module entry points are non-static with C linkage: mod_h323.cpp wraps
 * them in SWITCH_BEGIN_EXTERN_C / SWITCH_END_EXTERN_C (mod_h323.cpp:146 and
 * :201) and SWITCH_MODULE_LOAD_FUNCTION expands to a plain definition with no
 * storage class (switch_types.h:2611).  Restating the contract here documents
 * exactly what this suite calls, and keeps the file correct if it is ever
 * switched back to including only the header.
 */
SWITCH_BEGIN_EXTERN_C
SWITCH_MODULE_LOAD_FUNCTION(mod_h323_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_h323_shutdown);
SWITCH_END_EXTERN_C

/*
 * Test-local subclass: the only sanctioned route to FSH323EndPoint's protected
 * members (mod_h323.h:273-287).  Nothing is de-staticised, no symbol is
 * re-exported and no `friend` declaration is added to the production header -
 * this is ordinary C++ derived-class access.  m_listeners, m_ai, m_pi,
 * ReadConfig(), Initialise() and GetSwitchInterface() are already public and
 * need no help from this class.
 */
class FSH323TestEndPoint:public FSH323EndPoint {
  public:
	FSH323TestEndPoint():FSH323EndPoint()
	{
	}

	/* gk-address: "" disables gatekeeper registration, "*" searches the LAN */
	const PString & TestGetGkAddress() const
	{
		return m_gkAddress;
	}

	/* NOTE: `gk-identifer` is misspelled identically in h323.conf.xml:11 and
	 * in mod_h323.cpp:539.  The pair AGREES, so the misspelling is
	 * load-bearing and is reproduced verbatim by this suite. */
	const PString & TestGetGkIdentifer() const
	{
		return m_gkIdentifer;
	}

	const PString & TestGetGkInterface() const
	{
		return m_gkInterface;
	}

	const PString & TestGetEndpointName() const
	{
		return m_endpointname;
	}

	const PStringList & TestGetGkPrefixes() const
	{
		return m_gkPrefixes;
	}

	int TestGetGkRetry() const
	{
		return m_gkretry;
	}

	/* NULL whenever gatekeeper registration was never initiated */
	FSGkRegThread *TestGetGkRegistrationThread() const
	{
		return m_thread;
	}

	bool TestGetFastStart() const
	{
		return m_faststart;
	}

	bool TestGetH245Tunneling() const
	{
		return m_h245tunneling;
	}
};

/*
 * Loopback-only, high-unprivileged listener ports.  Fixed rather than random
 * so the suite is deterministic and reproducible.  Only the two ports used by
 * cases that run Initialise() are ever actually bound; the rest are parsed and
 * never opened.
 */
#define FST_H323_PORT_CODEC_PREFS   "21721"	/* bound: Initialise() runs   */
#define FST_H323_PORT_MODULE_LOAD   "21722"	/* bound: module load runs    */
#define FST_H323_PORT_GK_DISABLED   "21723"	/* parsed only, never opened  */
#define FST_H323_PORT_GK_LAN        "21724"	/* parsed only, never opened  */
#define FST_H323_PORT_LISTENER_ONE  "21725"	/* parsed only, never opened  */
#define FST_H323_PORT_LISTENER_TWO  "21726"	/* parsed only, never opened  */

#define FST_H323_LOOPBACK           "127.0.0.1"

/* The configuration file name mod_h323 asks for (mod_h323.cpp:464) */
#define FST_H323_CONF_FILE          "h323.conf"

/*
 * Injected configuration documents.
 *
 * These carry the FULL <document>/<section name="configuration"> envelope.
 * That is mandatory and is NOT optional decoration: switch_xml_locate()
 * resolves a lookup with switch_xml_find_child(xml, "section", "name",
 * section) AND switch_xml_find_child(conf, tag_name, key_name, key_value)
 * (src/switch_xml.c:1850), and BOTH must succeed.  The shipped
 * h323.conf.xml is a bare <configuration> element with no envelope, so it can
 * never be located and is not usable as a binding payload.
 *
 * Every document is a file-scope string literal, so it outlives any binding
 * that references it and the binding callback can re-parse it on demand.
 */

/* CASE: empty gk-address.  endpoint-name is deliberately OMITTED so that the
 * hard-coded default "FreeSwitch" (mod_h323.cpp:492) is observable. */
static const char fst_h323_conf_gk_disabled[] =
	"<document type=\"freeswitch/xml\">"
	"  <section name=\"configuration\">"
	"    <configuration name=\"h323.conf\" description=\"H323 Endpoints\">"
	"      <settings>"
	"        <param name=\"context\" value=\"default\"/>"
	"        <param name=\"dialplan\" value=\"XML\"/>"
	"        <param name=\"codec-prefs\" value=\"PCMA,PCMU\"/>"
	"        <param name=\"gk-address\" value=\"\"/>"
	"        <param name=\"gk-identifer\" value=\"\"/>"
	"        <param name=\"gk-interface\" value=\"\"/>"
	"      </settings>"
	"      <listeners>"
	"        <listener name=\"fst-gk-disabled\">"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_GK_DISABLED "\"/>"
	"        </listener>"
	"      </listeners>"
	"    </configuration>"
	"  </section>"
	"</document>";

/* CASE: gk-address "*" requests a LAN gatekeeper search.  Initialise() is
 * never run against this document, so no RAS registration thread starts. */
static const char fst_h323_conf_gk_lan_search[] =
	"<document type=\"freeswitch/xml\">"
	"  <section name=\"configuration\">"
	"    <configuration name=\"h323.conf\" description=\"H323 Endpoints\">"
	"      <settings>"
	"        <param name=\"context\" value=\"default\"/>"
	"        <param name=\"dialplan\" value=\"XML\"/>"
	"        <param name=\"gk-address\" value=\"*\"/>"
	"        <param name=\"gk-identifer\" value=\"fst-gatekeeper\"/>"
	"        <param name=\"gk-interface\" value=\"\"/>"
	"        <param name=\"gk-retry\" value=\"45\"/>"
	"        <param name=\"gk-prefix\" value=\"1234\"/>"
	"      </settings>"
	"      <listeners>"
	"        <listener name=\"fst-gk-lan\">"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_GK_LAN "\"/>"
	"        </listener>"
	"      </listeners>"
	"    </configuration>"
	"  </section>"
	"</document>";

/* CASE: listener parsing.  Two listeners - the first named, the second with
 * no name attribute at all so that the "unnamed" default (mod_h323.cpp:567)
 * is observable.  Also overrides endpoint-name, progress-indication and
 * alerting-indication away from their hard-coded defaults. */
static const char fst_h323_conf_listeners[] =
	"<document type=\"freeswitch/xml\">"
	"  <section name=\"configuration\">"
	"    <configuration name=\"h323.conf\" description=\"H323 Endpoints\">"
	"      <settings>"
	"        <param name=\"context\" value=\"default\"/>"
	"        <param name=\"dialplan\" value=\"XML\"/>"
	"        <param name=\"gk-address\" value=\"\"/>"
	"        <param name=\"endpoint-name\" value=\"fs\"/>"
	"        <param name=\"progress-indication\" value=\"16\"/>"
	"        <param name=\"alerting-indication\" value=\"8\"/>"
	"        <param name=\"faststart\" value=\"false\"/>"
	"        <param name=\"h245tunneling\" value=\"false\"/>"
	"      </settings>"
	"      <listeners>"
	"        <listener name=\"fst-loopback\">"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_LISTENER_ONE "\"/>"
	"        </listener>"
	"        <listener>"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_LISTENER_TWO "\"/>"
	"        </listener>"
	"      </listeners>"
	"    </configuration>"
	"  </section>"
	"</document>";

/* CASE: codec preference order.  The value is the shipped sample's
 * codec-prefs string (h323.conf.xml:6) verbatim.  This document IS used with
 * Initialise(), hence the loopback listener and the empty gk-address. */
static const char fst_h323_conf_codec_prefs[] =
	"<document type=\"freeswitch/xml\">"
	"  <section name=\"configuration\">"
	"    <configuration name=\"h323.conf\" description=\"H323 Endpoints\">"
	"      <settings>"
	"        <param name=\"context\" value=\"default\"/>"
	"        <param name=\"dialplan\" value=\"XML\"/>"
	"        <param name=\"codec-prefs\" value=\"PCMA,PCMU,GSM,G729\"/>"
	"        <param name=\"gk-address\" value=\"\"/>"
	"        <param name=\"endpoint-name\" value=\"fs\"/>"
	"      </settings>"
	"      <listeners>"
	"        <listener name=\"fst-codec-prefs\">"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_CODEC_PREFS "\"/>"
	"        </listener>"
	"      </listeners>"
	"    </configuration>"
	"  </section>"
	"</document>";

/* CASE: full module load.  This document IS used with mod_h323_load(), which
 * reaches Initialise(), hence the loopback listener and empty gk-address. */
static const char fst_h323_conf_module_load[] =
	"<document type=\"freeswitch/xml\">"
	"  <section name=\"configuration\">"
	"    <configuration name=\"h323.conf\" description=\"H323 Endpoints\">"
	"      <settings>"
	"        <param name=\"context\" value=\"default\"/>"
	"        <param name=\"dialplan\" value=\"XML\"/>"
	"        <param name=\"codec-prefs\" value=\"PCMA,PCMU\"/>"
	"        <param name=\"gk-address\" value=\"\"/>"
	"        <param name=\"endpoint-name\" value=\"fs\"/>"
	"      </settings>"
	"      <listeners>"
	"        <listener name=\"fst-module-load\">"
	"          <param name=\"h323-ip\" value=\"" FST_H323_LOOPBACK "\"/>"
	"          <param name=\"h323-port\" value=\"" FST_H323_PORT_MODULE_LOAD "\"/>"
	"        </listener>"
	"      </listeners>"
	"    </configuration>"
	"  </section>"
	"</document>";

/*
 * ---------------------------------------------------------------------------
 * CONFIGURATION INJECTION
 * ---------------------------------------------------------------------------
 *
 * Configuration is supplied through the same public XML binding API that
 * mod_xml_curl itself registers with (switch_xml_bind_search_function_ret,
 * switch_xml.h:429).  switch_xml_locate() consults the registered bindings
 * before the static root (src/switch_xml.c:1806-1848), so one binary can
 * present a different h323.conf to each test case.
 *
 * The alternative - replacing the XML root wholesale through the
 * root-replacement hook declared at switch_xml.h:345 - is deliberately not
 * used: it has no callers anywhere in the tree and would amount to untested
 * code propping up a test.
 *
 * Note what a binding CANNOT do: on a miss, switch_xml_locate() frees the
 * candidate document and retries against the static root
 * (src/switch_xml.c:1861-1869).  A binding therefore cannot suppress a
 * static-root hit.  That is exactly why the configuration-absent failure case
 * works by OMISSION - the suite's fixture root (test/conf_h323/freeswitch.xml)
 * deliberately contains no <configuration name="h323.conf"> child, so with no
 * binding registered both the binding list and the static root miss.
 */

SWITCH_BEGIN_EXTERN_C

/*
 * The suite's h323.conf provider.
 *
 * Three properties of this callback are load-bearing:
 *
 *   1. It returns a FRESHLY PARSED document on EVERY invocation.  The caller
 *      owns what it is handed: switch_xml_locate() frees it on a miss
 *      (src/switch_xml.c:1862) and ReadConfig() frees it on a hit
 *      (mod_h323.cpp:590-591).  Returning a cached pointer twice would be a
 *      double free.  The document TEXT is passed as user_data, so re-parsing
 *      is all this function has to do.
 *
 *   2. It is gated on key_value.  Without that gate it would hijack every
 *      other configuration lookup - modules.conf and console.conf during core
 *      bootstrap included - and destabilise the whole suite.
 *
 *   3. It is registered with a "configuration" section mask; a mis-masked
 *      binding is silently skipped (src/switch_xml.c:1809-1811).
 */
static switch_xml_t fst_h323_config_search(const char *section, const char *tag_name, const char *key_name,
										   const char *key_value, switch_event_t *params, void *user_data)
{
	const char *document = (const char *) user_data;

	/* The requested document is selected entirely by the four gates below and by
	   the template handed in through user_data, so the event parameters carry
	   nothing this provider needs. */
	(void) params;

	/* Answer for exactly one lookup: the h323.conf configuration section */
	if (zstr(section) || strcasecmp(section, "configuration")) {
		return NULL;
	}

	if (zstr(tag_name) || strcasecmp(tag_name, "configuration")) {
		return NULL;
	}

	if (zstr(key_name) || strcasecmp(key_name, "name")) {
		return NULL;
	}

	if (zstr(key_value) || strcasecmp(key_value, FST_H323_CONF_FILE)) {
		return NULL;
	}

	if (zstr(document)) {
		return NULL;
	}

	/*
	 * switch_xml_parse_str_dynamic() takes a non-const char * because
	 * switch_xml_parse_str() rewrites its buffer in place.  With dup ==
	 * SWITCH_TRUE it duplicates first (src/switch_xml.c:1075) and frees the
	 * duplicate from switch_xml_free(), so passing a pointer to immutable
	 * storage is safe and no allocation is leaked on the failure path.
	 */
	return switch_xml_parse_str_dynamic((char *) document, SWITCH_TRUE);
}

SWITCH_END_EXTERN_C

/*
 * Register the provider above for the "configuration" section.  `document`
 * must outlive the binding; every caller passes a file-scope string literal.
 */
static switch_status_t fst_h323_bind_config(const char *document)
{
	switch_xml_binding_t *binding = NULL;
	switch_xml_section_t sections;

	if (zstr(document)) {
		return SWITCH_STATUS_FALSE;
	}

	sections = switch_xml_parse_section_string("configuration");

	if (!(sections & SWITCH_XML_SECTION_CONFIG)) {
		/* Would be silently skipped by switch_xml_locate() - refuse instead */
		return SWITCH_STATUS_FALSE;
	}

	return switch_xml_bind_search_function_ret(fst_h323_config_search, sections, (void *) document, &binding);
}

/*
 * Remove the provider.  The function-pointer variant is the correct one here:
 * the handle variant (switch_xml.h:434) takes a switch_xml_binding_t ** that
 * this suite deliberately does not retain, and exactly one binding with this
 * unique function pointer is ever registered at a time.
 */
static switch_status_t fst_h323_unbind_config(void)
{
	return switch_xml_unbind_search_function_ptr(fst_h323_config_search);
}

/*
 * ---------------------------------------------------------------------------
 * CAPABILITY-TABLE OBSERVATION
 * ---------------------------------------------------------------------------
 *
 * mod_h323_globals is a .cpp-file static (mod_h323.cpp:42), so the codec
 * assertion is made through the public H323EndPoint capability API instead of
 * peeking at the stored preference string.
 *
 * Serialising the table into one '|' delimited string lets the assertions be
 * made on PRESENCE (substring) and RELATIVE ORDER (substring position) rather
 * than on an absolute capability count, which is not a stable observable:
 * H323Plus decorates registered capability names with "{sw}" / "{hw}"
 * suffixes, and AddAllCapabilities() (mod_h323.cpp:404) adds however many
 * factory entries match each wildcard.
 */
static void fst_h323_capability_table(const H323Capabilities & capabilities, char *buf, switch_size_t buflen)
{
	PINDEX size;
	PINDEX i;
	switch_size_t used;

	switch_assert(buf != NULL);
	switch_assert(buflen > 1);

	buf[0] = '\0';
	used = 0;
	size = capabilities.GetSize();

	for (i = 0; i < size; i++) {
		/* GetFormatName() returns by value - hold it in a named local so the
		 * character pointer taken from it is not a dangling temporary. */
		PString name = capabilities[i].GetFormatName();
		const char *cname = (const char *) name;
		int written;

		if (zstr(cname)) {
			continue;
		}

		written = switch_snprintf(buf + used, buflen - used, "|%s", cname);

		if (written <= 0) {
			break;				/* buffer exhausted - stop cleanly */
		}

		used += (switch_size_t) written;

		if (used + 1 >= buflen) {
			break;
		}
	}
}

/*
 * Byte offset of `needle` within `haystack`, or -1 when absent.  Used to
 * compare the relative order of capability names.
 */
static int fst_h323_name_position(const char *haystack, const char *needle)
{
	const char *found;

	if (zstr(haystack) || zstr(needle)) {
		return -1;
	}

	found = strstr(haystack, needle);

	if (!found) {
		return -1;
	}

	return (int) (found - haystack);
}

/*
 * ---------------------------------------------------------------------------
 * PTLib PROCESS OWNERSHIP
 * ---------------------------------------------------------------------------
 *
 * PTLib permits at most ONE live PProcess-derived object per process, and
 * H323EndPoint's constructor calls PProcess::Current().GetUserName()
 * (h323ep.cxx:713) - which prints "Catastrophic failure" and _exit(1)s when no
 * PProcess exists.  So every case that constructs an endpoint needs exactly one
 * live PProcess, and never two.
 *
 * That alone would allow each case to own a private FSProcess.  A second,
 * measured property of PTLib rules that out:
 *
 *   PProcess::~PProcess() runs PostShutdown() (osutils.cxx:1646-1654), which
 *   calls DestroySingletons() on EVERY PFactory.  That IRREVERSIBLY empties
 *   both the OpalMediaFormat registry and the H323CapabilityFactory key list -
 *   measured going from 15 media formats and 11 capability keys down to 0 and
 *   0, and they are never repopulated, because the objects that register them
 *   are static and their constructors ran once at program start.
 *
 * The consequence is decisive for the codec case: AddAllCapabilities()
 * (mod_h323.cpp:404) enumerates that factory and validates each match against
 * the media-format registry, so once ANY PProcess has been destroyed it can
 * never add an audio capability again.  A per-case FSProcess would leave the
 * codec case observing an empty capability table - not because mod_h323 is
 * wrong, but because the harness had already burnt down the registry.
 *
 * Therefore ONE FSProcess is shared by the direct-object cases and released by
 * the last of them, immediately before the case that lets the module create its
 * own.  This is deliberate, is not a resource deferred to teardown - the
 * release is an explicit statement inside a test case - and it keeps the
 * one-live-PProcess invariant at every instant.
 */
static FSProcess *fst_h323_process = NULL;

/*
 * Return the shared PProcess, creating it on first use.  Under -fno-exceptions
 * `new` aborts rather than returning NULL, so the NULL check is belt-and-braces
 * in the same style the production module uses (mod_h323.cpp:169-171).
 */
static FSProcess *fst_h323_process_acquire(void)
{
	if (!fst_h323_process) {
		fst_h323_process = new FSProcess();
	}

	return fst_h323_process;
}

/*
 * Release the shared PProcess.  Safe to call when nothing is held, which is
 * what lets the module-load case defensively clear any instance left behind by
 * an earlier case that exited on a fatal check.
 */
static void fst_h323_process_release(void)
{
	if (fst_h323_process) {
		delete fst_h323_process;
		fst_h323_process = NULL;
	}
}

/*
 * Release the H323ListenerTCP objects that a direct ReadConfig() left the test
 * owning, and empty the record list.
 *
 * WHY THIS IS NEEDED AT ALL
 * -------------------------
 * FSH323EndPoint::ReadConfig() allocates one listener per <listener> element
 * unconditionally - `listener.listenAddress = new H323ListenerTCP(*this, ip,
 * port)` (mod_h323.cpp:583) - and stores it as a RAW pointer inside the
 * FSListener record it appends to m_listeners.  FSListener has a defaulted
 * constructor that leaves listenAddress uninitialised and NO destructor
 * (mod_h323.h:236-243), and ~FSH323EndPoint only calls StopGkClient() and
 * ClearAllCalls() (mod_h323.cpp:612-617).  So `delete endpoint` alone does not
 * free them, and a case that reads configuration without going on to
 * Initialise() is the sole owner of everything ReadConfig() constructed.
 * Freeing it here is the test cleaning up after itself - it is NOT a
 * workaround for a production defect, because in production ReadConfig() is
 * only ever reached from Initialise(), which hands each listener straight to
 * StartListener().
 *
 * WHY IT MUST NOT BE CALLED AFTER Initialise()
 * --------------------------------------------
 * H323EndPoint::StartListener(H323Listener *) documents the transfer
 * explicitly: "if this returns TRUE, then the endpoint is responsible for
 * deleting the H323Listener listener object.  If FALSE is returned then the
 * object is not deleted and it is up to the caller to release the memory"
 * (h323ep.h:532-545).  Initialise() calls it for every record
 * (mod_h323.cpp:441-448), so once Initialise() has succeeded the listeners
 * belong to the base endpoint's H323ListenerList (h323ep.h:3114) and are freed
 * by ~H323EndPoint.  Calling this helper there would be a DOUBLE FREE.  It is
 * therefore invoked by the three ReadConfig()-only cases and by no other,
 * which was confirmed by measurement: valgrind attributes four definitely-lost
 * H323ListenerTCP blocks to exactly those three cases and none to the
 * Initialise() case.
 */
static void fst_h323_release_unstarted_listeners(FSH323TestEndPoint * endpoint)
{
	if (!endpoint) {
		return;
	}

	for (std::list < FSListener >::iterator it = endpoint->m_listeners.begin(); it != endpoint->m_listeners.end(); ++it) {
		delete it->listenAddress;
		it->listenAddress = NULL;
	}

	endpoint->m_listeners.clear();
}

/*
 * The memory pool the loaded module is given, and the reason it is not fst_pool.
 *
 * WHY A DEDICATED POOL IS REQUIRED
 * --------------------------------
 * mod_h323_load() does not merely read its pool argument, it allocates
 * long-lived state out of it: switch_loadable_module_create_module_interface()
 * (mod_h323.cpp:159) carves the module interface, and every interface hung off
 * it, straight out of that pool, and the heap FSProcess/FSH323EndPoint the load
 * constructs keeps pointing at the result for as long as the module is loaded.
 *
 * fst_pool cannot serve that purpose.  It is created fresh by FST_SETUP_BEGIN
 * and destroyed by FST_TEARDOWN_BEGIN (switch_test.h:407-414 and 425-432), so
 * its lifetime is exactly ONE test case.  This suite deliberately spans two:
 * case 6 loads the module and case 7 asserts that shutting it down works.  Had
 * case 6 loaded with fst_pool, that pool would already have been destroyed by
 * the time case 7 ran, and mod_h323_shutdown() would have been asked to tear
 * down a module whose interface memory was freed underneath it - a
 * use-after-free across a case boundary, and a hard build failure under the
 * address sanitizer the CI configure line always enables (ci.sh:76-79).
 *
 * So the pool that backs retained module state is created explicitly before the
 * load, outlives the case that created it, and is destroyed only after shutdown
 * has been asserted.  Per-case observations continue to use fst_pool; only
 * state intentionally retained BETWEEN cases lives here.
 */
static switch_memory_pool_t *fst_h323_module_pool = NULL;

/*
 * Create the module-lifetime pool.  Returns SWITCH_STATUS_SUCCESS only when a
 * usable pool is available afterwards, so the caller can make it a hard
 * precondition of loading.  Calling it twice without an intervening destroy is
 * treated as success and does not leak, because the existing pool is kept.
 */
static switch_status_t fst_h323_module_pool_create(void)
{
	if (fst_h323_module_pool) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_core_new_memory_pool(&fst_h323_module_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return fst_h323_module_pool ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*
 * Destroy the module-lifetime pool.  MUST NOT be called until the module has
 * been shut down, because shutdown is the last thing that touches pool-backed
 * module state.  switch_core_destroy_memory_pool() NULLs the caller's pointer,
 * so this is safe to call when nothing is held.
 */
static void fst_h323_module_pool_destroy(void)
{
	if (fst_h323_module_pool) {
		switch_core_destroy_memory_pool(&fst_h323_module_pool);
	}
}


/*
 * ---------------------------------------------------------------------------
 * THE SUITE
 * ---------------------------------------------------------------------------
 *
 * "conf_h323" names this module's own fixture root.  The core bootstrap builds
 * the configuration directory as SWITCH_TEST_BASE_DIR_FOR_CONF, a path
 * separator, then that name (switch_test.h:92-93), so the target's Makefile.am
 * declaration must define -DSWITCH_TEST_BASE_DIR_FOR_CONF and
 * -DSWITCH_TEST_BASE_DIR_OVERRIDE to ${abs_builddir}/test - the same pair
 * carried by, for example, src/mod/applications/mod_commands/Makefile.am:15 -
 * for the name to resolve to src/mod/endpoints/mod_h323/test/conf_h323.
 * Without those defines the bootstrap falls back to "./conf_h323"
 * (switch_test.h:94-99), which resolves against the working directory instead,
 * so the suite would only find its fixtures when run from inside test/.  See
 * the build-wiring note in the file header for the rest of that declaration.
 *
 * CASE ORDER IS LOAD-BEARING.  FCTX runs cases in declaration order, and the
 * order below is the only one that satisfies every constraint at once:
 *
 *   1. readconfig_without_configuration_fails ....... FIRST: observes the
 *      configuration-absent branch before any successful load has left module
 *      state behind.
 *   2. gatekeeper_registration_disabled_...  \
 *   3. gatekeeper_lan_search_address_...      >  socket-free, direct-object
 *   4. listeners_parsed_from_configuration   /   cases: ReadConfig() only.
 *   5. codec_prefs_negotiation_order ................ first case to run
 *      Initialise(), so the first to bind a socket.
 *   6. module_load_registers_endpoint_interface ..... full module load.
 *   7. module_shutdown_releases_resources ........... LAST: observes a fully
 *      initialised module and reclaims what case 6 allocated.
 *
 * Cases 1-5 share one FSProcess, which case 5 releases; case 6 then lets the
 * module create its own and case 7 asserts the module gives it back.  At no
 * instant are two PProcess-derived objects alive.
 */
FST_CORE_BEGIN("conf_h323")
{
	FST_SUITE_BEGIN(mod_h323)
	{
		/*
		 * These two hooks must be PRESENT even though they are empty.
		 * FST_CORE_BEGIN sets fst_core == 2, and every FST_TEST_BEGIN then
		 * fatally requires both fst_pool and a started fst_timer
		 * (switch_test.h:451-453).  FST_SETUP_BEGIN is the only thing that
		 * creates them (switch_test.h:407-412), so omitting it would make
		 * every case below fail fatally.
		 */
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		/*
		 * Deliberately empty.  The harness never dlopens or dlcloses
		 * mod_h323, so there is nothing here to unload - and every per-case
		 * resource (binding, endpoint, PProcess, module interface) is
		 * released inside the case that created it.
		 */
		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * CASE 1 - the configuration-absent failure branch.  DECLARED FIRST.
		 *
		 * Asserted on ReadConfig() directly, and deliberately NOT on
		 * mod_h323_load(): FSH323EndPoint::Initialise() discards ReadConfig()'s
		 * status (mod_h323.cpp:381) and returns TRUE unconditionally
		 * (mod_h323.cpp:457), so mod_h323_load() can never report a
		 * configuration failure and an assertion on it would prove nothing.
		 *
		 * Socket-free: ReadConfig() only CONSTRUCTS H323ListenerTCP objects
		 * (mod_h323.cpp:583); OpenH323 binds in H323ListenerTCP::Open(), which
		 * is reached from H323EndPoint::StartListener() and which ReadConfig()
		 * never calls.  On this path nothing is even constructed.
		 */
		FST_TEST_BEGIN(readconfig_without_configuration_fails)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_SUCCESS;

			/* Fatal checks are made only BEFORE anything else is allocated:
			 * fst_requires breaks out of the case body, which would skip the
			 * cleanup below. Everything after an allocation is non-fatal. */
			fst_requires(fst_h323_process_acquire() != NULL);
			fst_requires(PProcess::IsInitialised());

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			/* No binding is registered, and test/conf_h323/freeswitch.xml
			 * deliberately carries no <configuration name="h323.conf"> child,
			 * so both the binding list and the static root miss,
			 * switch_xml_open_cfg() (mod_h323.cpp:482) returns NULL and
			 * ReadConfig() takes its error branch at mod_h323.cpp:484-487. */
			status = endpoint->ReadConfig(0);

			fst_check(status == SWITCH_STATUS_FALSE);

			/* Nothing was parsed, so no listener was appended */
			fst_check(endpoint->m_listeners.empty());

			/* m_pi, m_ai and m_endpointname are deliberately NOT asserted on
			 * this path: their defaults are applied at mod_h323.cpp:490-493,
			 * AFTER the failure return, and the constructor
			 * (mod_h323.cpp:599-610) leaves the two ints uninitialised.
			 *
			 * The context and dialplan defaults applied at mod_h323.cpp:474-475
			 * are likewise NOT asserted, deliberately.  They live in
			 * mod_h323_globals, a .cpp-file static (mod_h323.cpp:42) whose only
			 * setters are the file-static SWITCH_DECLARE_GLOBAL_STRING_FUNC
			 * wrappers (mod_h323.cpp:44-47), so no public seam exposes them.  An
			 * assertion on them would rest purely on this suite's internal
			 * linkage to the module translation unit, and asserting through
			 * internal linkage is what makes a harness brittle: it breaks on a
			 * refactor that changes nothing observable.
			 *
			 * The stronger property is asserted on the public seams instead: the
			 * failure branch is deterministic and leaves no residue, so repeating
			 * it produces the identical observable result. */
			status = endpoint->ReadConfig(0);

			fst_check(status == SWITCH_STATUS_FALSE);
			fst_check(endpoint->m_listeners.empty());

			delete endpoint;

			/* the shared PProcess is deliberately retained for the next case */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 2 - an empty gk-address leaves gatekeeper registration
		 * disabled.  Socket-free: ReadConfig() only.
		 */
		FST_TEST_BEGIN(gatekeeper_registration_disabled_when_gk_address_empty)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(fst_h323_process_acquire() != NULL);
			fst_requires(fst_h323_bind_config(fst_h323_conf_gk_disabled) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			status = endpoint->ReadConfig(0);

			/* The binding answered, so the configuration was located */
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* An explicitly empty gk-address is stored empty ... */
			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check_string_equals((const char *) endpoint->TestGetGkAddress(), "");
			fst_check(endpoint->TestGetGkIdentifer().IsEmpty());
			fst_check(endpoint->TestGetGkInterface().IsEmpty());

			/* ... and registration is therefore never initiated.  The
			 * !m_gkAddress.IsEmpty() guard at mod_h323.cpp:451 short-circuits,
			 * so no FSGkRegThread is constructed or resumed - which is what
			 * makes this case inherently free of network side effects. */
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* endpoint-name was omitted from this document, so the hard-coded
			 * default at mod_h323.cpp:492 stands */
			fst_check_string_equals((const char *) endpoint->TestGetEndpointName(), "FreeSwitch");

			/* the located document's single listener stanza was parsed */
			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);

			/* Initialise() was never called, so this case still owns the
			 * listener ReadConfig() constructed; ~FSH323EndPoint does not free
			 * it.  See fst_h323_release_unstarted_listeners(). */
			fst_h323_release_unstarted_listeners(endpoint);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* the shared PProcess is deliberately retained for the next case */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 3 - gk-address "*" is preserved verbatim as the LAN-search
		 * sentinel documented at h323.conf.xml:10 ("empty to disable, \"*\" to
		 * search LAN").
		 *
		 * Only the STORED value is asserted.  Initialise() is deliberately
		 * never called against this configuration: a non-empty gk-address
		 * makes mod_h323.cpp:451-455 construct an FSGkRegThread and Resume()
		 * it, which is a live RAS registration thread.  Worse, tearing that
		 * down goes through StopGkClient()'s unbounded
		 * `while (m_stop_gk) { h_timer(2); }` wait (mod_h323.cpp:697-700).
		 * Socket-free: ReadConfig() only.
		 */
		FST_TEST_BEGIN(gatekeeper_lan_search_address_preserved_verbatim)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(fst_h323_process_acquire() != NULL);
			fst_requires(fst_h323_bind_config(fst_h323_conf_gk_lan_search) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			status = endpoint->ReadConfig(0);

			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* mod_h323.cpp:537-538 stores the value unmodified - no
			 * normalisation, no expansion, no substitution */
			fst_check(!endpoint->TestGetGkAddress().IsEmpty());
			fst_check_string_equals((const char *) endpoint->TestGetGkAddress(), "*");
			fst_check_int_equals((int) endpoint->TestGetGkAddress().GetLength(), 1);

			/* the deliberately misspelled `gk-identifer` key (mod_h323.cpp:539
			 * and h323.conf.xml:11 agree on the misspelling) is parsed */
			fst_check_string_equals((const char *) endpoint->TestGetGkIdentifer(), "fst-gatekeeper");

			/* gk-retry is only ever assigned when the param is supplied
			 * (mod_h323.cpp:547-548), and this document supplies it */
			fst_check_int_equals(endpoint->TestGetGkRetry(), 45);

			/* gk-prefix appends to a PStringList (mod_h323.cpp:545-546).  It
			 * is a code-only key: it appears in no shipped fixture. */
			fst_check_int_equals((int) endpoint->TestGetGkPrefixes().GetSize(), 1);

			if (endpoint->TestGetGkPrefixes().GetSize() == 1) {
				fst_check_string_equals((const char *) endpoint->TestGetGkPrefixes()[0], "1234");
			}

			/* Asserting the sentinel does NOT start registration: only
			 * Initialise() does, and this case never calls it. */
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* ReadConfig()-only case: the listeners are still the test's. */
			fst_h323_release_unstarted_listeners(endpoint);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* the shared PProcess is deliberately retained for the next case */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 4 - listener address and port are taken from configuration.
		 *
		 * FSListener (mod_h323.h:236-243) has exactly four members - name,
		 * listenAddress, localUserName, gatekeeper - and NO port member: the
		 * `WORD port` at mod_h323.cpp:571 is function-local and is only ever
		 * handed to the H323ListenerTCP constructor.  The configured port is
		 * therefore observed through the listener object itself:
		 * H323ListenerTCP's constructor initialises its socket WITH the port
		 * (transports.cxx:1288), so GetTransportAddress() reports both the
		 * bound address and the configured port without the socket ever having
		 * been opened.
		 *
		 * This also pins the two documented defaults on the configured side of
		 * the pair: endpoint-name "fs" against the "FreeSwitch" default
		 * asserted in case 2, and a nameless <listener> falling back to
		 * "unnamed" (mod_h323.cpp:567-568).
		 *
		 * Socket-free: ReadConfig() only.
		 */
		FST_TEST_BEGIN(listeners_parsed_from_configuration)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(fst_h323_process_acquire() != NULL);
			fst_requires(fst_h323_bind_config(fst_h323_conf_listeners) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			status = endpoint->ReadConfig(0);

			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* both <listener> children were parsed, in document order */
			fst_check_int_equals((int) endpoint->m_listeners.size(), 2);

			if (endpoint->m_listeners.size() == 2) {
				FSListener & first = endpoint->m_listeners.front();
				FSListener & second = endpoint->m_listeners.back();

				/* the named listener keeps its name attribute ... */
				fst_check_string_equals((const char *) first.name, "fst-loopback");

				/* ... and the nameless one takes the documented default */
				fst_check_string_equals((const char *) second.name, "unnamed");

				/* every parsed listener owns a constructed, unopened listener */
				fst_check(first.listenAddress != NULL);
				fst_check(second.listenAddress != NULL);

				if (first.listenAddress != NULL) {
					H323TransportAddress address = first.listenAddress->GetTransportAddress();
					const char *text = (const char *) address;

					fst_check(!zstr(text));
					fst_check_string_has(text, FST_H323_LOOPBACK);
					fst_check_string_has(text, FST_H323_PORT_LISTENER_ONE);
				}

				if (second.listenAddress != NULL) {
					H323TransportAddress address = second.listenAddress->GetTransportAddress();
					const char *text = (const char *) address;

					fst_check(!zstr(text));
					fst_check_string_has(text, FST_H323_LOOPBACK);
					fst_check_string_has(text, FST_H323_PORT_LISTENER_TWO);
				}
			}

			/* configured values override every hard-coded default applied at
			 * mod_h323.cpp:490-492 */
			fst_check_string_equals((const char *) endpoint->TestGetEndpointName(), "fs");
			fst_check_int_equals(endpoint->m_pi, 16);
			fst_check_int_equals(endpoint->m_ai, 8);
			fst_check(endpoint->TestGetFastStart() == false);
			fst_check(endpoint->TestGetH245Tunneling() == false);

			/* still no gatekeeper activity: gk-address is empty here too */
			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* ReadConfig()-only case, and the one that constructs the most
			 * listeners: both are still the test's to free. */
			fst_h323_release_unstarted_listeners(endpoint);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* the shared PProcess is deliberately retained for the next case */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 5 - the codec preference string "PCMA,PCMU,GSM,G729" is
		 * honoured in order.
		 *
		 * This is the first case that calls Initialise(), because the
		 * capability table is built there (mod_h323.cpp:393-421); ReadConfig()
		 * only stores the preference string.  It therefore binds a socket, and
		 * the injected configuration accordingly pins the listener to
		 * 127.0.0.1 on a high unprivileged port and leaves gk-address empty.
		 *
		 * The mechanism, so that this case is not mis-modelled: the loop at
		 * mod_h323.cpp:397-421 walks the module's own h323_formats table
		 * (mod_h323.cpp:58-72) in TABLE order and, for each entry, asks whether
		 * the entry's short token occurs as a SUBSTRING of the configured
		 * string (mod_h323.cpp:400).  Capabilities are therefore appended in
		 * table order, filtered by presence in the configuration - the
		 * resulting order only coincidentally equals the order the operator
		 * wrote.  For "PCMA,PCMU,GSM,G729" the table entries that match are
		 * PCMA, PCMU, GSM and G729, in that table order.
		 *
		 * Assertions are PRESENCE plus RELATIVE ORDER plus MONOTONIC GROWTH.
		 * An absolute capability count is deliberately never asserted: each
		 * AddAllCapabilities() call (mod_h323.cpp:404) adds every factory entry
		 * matching a "<name>*{sw}" wildcard, so "G.729*{sw}" alone can match
		 * the G.729, G.729A, G.729B and G.729A/B registrars that mod_h323.h
		 * installs (mod_h323.h:620-628).
		 */
		FST_TEST_BEGIN(codec_prefs_negotiation_order)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_loadable_module_interface_t *module_interface = NULL;
			char capabilities[2048];
			int before = 0;
			int after = 0;
			int at_alaw = -1;
			int at_ulaw = -1;
			int at_gsm = -1;
			int at_g729 = -1;
			bool initialised = false;

			/* All fatal checks precede the binding registration, so a fatal
			 * exit can never orphan a binding into a later case.
			 *
			 * This case MUST run while the very first PProcess of the process
			 * lifetime is still alive, which is exactly what the shared
			 * FSProcess guarantees: once any PProcess has been destroyed the
			 * capability factory and the media-format registry are empty for
			 * good and AddAllCapabilities() can add nothing. */
			fst_requires(fst_h323_process_acquire() != NULL);

			/* Initialise() calls switch_loadable_module_create_interface(), so
			 * it needs a real module interface from a real pool. */
			module_interface = switch_loadable_module_create_module_interface(fst_pool, "mod_h323_test");
			fst_requires(module_interface != NULL);

			fst_requires(fst_h323_bind_config(fst_h323_conf_codec_prefs) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			before = (int) endpoint->GetCapabilities().GetSize();

			/* Initialise() reads the configuration itself (mod_h323.cpp:381),
			 * which is why the binding is registered before this call. */
			initialised = endpoint->Initialise(module_interface);

			after = (int) endpoint->GetCapabilities().GetSize();

			/* Initialise() returns TRUE unconditionally (mod_h323.cpp:457) */
			fst_check(initialised == true);

			/* a fresh endpoint starts with an empty capability table, and the
			 * preference string must have grown it */
			fst_check_int_equals(before, 0);
			fst_check(after > before);

			fst_h323_capability_table(endpoint->GetCapabilities(), capabilities, sizeof(capabilities));

			fst_check(!zstr(capabilities));

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "mod_h323 capability table for codec-prefs \"PCMA,PCMU,GSM,G729\": %s\n", capabilities);

			/* PRESENCE - matched on the media format name rather than the
			 * configuration token, because the capability table holds
			 * h323_formats' left-hand column.  Substring matching keeps the
			 * assertion stable against H323Plus' "{sw}"/"{hw}" name suffixes. */
			fst_check_string_has(capabilities, "G.711-ALaw-64k");
			fst_check_string_has(capabilities, "G.711-uLaw-64k");
			fst_check_string_has(capabilities, "GSM-06.10");
			fst_check_string_has(capabilities, "G.729");

			at_alaw = fst_h323_name_position(capabilities, "G.711-ALaw-64k");
			at_ulaw = fst_h323_name_position(capabilities, "G.711-uLaw-64k");
			at_gsm = fst_h323_name_position(capabilities, "GSM-06.10");
			at_g729 = fst_h323_name_position(capabilities, "G.729");

			/* RELATIVE ORDER - PCMA before PCMU before GSM before G729 */
			fst_check(at_alaw >= 0);
			fst_check(at_ulaw > at_alaw);
			fst_check(at_gsm > at_ulaw);
			fst_check(at_g729 > at_gsm);

			/* endpoint-name is applied to the inherited H323EndPoint local user
			 * name when it is non-empty (mod_h323.cpp:439) */
			fst_check_string_equals((const char *) endpoint->TestGetEndpointName(), "fs");
			fst_check_string_has((const char *) endpoint->GetLocalUserName(), "fs");

			/* the endpoint interface is registered during Initialise()
			 * (mod_h323.cpp:388-391) */
			fst_check(endpoint->GetSwitchInterface() != NULL);

			/* the configured loopback listener was the one started, so the
			 * empty-list wildcard fallback at mod_h323.cpp:441-442 was not
			 * taken, and gk-address is empty so no RAS thread exists */
			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* NO fst_h323_release_unstarted_listeners() HERE, DELIBERATELY.
			 * Initialise() handed this listener to StartListener(), which
			 * returned TRUE and thereby took ownership (h323ep.h:532-545), so
			 * it now lives in the base endpoint's H323ListenerList.
			 * ~H323EndPoint() removes and destroys every started listener
			 * (h323ep.cxx:978), closing the loopback socket.  Releasing it here
			 * as well would be a double free - and valgrind confirms this case
			 * leaks nothing without it. */
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* This is the last case that constructs an endpoint directly, so the
			 * shared PProcess is released HERE - explicitly, inside a test case,
			 * never deferred to teardown - leaving the next case free to let the
			 * module create its own.  At no instant are two alive. */
			fst_h323_process_release();

			fst_check(!PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 6 - a configuration-present module load registers the endpoint
		 * interface.
		 *
		 * mod_h323_load() is the module's own entry point, reached by name
		 * rather than through a dlopen: SWITCH_MODULE_LOAD_FUNCTION expands to
		 * a plain definition with no storage class and mod_h323.cpp declares it
		 * inside SWITCH_BEGIN_EXTERN_C, so it has C linkage and external
		 * visibility.
		 *
		 * The binding must be registered BEFORE the call, because the load
		 * path reads the configuration itself.  This case leaves the module
		 * loaded on purpose: case 7 asserts that shutting it down works.
		 */
		FST_TEST_BEGIN(module_load_registers_endpoint_interface)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			FSProcess *process = NULL;

			/* Defensive: clear any shared PProcess an earlier case left behind by
			 * exiting on a fatal check.  A no-op in a healthy run, and it keeps
			 * this case from ever asking PTLib for a second live PProcess. */
			fst_h323_process_release();

			/* Expected consequence of the shared process having been released:
			 * PProcess::~PProcess() has already emptied the H323CapabilityFactory
			 * and the media-format registry for good, so the Initialise() inside
			 * the load below can add no audio capability and logs "failed to add
			 * capability" once per codec-prefs entry (mod_h323.cpp:408 and :417).
			 * That is harness order, not a module defect, and no verdict in this
			 * case reads the capability table: the codec-preference contract is
			 * asserted in the codec case above, which runs while the first
			 * PProcess of the run - and therefore the factory - is still alive. */

			fst_requires(!PProcess::IsInitialised());
			fst_requires(fst_h323_bind_config(fst_h323_conf_module_load) == SWITCH_STATUS_SUCCESS);

			/* The module is loaded against the module-lifetime pool, NEVER
			 * against fst_pool: this case deliberately leaves the module loaded
			 * for case 7, and fst_pool does not survive this case's teardown.
			 * See fst_h323_module_pool above. */
			fst_requires(fst_h323_module_pool_create() == SWITCH_STATUS_SUCCESS);
			fst_requires(fst_h323_module_pool != NULL);

			/* mod_h323_load() creates the module interface itself
			 * (mod_h323.cpp:159) and hands it back through the out-parameter */
			status = mod_h323_load(&module_interface, fst_h323_module_pool);

			/* POSIX builds return SUCCESS; WIN32 returns NOUNLOAD
			 * (mod_h323.cpp:175-179).  Both are a successful load. */
			fst_check(status == SWITCH_STATUS_SUCCESS || status == SWITCH_STATUS_NOUNLOAD);
			fst_check(module_interface != NULL);

			/* The FSProcess the module allocated (mod_h323.cpp:167) is now the
			 * PTLib process singleton, and it is reached here through PTLib's own
			 * PUBLIC interface rather than through the module's static
			 * h323_process pointer.  PProcess::Current() is public, and the
			 * POINTER form of dynamic_cast is used deliberately: it yields NULL
			 * on a type mismatch instead of throwing, which matters because this
			 * target is compiled -fno-exceptions.  IsInitialised() is checked
			 * first so Current() is only called once a process exists. */
			fst_check(PProcess::IsInitialised());

			if (PProcess::IsInitialised()) {
				process = dynamic_cast<FSProcess *>(&PProcess::Current());
				fst_check(process != NULL);
			}

			if (module_interface != NULL) {
				/* The module name on the interface is the `modname` the module
				 * definition macro emits (switch_types.h:2641), which is the
				 * literal "mod_h323"; mod_h323.cpp:159 passes it straight to
				 * switch_loadable_module_create_module_interface().  It is a
				 * different string from the endpoint interface name asserted
				 * below, and both are public members of the returned
				 * interface (switch_loadable_module.h:64-67). */
				fst_check(!zstr(module_interface->module_name));
				fst_check_string_equals(module_interface->module_name, "mod_h323");

				fst_check(module_interface->endpoint_interface != NULL);

				if (module_interface->endpoint_interface != NULL) {
					/* the interface name is the module's own `modulename`
					 * constant, which is the literal "h323"
					 * (mod_h323.cpp:57 and :389) - never "mod_h323", which is
					 * the separate `modname` the definition macro emits */
					fst_check(!zstr(module_interface->endpoint_interface->interface_name));
					fst_check_string_equals(module_interface->endpoint_interface->interface_name, "h323");
					fst_check(module_interface->endpoint_interface->io_routines != NULL);
					fst_check(module_interface->endpoint_interface->state_handler != NULL);
				}
			}

			/* The same interface is reachable through two further public
			 * accessors, both on the handle obtained above: FSProcess's public
			 * GetH323EndPoint() (mod_h323.h:230-232) and the endpoint's public
			 * GetSwitchInterface() (mod_h323.h:266-268).  A live FSProcess
			 * implies a constructed endpoint (mod_h323.cpp:173-183). */
			if (process != NULL) {
				FSH323EndPoint & endpoint = process->GetH323EndPoint();
				switch_endpoint_interface_t *registered = endpoint.GetSwitchInterface();

				fst_check(registered != NULL);

				if (registered != NULL) {
					fst_check_string_equals(registered->interface_name, "h323");
				}

				/* the injected loopback listener was used, so the wildcard
				 * fallback on port 1720 was not taken */
				fst_check_int_equals((int) endpoint.m_listeners.size(), 1);
			}

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* h323_process is deliberately left alive for the next case */
		}
		FST_TEST_END()

		/*
		 * CASE 7 - mod_h323_shutdown() succeeds.  DECLARED LAST.
		 *
		 * Declared last for two reasons: it must observe a fully initialised
		 * module, and it is the only thing that releases the four global strings
		 * mod_h323_globals holds (mod_h323.cpp:190-193), so running it last
		 * leaves none of them retained at exit.
		 *
		 * Be precise about what that reclaims.  All four setters are generated by
		 * SWITCH_DECLARE_GLOBAL_STRING_FUNC (mod_h323.cpp:44-47), and each frees
		 * the previous value before strdup'ing the new one
		 * (src/include/switch_utils.h:1009-1010).  At most one allocation per
		 * global is therefore outstanding at any instant: each earlier case's
		 * strings are reclaimed by the next case that overwrites them, and this
		 * case releases only the values still retained when it runs - not an
		 * accumulation from every earlier case.
		 *
		 * The globals differ in when they are set, which is worth stating
		 * exactly.  context and dialplan are set unconditionally near the top of
		 * ReadConfig() (mod_h323.cpp:474-475), before the configuration is even
		 * opened, so they are allocated even by case 1's failing call and may
		 * then be overwritten from the settings loop (mod_h323.cpp:506, :508).
		 * codec_string is set only when codec-prefs is present
		 * (mod_h323.cpp:510), which is true of three of this file's five
		 * documents.  rtp_timer_name has two possible sources - an explicit
		 * rtp-timer-name param (mod_h323.cpp:514) and the "soft" fallback taken
		 * when use-rtp-timer was enabled (mod_h323.cpp:593-594) - and no document
		 * here carries either key, so it stays NULL throughout and its
		 * switch_safe_free is a no-op.  None of that is asserted here: those
		 * globals sit behind internal linkage, and their release is covered by
		 * the sanitizer instead.
		 *
		 * The status is asserted rather than discarded.  That is only possible
		 * because the suite uses FST_SUITE_BEGIN: the module-loading bootstrap
		 * tier's end macro (switch_test.h:379-387) would have performed an
		 * unasserted unload at suite end instead.
		 */
		FST_TEST_BEGIN(module_shutdown_releases_resources)
		{
			switch_status_t status = SWITCH_STATUS_FALSE;

			/* The previous case left the module loaded.  Asserted through PTLib's
			 * public singleton interface, not through the module's static
			 * h323_process pointer. */
			fst_check(PProcess::IsInitialised());

			if (PProcess::IsInitialised()) {
				fst_check(dynamic_cast<FSProcess *>(&PProcess::Current()) != NULL);
			}

			status = mod_h323_shutdown();

			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Resource release is asserted on a PUBLIC seam.  mod_h323.cpp:195-196
			 * deletes the process, and FSProcess::~FSProcess()
			 * (mod_h323.cpp:366-368) deletes the endpoint with it; deleting the
			 * one live PProcess-derived object is precisely what makes
			 * PProcess::IsInitialised() false again, so this single public check
			 * proves both the process and the endpoint it owned are gone.
			 *
			 * The four global strings freed at mod_h323.cpp:190-193 are NOT
			 * asserted: they live in the .cpp-file static mod_h323_globals
			 * (mod_h323.cpp:42) behind file-static setters, so no public seam
			 * exposes them.  Their release is covered where it is genuinely
			 * observable - by the sanitizer, which fails the build on a leak. */
			fst_check(!PProcess::IsInitialised());

			/* Shutdown is idempotent: every pointer it touches was nulled, so a
			 * second call succeeds and the process stays released. */
			status = mod_h323_shutdown();

			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(!PProcess::IsInitialised());

			/* ONLY NOW is the module-lifetime pool released.  Shutdown is the
			 * last thing that touches pool-backed module state, so this is the
			 * earliest point at which destroying it is safe - and doing it here,
			 * inside the case, keeps it out of the per-case teardown that owns
			 * fst_pool. */
			fst_h323_module_pool_destroy();
			fst_check(fst_h323_module_pool == NULL);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
