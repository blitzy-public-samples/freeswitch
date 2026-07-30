/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2018, Anthony Minessale II <anthm@freeswitch.org>
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
 * Tuyan Ozipek <tuyanozipek@gmail.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Tuyan Ozipek <tuyanozipek@gmail.com>
 * Lukasz Zwierko <lzwierko@gmail.com>
 * Robert Jongbloed <robertj@voxlucida.com.au>
 *
 * mod_opal_test -- mod_opal tests
 *
 */

#include <switch.h>
#include <test/switch_test.h>
#include "../mod_opal.h"

/*
 * ---------------------------------------------------------------------------
 * Suite design notes
 * ---------------------------------------------------------------------------
 *
 * This is a white-box-free harness: it includes only "../mod_opal.h" and reaches
 * its subjects exclusively through legitimate C++ access.  Nothing is
 * de-staticised, no symbol is re-exported, no `friend' declaration is added and
 * not one line of mod_opal.cpp / mod_opal.h is modified.
 *
 * BUILD WIRING THIS FILE DEPENDS ON, STATED AS THE REQUIREMENT IT IS:
 * src/mod/endpoints/mod_opal/Makefile.am declares only the production module
 * target, so nothing in the tree builds or collects this suite yet.  Because the
 * suite includes the header alone, the module's own translation unit has to reach
 * the link some other way, and the shape that supplies it is the tree's C++
 * module-test precedent (src/mod/codecs/mod_openh264/Makefile.am:12-21): a
 * noinst_LTLIBRARIES = libmodopal.la convenience library reusing the module's two
 * verified `pkg-config opal` sed-filtered expressions, a noinst_PROGRAMS =
 * test/test_mod_opal program whose only source is test/test_mod_opal.cpp, an
 * _LDFLAGS carrying the OPAL --libs expansion, an _LDADD naming libmodopal.la and
 * $(switch_builddir)/libfreeswitch.la, the two -DSWITCH_TEST_BASE_DIR_* defines
 * described beside the suite below, and TESTS = $(noinst_PROGRAMS).  Every flag
 * the production target carries must be carried here too, because a binary built
 * with different flags is not testing the same code.  That file is owned
 * elsewhere and is not edited by this suite; no part of this file can stand in
 * for it.
 *
 * Entry points.  SWITCH_MODULE_LOAD_FUNCTION / SWITCH_MODULE_SHUTDOWN_FUNCTION
 * expand to plain definitions with no storage class, so mod_opal_load() and
 * mod_opal_shutdown() have external linkage and are called directly here -
 * their switch_status_t results become genuine assertions rather than the
 * unasserted load and unload that the framework's module-scoped suite
 * bootstrap would perform on the suite's behalf.  Because mod_opal.cpp wraps
 * them in SWITCH_BEGIN_EXTERN_C, the declarations below must carry C linkage
 * or this C++ unit would fail to link against the mangled names.  The
 * module-interface definition macro is deliberately not invoked either: the
 * module's own translation unit already emits mod_opal_module_interface, and a
 * second definition would be a duplicate symbol.
 *
 * Configuration injection.  switch_xml_open_cfg() resolves "opal.conf" through
 * switch_xml_locate(), which consults the registered XML search bindings before
 * falling back to the static configuration root.  A binding cannot suppress a
 * static-root hit, so the two configuration outcomes are produced as follows:
 *
 *   failure - the suite's own fixture root (test/conf_opal/freeswitch.xml)
 *             deliberately contains no <configuration name="opal.conf"> child.
 *             With no binding registered both the binding list and the static
 *             root miss, switch_xml_open_cfg() returns NULL and
 *             FSManager::ReadConfig() takes its documented error branch.
 *
 *   success - a binding is registered for the duration of the case and returns
 *             a freshly parsed copy of TEST_OPAL_CONFIG_XML below.
 *
 * PTLib process singleton.  OpalManager's constructor reads
 * PProcess::Current(), which terminates the process outright when no
 * PProcess-derived object exists.  Every FSManager therefore needs a live
 * PProcess, while PTLib permits at most one at a time.  The suite keeps a
 * single suite-local FSProcess for the cases that build managers directly and
 * releases it before mod_opal_load() creates its own, so exactly one
 * PProcess-derived object is alive at any instant.
 *
 * Determinism.  No case starts a runtime thread, dlopens or dlcloses a module,
 * talks to a third party, opens a random port or depends on the wall clock.
 * Every injected listener binds loopback on a fixed high port.
 */

SWITCH_BEGIN_EXTERN_C
SWITCH_MODULE_LOAD_FUNCTION(mod_opal_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_opal_shutdown);
SWITCH_END_EXTERN_C

/*
 * The endpoint interface name registered by FSManager::Initialise().  The
 * module's own ModuleName constant has internal linkage, so the expected value
 * is spelled out here.  It is "opal", never "mod_opal".
 */
#define TEST_OPAL_INTERFACE_NAME "opal"

/* The module name the definition macro emits, carried by the loadable module
 * interface that mod_opal_load() returns. */
#define TEST_OPAL_MODULE_NAME "mod_opal"

/* The configuration file name FSManager::ReadConfig() asks for. */
#define TEST_OPAL_CONFIG_FILE "opal.conf"

/*
 * Listener coordinates for the injected configuration.  Loopback plus a fixed
 * high port keeps FSManager::Initialise()'s StartListener() call harmless: the
 * shipped sample's $${local_ip_v4} would resolve to a real interface, and its
 * port 1720 is the IANA-registered H.323 call-signalling port - the value
 * H323EndPoint::DefaultTcpSignalPort carries (mod_opal.h:89) and therefore the
 * port a real H.323 service on this host is most likely to already hold.
 * Omitting the listener stanza altogether is worse still: ReadConfig() appends
 * listeners only when a <listeners> element is present (mod_opal.cpp:411-413),
 * so an empty list drives Initialise() into the StartListener("") wildcard bind
 * (mod_opal.cpp:284-285).
 */
#define TEST_OPAL_LISTEN_NAME "opal-test-listener"
#define TEST_OPAL_LISTEN_ADDRESS "127.0.0.1"
#define TEST_OPAL_LISTEN_PORT "21720"

/*
 * Settings values chosen so the assertions cannot pass by accident.  FSManager
 * member-initialises m_context to "default" and m_dialplan to "XML" in its
 * constructor, so the injected configuration must differ from both for the
 * accessor assertions to prove that parsing actually happened.
 */
#define TEST_OPAL_CONTEXT "opal-test-context"
#define TEST_OPAL_DIALPLAN "OPAL_TEST_DIALPLAN"
#define TEST_OPAL_CODEC_PREFS "PCMA,PCMU,GSM,G729"

/*
 * The document handed back by the injected configuration provider.
 *
 * The full <document>/<section name="configuration">/<configuration> envelope
 * is mandatory: switch_xml_locate() resolves a request with two nested
 * switch_xml_find_child() lookups and both must succeed.  The shipped
 * conf/vanilla/autoload_configs/opal.conf.xml is a bare <configuration>
 * element and therefore cannot be used as a binding payload as-is.
 *
 * Only keys FSManager::ReadConfig() actually recognises appear here.  Three
 * recognised keys are deliberately omitted for side-effect containment:
 * "dtmf-type" would mutate the H.323 endpoint's user-input mode for no
 * assertion, a non-empty "gk-address" would trigger a live gatekeeper RAS
 * registration, and a non-zero "trace-level" would install a PTrace stream
 * that only the destruction of an FSProcess reclaims.
 */
static const char TEST_OPAL_CONFIG_XML[] =
	"<document type=\"freeswitch/xml\">"
		"<section name=\"configuration\">"
			"<configuration name=\"" TEST_OPAL_CONFIG_FILE "\" description=\"Opal Endpoints\">"
				"<settings>"
					"<param name=\"context\" value=\"" TEST_OPAL_CONTEXT "\"/>"
					"<param name=\"dialplan\" value=\"" TEST_OPAL_DIALPLAN "\"/>"
					"<param name=\"codec-prefs\" value=\"" TEST_OPAL_CODEC_PREFS "\"/>"
					"<param name=\"disable-transcoding\" value=\"true\"/>"
					"<param name=\"jitter-size\" value=\"40,100\"/>"
				"</settings>"
				"<listeners>"
					"<listener name=\"" TEST_OPAL_LISTEN_NAME "\">"
						"<param name=\"h323-ip\" value=\"" TEST_OPAL_LISTEN_ADDRESS "\"/>"
						"<param name=\"h323-port\" value=\"" TEST_OPAL_LISTEN_PORT "\"/>"
					"</listener>"
				"</listeners>"
			"</configuration>"
		"</section>"
	"</document>";

/*
 * A second configuration document whose single <listener> element carries NO
 * name attribute.  This is the input that drives FSManager::ReadConfig() down
 * its listener-name default branch (mod_opal.cpp:418-419): switch_xml_attr_soft()
 * yields "" for an absent attribute, PString::IsEmpty() is therefore true, and
 * the name becomes the literal "unnamed".
 *
 * Identical to the default document in every other respect - same recognised
 * keys, same loopback address, same high port - so the only
 * difference the assertion can be responding to is the missing attribute.  It
 * declares the port explicitly rather than relying on FSListener's constructor
 * default, because case 3 already covers that default and mixing the two would
 * blur which property failed.
 */
static const char TEST_OPAL_CONFIG_XML_UNNAMED_LISTENER[] =
	"<document type=\"freeswitch/xml\">"
		"<section name=\"configuration\">"
			"<configuration name=\"" TEST_OPAL_CONFIG_FILE "\" description=\"Opal Endpoints\">"
				"<settings>"
					"<param name=\"context\" value=\"" TEST_OPAL_CONTEXT "\"/>"
					"<param name=\"dialplan\" value=\"" TEST_OPAL_DIALPLAN "\"/>"
					"<param name=\"codec-prefs\" value=\"" TEST_OPAL_CODEC_PREFS "\"/>"
					"<param name=\"disable-transcoding\" value=\"true\"/>"
					"<param name=\"jitter-size\" value=\"40,100\"/>"
				"</settings>"
				"<listeners>"
					"<listener>"
						"<param name=\"h323-ip\" value=\"" TEST_OPAL_LISTEN_ADDRESS "\"/>"
						"<param name=\"h323-port\" value=\"" TEST_OPAL_LISTEN_PORT "\"/>"
					"</listener>"
				"</listeners>"
			"</configuration>"
		"</section>"
	"</document>";

/*
 * ---------------------------------------------------------------------------
 * LISTENER-NAME OBSERVATION BY LOG CAPTURE
 * ---------------------------------------------------------------------------
 *
 * FSManager::m_listeners is PRIVATE (mod_opal.h, last member of the private
 * section), and no accessor exposes it or any element of it - FSManager's public
 * surface is GetSwitchInterface / GetContext / GetDialPlan / GetCodecPrefs /
 * GetDisableTranscoding only.  Constructing an FSListener directly, as case 3
 * does, cannot reach the default either: "unnamed" is applied by ReadConfig(),
 * not by the constructor, which initialises only m_port.
 *
 * The one thing ReadConfig() makes publicly observable about a listener it
 * stored is the line it emits for each one at mod_opal.cpp:431:
 *
 *     switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
 *                       "Created Listener '%s'\n", (const char *) listener.m_name);
 *
 * Capturing that through switch_log_bind_logger() is therefore the only
 * legitimate seam, and it needs no private access, no friend declaration, no
 * de-staticising and no production edit - rule R2-1 and rule R2-2 both hold.
 * The technique is sanctioned in-tree: tests/unit/switch_log.c binds a logger
 * and waits for a matching line under a mutex and condition variable, which is
 * the shape reproduced below.
 *
 * DELIVERY IS ASYNCHRONOUS, so a bounded wait is mandatory rather than
 * decorative.  switch_log_printf() only enqueues the node
 * (switch_queue_trypush(LOG_QUEUE, node), src/switch_log.c:732); bound loggers
 * are invoked later, from log_thread(), which pops the queue and calls each
 * binding whose level admits the node (src/switch_log.c:511-518).  A test that
 * read the captured value straight after ReadConfig() returned would be racing
 * that thread.
 *
 * The binding level is SWITCH_LOG_DEBUG because the dispatch test is
 * "binding->level >= node->level" and DEBUG is the highest-numbered real level,
 * so a lower binding level would filter this line out.  Binding at DEBUG also
 * raises the core's MAX_LEVEL (src/switch_log.c:461-463), which is the gate that
 * decides whether a node is enqueued for bound loggers at all
 * (src/switch_log.c:706); and FST_TEST_BEGIN independently raises the core log
 * level to SWITCH_LOG_DEBUG for the duration of each case, clearing the earlier
 * limit_level gate.  All three conditions therefore hold by construction rather
 * than by luck.
 */
#define TEST_OPAL_LISTENER_LOG_MARKER "Created Listener '"
#define TEST_OPAL_LOG_TIMEOUT_MS 5000

/*
 * Slack applied to the arming stamp described below.  The soft timer refreshes
 * the cached clock in coarse steps, so a node emitted immediately after arming
 * can legitimately carry a stamp a few milliseconds older than the arming
 * instant.  100 ms absorbs that comfortably while still leaving most of the
 * roughly one second that separates consecutive cases as margin against a stale
 * node from the previous case.
 */
#define TEST_OPAL_LOG_ARM_SLACK_US 100000

static switch_mutex_t *test_opal_log_mutex = NULL;
static switch_thread_cond_t *test_opal_log_cond = NULL;
static char test_opal_log_listener_name[128];
static int test_opal_log_captured = 0;
static switch_time_t test_opal_log_armed_at = 0;

/*
 * Bound logger.  Extracts the listener name from between the quotes of the first
 * matching line and signals the waiter.
 *
 * Two filters make the capture unambiguous.  Only the FIRST match is kept, so a
 * later line cannot overwrite an observation the waiter has not read yet.  And a
 * node is only considered when its timestamp is at or after the moment capture
 * was armed, which closes the one race the queue makes possible: switch_log_printf()
 * enqueues nodes and log_thread() dispatches them later, so a line emitted by an
 * earlier case could in principle still be sitting in LOG_QUEUE when this logger
 * binds.  Such a node carries an older timestamp and is discarded here, so an
 * earlier case's listener name can never be mistaken for this one's.
 *
 * The stamp comparison must use ONE clock, and it has to be the log subsystem's
 * own.  src/switch_log.c:565 stamps a node with switch_micro_time_now(), which
 * returns the soft timer's cached clock while the core is running
 * (src/switch_time.c:311-314) and therefore LAGS the real clock that
 * switch_time_now() reads straight from CLOCK_REALTIME (src/switch_apr.c:327-336).
 * Arming from the real clock and comparing against a lagging node stamp discards
 * the very line being waited for, so test_opal_log_capture_start() arms with
 * switch_micro_time_now() as well.
 *
 * The mutex guard is dropped defensively when capture is not armed, so a stray
 * invocation can never touch pool memory the teardown has already freed.
 */
static switch_status_t test_opal_listener_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	(void) level;

	if (!test_opal_log_mutex || !node || !node->content) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(test_opal_log_mutex);

	if (!test_opal_log_captured && node->timestamp >= test_opal_log_armed_at) {
		const char *marker = strstr(node->content, TEST_OPAL_LISTENER_LOG_MARKER);

		if (marker) {
			const char *start = marker + (sizeof(TEST_OPAL_LISTENER_LOG_MARKER) - 1);
			const char *end = strchr(start, '\'');

			if (end && (switch_size_t) (end - start) < sizeof(test_opal_log_listener_name)) {
				memcpy(test_opal_log_listener_name, start, (switch_size_t) (end - start));
				test_opal_log_listener_name[end - start] = '\0';
				test_opal_log_captured = 1;
				switch_thread_cond_signal(test_opal_log_cond);
			}
		}
	}

	switch_mutex_unlock(test_opal_log_mutex);

	return SWITCH_STATUS_SUCCESS;
}

/*
 * Arm capture: reset the state, stamp the arming instant, build the mutex and
 * condition variable from the caller's pool, and bind the logger.  Safe to call
 * more than once per case; each call starts from a cleared buffer and a fresh
 * arming stamp, so a second observation can inherit neither the first one's
 * value nor a line the first one produced.
 *
 * The state is reset before the bind rather than after, so there is no window in
 * which a bound logger could see stale values, and no lock is needed for the
 * reset because no binding exists yet.
 *
 * The arming stamp deliberately comes from switch_micro_time_now(), the same
 * function the log subsystem stamps nodes with, less a small slack; see the
 * logger above for why any other clock silently discards the wanted line.
 */
static switch_status_t test_opal_log_capture_start(switch_memory_pool_t *pool)
{
	test_opal_log_listener_name[0] = '\0';
	test_opal_log_captured = 0;
	test_opal_log_armed_at = switch_micro_time_now() - TEST_OPAL_LOG_ARM_SLACK_US;

	if (switch_mutex_init(&test_opal_log_mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_thread_cond_create(&test_opal_log_cond, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return switch_log_bind_logger(test_opal_listener_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);
}

/*
 * Disarm capture.  The logger is unbound FIRST, and switch_log_unbind_logger()
 * takes the same BINDLOCK that log_thread() holds across every dispatch
 * (src/switch_log.c:511-518), so once it returns no invocation can still be in
 * flight; only then are the pool-backed pointers dropped.  Order matters,
 * because the mutex and condition variable live in the per-case pool that the
 * teardown destroys.
 *
 * The unbind status is returned rather than swallowed: an orphaned binding would
 * survive into every later case in the same process and would eventually be
 * invoked with a dangling mutex.
 */
static switch_status_t test_opal_log_capture_stop(void)
{
	switch_status_t status = switch_log_unbind_logger(test_opal_listener_logger);

	test_opal_log_mutex = NULL;
	test_opal_log_cond = NULL;

	return status;
}

/*
 * Wait up to timeout_ms for the listener line, then return the captured name, or
 * NULL if none arrived.  Re-reads the clock on every iteration so a spurious
 * wakeup cannot extend the deadline, which is the shape tests/unit/switch_log.c
 * uses.
 *
 * switch_time_now() is correct HERE, unlike in the arming stamp, because this
 * clock is only ever used to measure an elapsed interval for a relative
 * switch_thread_cond_timedwait() timeout - it is never compared against a log
 * node's own stamp, so the two clocks cannot disagree about anything that
 * matters.
 */
static const char *test_opal_log_wait(switch_interval_time_t timeout_ms)
{
	switch_time_t now = switch_time_now();
	switch_time_t expiration = now + (timeout_ms * 1000);
	const char *captured = NULL;

	if (!test_opal_log_mutex || !test_opal_log_cond) {
		return NULL;
	}

	switch_mutex_lock(test_opal_log_mutex);

	while (!test_opal_log_captured && (now = switch_time_now()) < expiration) {
		switch_thread_cond_timedwait(test_opal_log_cond, test_opal_log_mutex, expiration - now);
	}

	if (test_opal_log_captured) {
		captured = test_opal_log_listener_name;
	}

	switch_mutex_unlock(test_opal_log_mutex);

	return captured;
}

/*
 * The single suite-local PTLib process.  mod_opal's own FSProcess pointer has
 * internal linkage and is unreachable from here, so the suite owns its own.
 *
 * It exists purely so that PProcess::Current() has something to return, which
 * OpalManager's constructor requires.  The cases deliberately construct their
 * FSManager objects directly instead of going through FSProcess::Initialise(),
 * because that entry point would also initialise the manager and open its
 * listeners - which would rob the socket-free cases of that property.
 */
static FSProcess *test_opal_process = NULL;

/*
 * Injected configuration provider.
 *
 * Registered as a switch_xml_search_function_t, masked to the configuration
 * section only, and gated on the exact request FSManager::ReadConfig() makes.
 * The gate matters: switch_xml_locate() consults every binding for every
 * lookup, and an ungated provider would hijack modules.conf and console.conf.
 *
 * A fresh document is returned on every invocation.  switch_xml_locate() takes
 * ownership of whatever a binding returns - it frees the document itself when
 * the request cannot be resolved inside it, and hands it to the caller
 * otherwise - so returning a cached tree twice would be a double free.
 */
static switch_xml_t test_opal_xml_config_provider(const char *section,
												  const char *tag_name,
												  const char *key_name,
												  const char *key_value,
												  switch_event_t *params,
												  void *user_data)
{
	const char *document = (const char *) user_data;
	switch_xml_t xml = NULL;

	(void) params;

	/*
	 * The document to serve arrives as the binding's user_data, so one provider
	 * can present a different configuration per case.  NULL selects the default
	 * document, so a caller that needs only that document passes nothing.
	 */
	if (!document) {
		document = TEST_OPAL_CONFIG_XML;
	}

	if (!section || strcmp(section, "configuration")) {
		return NULL;
	}

	if (!tag_name || strcmp(tag_name, "configuration")) {
		return NULL;
	}

	if (!key_name || strcmp(key_name, "name")) {
		return NULL;
	}

	if (!key_value || strcmp(key_value, TEST_OPAL_CONFIG_FILE)) {
		return NULL;
	}

	/*
	 * SWITCH_TRUE makes switch_xml_parse_str_dynamic() duplicate the buffer
	 * before parsing it, so the constant above is never written to and the
	 * duplicate is released by switch_xml_free().  Passing SWITCH_FALSE here
	 * would let the parser write null terminators into read-only storage.
	 */
	xml = switch_xml_parse_str_dynamic((char *) document, SWITCH_TRUE);

	return xml;
}

/*
 * Register the injected configuration provider, serving the supplied document.
 * Never called from inside the provider itself: switch_xml_locate() holds a read
 * lock across the callback that the bind and unbind paths take for writing.
 *
 * The cast discards const only to satisfy the void * user_data parameter; the
 * provider re-adds const immediately and the document is duplicated before it is
 * parsed, so the storage behind it is never written to.
 */
static switch_status_t test_opal_bind_config_document(const char *document)
{
	return switch_xml_bind_search_function_ret(test_opal_xml_config_provider,
											   SWITCH_XML_SECTION_CONFIG,
											   (void *) document,
											   NULL);
}

/*
 * Register the provider serving the default configuration document.
 */
static switch_status_t test_opal_bind_config(void)
{
	return test_opal_bind_config_document(NULL);
}

/*
 * Remove the injected configuration provider.  Unbinding by function pointer
 * keeps registration and removal symmetric without carrying a handle around,
 * and the result is asserted by every caller because an orphaned binding would
 * leak into every case that follows this one in the same process.
 */
static switch_status_t test_opal_unbind_config(void)
{
	return switch_xml_unbind_search_function_ptr(test_opal_xml_config_provider);
}

/*
 * Return the suite-local PTLib process, creating it on first use.  Every
 * FSManager needs one to exist, because OpalManager's constructor reads
 * PProcess::Current().
 */
static FSProcess *test_opal_acquire_process(void)
{
	if (!test_opal_process) {
		test_opal_process = new FSProcess();
	}

	return test_opal_process;
}

/*
 * Destroy the suite-local PTLib process if one is alive.  Idempotent, so it
 * doubles as the safety net that guarantees nothing outlives the suite.
 */
static void test_opal_release_process(void)
{
	if (test_opal_process) {
		delete test_opal_process;
		test_opal_process = NULL;
	}
}

/*
 * The pool the loaded module is given, and why it cannot be fst_pool.
 *
 * mod_opal_load() allocates state out of its pool argument that stays reachable
 * for as long as the module is loaded: it carves the module interface with
 * switch_loadable_module_create_module_interface(pool, modname)
 * (mod_opal.cpp:111), and the FSProcess it then creates on the heap reaches
 * FSManager::Initialise(), which stores an interface allocated from that same
 * pool in the manager's m_FreeSwitch member (mod_opal.cpp:277, mod_opal.h:115).
 *
 * fst_pool is created by FST_SETUP_BEGIN and destroyed by FST_TEARDOWN_BEGIN, so
 * it lasts exactly ONE case.  This suite deliberately leaves the module loaded at
 * the end of the load case so that the shutdown case can assert
 * mod_opal_shutdown()'s status.  Loading against fst_pool would therefore free
 * the module interface at the load case's teardown, and the shutdown case's
 * `delete opal_process` -> ~FSProcess -> `delete m_manager` (mod_opal.cpp:244)
 * would then unwind a dangling pointer - a use-after-free across a case boundary,
 * and a build failure under the address sanitizer that the CI configure line
 * always enables.
 *
 * Only state retained BETWEEN cases belongs here.  Per-case observations keep
 * using fst_pool, which is correct for them: the interface the load case builds
 * for its own scoped FSManager is consumed and discarded inside that case.
 */
static switch_memory_pool_t *test_opal_module_pool = NULL;

/* The interface the load case obtained, kept so the shutdown case can observe it. */
static switch_loadable_module_interface_t *test_opal_module_interface = NULL;

/*
 * Create the module-lifetime pool.  Idempotent, and reports success only when a
 * usable pool exists afterwards so the caller can gate the load on it.
 */
static switch_status_t test_opal_module_pool_create(void)
{
	if (test_opal_module_pool) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (switch_core_new_memory_pool(&test_opal_module_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return test_opal_module_pool ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*
 * Destroy the module-lifetime pool.  MUST NOT run before the module has been shut
 * down, because shutdown is the last thing that touches pool-backed module state.
 * switch_core_destroy_memory_pool() nulls the caller's pointer, so this is safe to
 * call when nothing is held.
 */
static void test_opal_module_pool_destroy(void)
{
	if (test_opal_module_pool) {
		switch_core_destroy_memory_pool(&test_opal_module_pool);
	}
}

/*
 * The manager the loaded module owns, reached through public interfaces only:
 * PTLib's PProcess::Current() and FSProcess::GetManager() (mod_opal.h).  The
 * POINTER form of dynamic_cast is used deliberately - it yields NULL on a type
 * mismatch instead of throwing, which matters under -fno-exceptions.
 *
 * The manager is returned by pointer rather than by reference so that the "no
 * module loaded" outcome is expressible and every caller has to handle it.  That
 * is what makes the shutdown assertions behaviour-sensitive rather than a
 * tautology: reachability flips from non-NULL to NULL across the call.
 */
static FSManager *test_opal_module_manager(void)
{
	FSProcess *process = NULL;

	if (!PProcess::IsInitialised()) {
		return NULL;
	}

	process = dynamic_cast<FSProcess *>(&PProcess::Current());

	if (!process) {
		return NULL;
	}

	return &process->GetManager();
}

/*
 * ---------------------------------------------------------------------------
 * The suite
 * ---------------------------------------------------------------------------
 *
 * A real core is required: switch_xml_open_cfg() asserts that the main XML root
 * exists, which only the full bootstrap provides.  "conf_opal" names this
 * module's own fixture root: the bootstrap builds the configuration directory as
 * SWITCH_TEST_BASE_DIR_FOR_CONF, a path separator, then that name
 * (switch_test.h:92-93), so the target's Makefile.am declaration must define
 * -DSWITCH_TEST_BASE_DIR_FOR_CONF and -DSWITCH_TEST_BASE_DIR_OVERRIDE to
 * ${abs_builddir}/test for it to resolve to
 * src/mod/endpoints/mod_opal/test/conf_opal.  Without those defines the
 * bootstrap falls back to "./conf_opal" (switch_test.h:94-99), which resolves
 * against the working directory, so the suite would only find its fixtures when
 * run from inside test/.  See the build-wiring note in the file header.
 *
 * FCTX runs cases in declaration order within a single process, and that order
 * is load bearing:
 *
 *   1. the configuration-absent failure runs first, before any successful
 *      parse or load can leave module state behind;
 *   2. the three cases that only build objects follow, so they observe a
 *      pristine OPAL media-format registry - FSManager::Initialise() mutates
 *      the process-global registry;
 *   3. the load case comes next and takes over the PTLib process singleton;
 *   4. the shutdown case is declared last so it observes a fully initialised
 *      module, and its result is asserted rather than discarded.
 */
FST_CORE_BEGIN("conf_opal")
{
	FST_SUITE_BEGIN(mod_opal)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * Case 1 - configuration absent.
		 *
		 * ReadConfig() is asserted directly rather than through
		 * mod_opal_load(), because FSManager::Initialise() discards
		 * ReadConfig()'s status: a load would still report success and the
		 * error branch would go unobserved.  No binding is registered, so the
		 * fixture root's deliberate omission of the module's configuration
		 * section is what makes the miss deterministic.
		 */
		FST_TEST_BEGIN(config_absent_read_config_fails)
		{
			switch_status_t status = SWITCH_STATUS_SUCCESS;

			fst_requires(test_opal_acquire_process() != NULL);

			{
				FSManager manager;

				status = manager.ReadConfig(false);
			}

			fst_check(status == SWITCH_STATUS_FALSE);
		}
		FST_TEST_END()

		/*
		 * Case 2 - dual endpoint construction.
		 *
		 * FSManager's constructor allocates an H.323, an IAX2 and a FreeSWITCH
		 * local endpoint.  The first two are private members, so they are
		 * observed through the inherited public OpalManager::FindEndPoint()
		 * lookup rather than reached for directly, which keeps the assertion
		 * valid across any refactoring of those members.  Constructing a
		 * manager binds no socket.
		 */
		FST_TEST_BEGIN(dual_endpoint_construction)
		{
			/*
			 * Held by const pointer: the case observes the endpoints the
			 * constructor allocated, it does not reach into them.  The
			 * accessor used below is itself const.
			 */
			const OpalEndPoint *h323_endpoint = NULL;
			const OpalEndPoint *iax2_endpoint = NULL;

			fst_requires(test_opal_acquire_process() != NULL);

			{
				FSManager manager;

				h323_endpoint = manager.FindEndPoint("h323");
				iax2_endpoint = manager.FindEndPoint("iax2");

				fst_check(h323_endpoint != NULL);
				fst_check(iax2_endpoint != NULL);
				fst_check(h323_endpoint != iax2_endpoint);

				if (h323_endpoint) {
					fst_check_string_equals((const char *) h323_endpoint->GetPrefixName(), "h323");
					fst_check(manager.FindEndPointAs<H323EndPoint>("h323") != NULL);
				}

				if (iax2_endpoint) {
					fst_check_string_equals((const char *) iax2_endpoint->GetPrefixName(), "iax2");
					fst_check(manager.FindEndPointAs<IAX2EndPoint>("iax2") != NULL);
				}
			}
		}
		FST_TEST_END()

		/*
		 * Case 3 - default signalling port.
		 *
		 * FSListener's public inline constructor is the module's only source of
		 * a default signalling port, because ReadConfig() supplies none: a
		 * configuration that declares a listener but omits the port therefore
		 * still ends up on the default.  Constructing one binds no socket, and
		 * because only the port is constructor-initialised no assertion is made
		 * about the listener name here.
		 */
		FST_TEST_BEGIN(listener_default_signalling_port)
		{
			FSListener listener;

			fst_check_int_equals((int) listener.m_port, (int) H323EndPoint::DefaultTcpSignalPort);
		}
		FST_TEST_END()

		/*
		 * Case 4 - settings parsed from the injected configuration.
		 *
		 * ReadConfig() is driven directly because it is socket free, and it is
		 * called exactly once on this manager: the listener list is appended to
		 * rather than rebuilt, so a second call would not start from a clean
		 * slate.  Every assertion goes through a public const accessor; the
		 * private listener list is never read.
		 *
		 * The accessor assertions are gated on a successful parse.  Without
		 * one, the transcoding flag would still hold the indeterminate value
		 * FSManager's constructor left behind.
		 */
		FST_TEST_BEGIN(settings_from_injected_configuration)
		{
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);
			fst_requires(test_opal_bind_config() == SWITCH_STATUS_SUCCESS);

			{
				FSManager manager;

				status = manager.ReadConfig(false);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					fst_check_string_equals((const char *) manager.GetContext(), TEST_OPAL_CONTEXT);
					fst_check_string_equals((const char *) manager.GetDialPlan(), TEST_OPAL_DIALPLAN);
					fst_check_string_equals((const char *) manager.GetCodecPrefs(), TEST_OPAL_CODEC_PREFS);
					fst_check(manager.GetDisableTranscoding());
				}
			}

			fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/*
		 * Case 5 - listener name defaults to "unnamed".
		 *
		 * ReadConfig() reads a listener's name with switch_xml_attr_soft(),
		 * which yields "" rather than NULL for an absent attribute, and
		 * substitutes the literal "unnamed" when the result is empty
		 * (mod_opal.cpp:418-419).  The stored listener is private, so the
		 * substitution is observed through the line ReadConfig() emits for each
		 * listener it created (mod_opal.cpp:431), captured by a bound logger.
		 *
		 * Both halves run in one case so the comparison is like for like: the
		 * same manager type, the same parse path and the same capture code
		 * observe two documents that differ only in the presence of the name
		 * attribute.  Capture state is cleared between them by
		 * test_opal_log_capture_start(), so the second observation cannot
		 * inherit the first one's value and the first cannot pass vacuously -
		 * were the capture broken, the wait would time out and both halves would
		 * report a NULL name rather than one of them silently agreeing.
		 *
		 * Each half is fully wound down before the next begins: the manager
		 * leaves scope, the logger is unbound and the provider is unregistered,
		 * so the loopback listener is closed again before the same port is
		 * reused.
		 *
		 * Only the two binds are fatal, and each sits at a point where nothing
		 * is yet registered - the first before any setup, the second after the
		 * first half has already been wound down - so a fatal exit there leaves
		 * nothing behind.  Every assertion that follows a bind is non-fatal, so
		 * the matching unbinds are always reached.
		 */
		FST_TEST_BEGIN(listener_name_defaults_to_unnamed)
		{
			const char *observed_name = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);

			/* Half one: a <listener> with no name attribute takes the default. */
			fst_requires(test_opal_bind_config_document(TEST_OPAL_CONFIG_XML_UNNAMED_LISTENER) == SWITCH_STATUS_SUCCESS);
			fst_check(test_opal_log_capture_start(fst_pool) == SWITCH_STATUS_SUCCESS);

			{
				FSManager manager;

				status = manager.ReadConfig(false);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					observed_name = test_opal_log_wait(TEST_OPAL_LOG_TIMEOUT_MS);
					fst_check(observed_name != NULL);

					if (observed_name) {
						fst_check_string_equals(observed_name, "unnamed");
					}
				}
			}

			fst_check(test_opal_log_capture_stop() == SWITCH_STATUS_SUCCESS);
			fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);

			/*
			 * Half two: the contrast.  An identical document that does carry a
			 * name attribute must yield that name verbatim, which is what proves
			 * "unnamed" above came from the default branch and not from a
			 * capture that reports the same string whatever it is given.
			 */
			observed_name = NULL;
			status = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_bind_config() == SWITCH_STATUS_SUCCESS);
			fst_check(test_opal_log_capture_start(fst_pool) == SWITCH_STATUS_SUCCESS);

			{
				FSManager manager;

				status = manager.ReadConfig(false);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					observed_name = test_opal_log_wait(TEST_OPAL_LOG_TIMEOUT_MS);
					fst_check(observed_name != NULL);

					if (observed_name) {
						fst_check_string_equals(observed_name, TEST_OPAL_LISTEN_NAME);
					}
				}
			}

			fst_check(test_opal_log_capture_stop() == SWITCH_STATUS_SUCCESS);
			fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/*
		 * Case 6 - module load and endpoint-interface registration.
		 *
		 * Two observations of the same registration, in the only order that
		 * keeps a single PTLib process alive throughout:
		 *
		 *   (a) a manager owned by this suite is initialised and its interface
		 *       read back through the public GetSwitchInterface() accessor,
		 *       which is only meaningful after Initialise() has run because
		 *       FSManager's constructor leaves the member uninitialised.  The
		 *       manager is scoped so its loopback listener closes again before
		 *       the module opens its own on the same port;
		 *
		 *   (b) the real module entry point is then called and its
		 *       switch_status_t asserted, with the registered endpoint
		 *       interface read back from the module interface it populated.
		 *
		 * Everything after the bind uses non-fatal checks so the provider is
		 * always unregistered before the case returns.
		 */
		FST_TEST_BEGIN(module_load_and_endpoint_interface)
		{
			switch_loadable_module_interface_t *observed_interface = NULL;
			switch_loadable_module_interface_t *loaded_interface = NULL;
			switch_endpoint_interface_t *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);
			fst_requires(test_opal_bind_config() == SWITCH_STATUS_SUCCESS);

			observed_interface = switch_loadable_module_create_module_interface(fst_pool, MODNAME);
			fst_check(observed_interface != NULL);

			if (observed_interface) {
				FSManager manager;

				fst_check(manager.Initialise(observed_interface));

				endpoint = manager.GetSwitchInterface();
				fst_check(endpoint != NULL);

				if (endpoint) {
					fst_check_string_equals(endpoint->interface_name, TEST_OPAL_INTERFACE_NAME);
				}
			}

			/* Hand the PTLib process singleton over to the module. */
			test_opal_release_process();

			/* The module is loaded against the module-lifetime pool, never against
			 * fst_pool: this case leaves the module loaded on purpose so that the
			 * shutdown case can assert its status, and fst_pool does not survive
			 * this case's teardown.  See test_opal_module_pool above. */
			fst_requires(test_opal_module_pool_create() == SWITCH_STATUS_SUCCESS);
			fst_requires(test_opal_module_pool != NULL);
			fst_check(test_opal_module_pool != fst_pool);

			status = mod_opal_load(&loaded_interface, test_opal_module_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(loaded_interface != NULL);

			if (loaded_interface) {
				/* The module name carried by the interface is the `modname` the
				 * module definition macro emits (switch_types.h:2641), which
				 * mod_opal.cpp:111 hands to
				 * switch_loadable_module_create_module_interface().  It is the
				 * literal "mod_opal" and is deliberately distinct from the
				 * endpoint interface name asserted next. */
				fst_check(loaded_interface->module_name != NULL);
				fst_check_string_equals(loaded_interface->module_name, TEST_OPAL_MODULE_NAME);

				fst_check(loaded_interface->endpoint_interface != NULL);

				if (loaded_interface->endpoint_interface) {
					fst_check_string_equals(loaded_interface->endpoint_interface->interface_name,
											TEST_OPAL_INTERFACE_NAME);
				}
			}

			/* the loaded module, its manager and this interface are deliberately
			 * left alive for the shutdown case to observe */
			test_opal_module_interface = loaded_interface;

			fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/*
		 * Case 7 - module shutdown.
		 *
		 * The module's shutdown entry point destroys the process object its
		 * load created and is unconditionally successful: deleting a null
		 * pointer is a no-op, so the call is safe even where no load ran.  It
		 * is declared last so it observes a fully initialised module, and
		 * because the suite is built on FST_SUITE_BEGIN there is no implicit
		 * unload at suite end to make the result unobservable.
		 */
		FST_TEST_BEGIN(module_shutdown_succeeds)
		{
			switch_status_t status = SWITCH_STATUS_FALSE;

			/*
			 * Observe the loaded module BEFORE the call, so the assertions after it
			 * measure a transition rather than restating a fact.  A shutdown that
			 * did nothing would leave every one of these unchanged and fail below.
			 */
			fst_requires(test_opal_module_pool != NULL);
			fst_check(test_opal_module_pool != fst_pool);

			fst_requires(test_opal_module_interface != NULL);
			fst_requires(test_opal_module_interface->endpoint_interface != NULL);
			fst_check_string_equals(test_opal_module_interface->endpoint_interface->interface_name,
									TEST_OPAL_INTERFACE_NAME);

			fst_check(PProcess::IsInitialised());
			fst_check(test_opal_module_manager() != NULL);

			status = mod_opal_shutdown();
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*
			 * mod_opal.cpp:130-133 deletes the process object, and ~FSProcess
			 * deletes the manager with it (mod_opal.cpp:244).  Deleting the one live
			 * PProcess-derived object is what makes PProcess::IsInitialised() false
			 * again, so these two public observations prove both the process and the
			 * manager it owned are gone.
			 */
			fst_check(!PProcess::IsInitialised());
			fst_check(test_opal_module_manager() == NULL);

			/* the module interface itself is pool-backed, not process-backed, so it
			 * is still readable after shutdown - the pool below is what releases it */
			fst_check_string_equals(test_opal_module_interface->endpoint_interface->interface_name,
									TEST_OPAL_INTERFACE_NAME);

			/* shutdown is idempotent: deleting a null pointer is a no-op */
			status = mod_opal_shutdown();
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(!PProcess::IsInitialised());

			/* Nothing PTLib-derived may outlive the suite. */
			test_opal_release_process();
			fst_check(test_opal_process == NULL);

			/*
			 * Only now may the module-lifetime pool go.  Shutdown was the last thing
			 * to touch pool-backed module state, so this is the earliest safe point -
			 * and doing it here, inside the case, keeps it clear of the per-case
			 * teardown that owns fst_pool.  switch_core_destroy_memory_pool() nulls
			 * the caller's pointer, which is asserted so a silent failure to release
			 * cannot pass unnoticed.
			 */
			test_opal_module_interface = NULL;
			test_opal_module_pool_destroy();
			fst_check(test_opal_module_pool == NULL);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
