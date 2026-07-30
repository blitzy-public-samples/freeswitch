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
 * Link shape.  Because the suite includes the header alone, the module's own
 * translation unit reaches the link through the libmodopal.la convenience library
 * that src/mod/endpoints/mod_opal/Makefile.am declares, following the tree's C++
 * module-test precedent (src/mod/codecs/mod_openh264/Makefile.am).  That target
 * carries the production module's own compile flags, because a binary built with
 * different flags is not testing the same code.
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
 * Determinism.  No case dlopens or dlcloses a module, talks to a third party,
 * opens a random port or depends on the wall clock.  No case starts an OPAL
 * runtime thread either, but that is an outcome of the containment described
 * next rather than something the module does for us: left to itself,
 * constructing an FSManager starts two.
 *
 * Socket footprint, stated so that nobody has to rediscover it.  Every socket
 * this suite is responsible for is bound to loopback:
 *
 *   127.0.0.1:21720 TCP - the H.323 call-signalling listener the injected
 *             configuration declares, held only for as long as the FSManager
 *             that read that configuration lives.
 *
 *   127.0.0.1:4569 UDP - the suite's own guard on the IAX2 default port, held
 *             for the whole suite.  It exists to make a socket NOT happen.
 *             FSManager's constructor allocates an IAX2EndPoint
 *             unconditionally (mod_opal.cpp:262-270) and IAX2EndPoint's
 *             constructor calls Initialise(), which listens on the WILDCARD
 *             address on this port and, if that succeeds, starts an
 *             IAX2Transmit and an IAX2Receiver thread.  Nothing mod_opal reads
 *             narrows the address, and the harness must not change the module,
 *             so instead the suite takes the port on loopback before any
 *             manager exists.  A loopback holder is enough to refuse a wildcard
 *             bind, so the listen fails, Initialise() returns before creating
 *             either thread, and no off-box-reachable socket is ever opened.
 *             OPAL reports the failure through PTRACE only and propagates no
 *             status, so nothing else about the module's observable behaviour
 *             changes.  Case 1 asserts all of this rather than assuming it, from
 *             inside the process and from outside it.  See the containment
 *             section further down for the measured bind semantics this relies
 *             on.
 *
 *   0.0.0.0:4569 UDP - what the above prevents.  This is what the suite opened
 *             before the guard existed: the IAX2 listener on the wildcard
 *             address, reachable from off-box for as long as the suite ran
 *             (CWE-668).  It must not reappear, and case 1 is what fails if it
 *             does.
 *
 * What the listener cases assert.  FSManager::Initialise() reports a
 * StartListener() failure only through PTRACE and propagates no status
 * (mod_opal.cpp:287-291), so a listener that cannot bind leaves neither a
 * FreeSWITCH log line nor a failed return behind.  The cases below therefore
 * observe the listener *configuration* that reached ReadConfig() - its name,
 * address and port - and not a socket proven to be bound.
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
 * A second listener name, used by the settings case alone.
 *
 * Every listener ReadConfig() stores is announced on the log (mod_opal.cpp:431),
 * and that line is the only publicly observable evidence of the listener-name
 * default that the case below it asserts.  If two cases announced the SAME name,
 * a line left over from the earlier one could satisfy the later one's
 * observation without the later parse having produced anything at all - the
 * assertion would then be reporting the wrong case's work.  Giving the settings
 * case a name that no other case ever produces removes that possibility by
 * construction: any line carrying this name is, by definition, not the line the
 * listener-name case is waiting for, and the capture's expected-name filter
 * rejects it and records it as foreign.
 */
#define TEST_OPAL_SETTINGS_LISTEN_NAME "opal-settings-listener"

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
 * The document served to the settings case, identical to the one above except
 * that its listener carries TEST_OPAL_SETTINGS_LISTEN_NAME.
 *
 * The distinct name is not decoration.  The settings case parses a configuration
 * that declares a listener, so it necessarily announces one on the log, and the
 * case that follows it observes exactly that kind of announcement.  Naming the
 * two listeners differently is what makes the later observation unambiguous: see
 * the note beside TEST_OPAL_SETTINGS_LISTEN_NAME.  The listener stanza itself
 * cannot simply be dropped from this document - a configuration without one
 * drives FSManager::Initialise() into its StartListener("") wildcard bind
 * (mod_opal.cpp:284-285), and every injected document in this suite therefore
 * carries an explicit loopback listener on a fixed high port.
 */
static const char TEST_OPAL_CONFIG_XML_SETTINGS[] =
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
					"<listener name=\"" TEST_OPAL_SETTINGS_LISTEN_NAME "\">"
						"<param name=\"h323-ip\" value=\"" TEST_OPAL_LISTEN_ADDRESS "\"/>"
						"<param name=\"h323-port\" value=\"" TEST_OPAL_LISTEN_PORT "\"/>"
					"</listener>"
				"</listeners>"
			"</configuration>"
		"</section>"
	"</document>";

/*
 * A third configuration document whose single <listener> element carries NO
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
 * DELIVERY IS ASYNCHRONOUS, and the consequence is a cross-case hazard rather
 * than a mere need to wait.  switch_log_printf() only enqueues the node
 * (switch_queue_trypush(LOG_QUEUE, node), src/switch_log.c:732); bound loggers
 * are invoked later, from log_thread(), which pops the queue and calls each
 * binding whose level admits the node (src/switch_log.c:501-518).  So a test
 * that read the captured value straight after ReadConfig() returned would be
 * racing that thread - and, worse, a listener line produced by an EARLIER case
 * can still be sitting in LOG_QUEUE when this case binds its logger, and would
 * then be handed to this logger as though this case had produced it.
 *
 * TWO INDEPENDENT MECHANISMS make the observation exact, and neither consults
 * the clock:
 *
 *   1. A QUEUE BARRIER.  Arming binds the logger and then emits a sentinel line
 *      of its own, carrying a token unique to that arming, and refuses to accept
 *      any listener line until the logger has SEEN that sentinel come back.
 *      LOG_QUEUE is one FIFO (src/switch_log.c:61, created at :755) drained by
 *      exactly one consumer (log_thread's blocking switch_queue_pop at :501), so
 *      once the sentinel has been dispatched every node enqueued before it has
 *      necessarily been dispatched already.  Anything left pending from an
 *      earlier case therefore arrives strictly BEFORE the barrier opens and is
 *      discarded, and everything this case goes on to produce is enqueued
 *      strictly AFTER the sentinel and is admitted.  That is a proof about queue
 *      order, not an estimate: it replaces the timestamp look-back an earlier
 *      revision used, which could only ever guess how much wall time separates
 *      two cases and gave no protection at all against a node that was still
 *      queued when the logger bound.
 *
 *   2. AN EXPECTED-NAME FILTER.  Arming also states the listener name the case
 *      is about to provoke, and only that name satisfies the wait.  A
 *      post-barrier line naming anything else is recorded separately as foreign,
 *      so the case can assert that no other listener line appeared inside its
 *      window rather than silently accepting the first marker it sees.  Together
 *      with the deliberately distinct name the settings case announces
 *      (TEST_OPAL_SETTINGS_LISTEN_NAME), a leaked line can therefore neither be
 *      mistaken for this case's line nor vacuously satisfy it.
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
 * Barrier tuning.  The overall bound is generous because it is only ever spent
 * on the failure path, and the slice is the interval after which the sentinel is
 * re-emitted: switch_queue_trypush() DROPS a node when the queue is full
 * (src/switch_log.c:732-734), and a barrier that hung forever on one dropped
 * sentinel would be a worse failure mode than one that simply says it again.
 */
#define TEST_OPAL_LOG_BARRIER_TIMEOUT_MS 5000
#define TEST_OPAL_LOG_BARRIER_SLICE_MS 250

/*
 * Sentinel text.  The trailing epoch makes each arming's token unique, so a
 * sentinel still queued from a previous arming cannot open this one's barrier
 * early.  It shares no substring with the listener marker above, so neither
 * search can ever match the other's line.
 */
#define TEST_OPAL_LOG_BARRIER_PREFIX "mod-opal-test-log-barrier-"

/* Bound on every listener name this suite records, expected or foreign. */
#define TEST_OPAL_LOG_NAME_MAX 128

/*
 * Capture state.
 *
 * The mutex and the condition variable live in a pool this capture owns, NOT in
 * fst_pool.  fst_pool is destroyed by FST_TEARDOWN_BEGIN before the teardown
 * body runs and lasts exactly one case, whereas a bound logger is reachable from
 * log_thread until switch_log_unbind_logger() returns.  Holding the two on
 * fst_pool would mean that any exit which skipped the disarm - and the suite's
 * safety sweep runs in a LATER case, by which time that pool is long gone - left
 * a live binding pointing at freed memory.  A capture-owned pool makes the disarm
 * safe from anywhere, which is precisely what lets the sweep call it.
 *
 * The name buffers are deliberately static rather than pool-backed, so a name a
 * case has already observed stays readable after the capture is torn down.
 */
static switch_memory_pool_t *test_opal_log_pool = NULL;
static switch_mutex_t *test_opal_log_mutex = NULL;
static switch_thread_cond_t *test_opal_log_cond = NULL;
static int test_opal_log_bound = 0;
static unsigned int test_opal_log_epoch = 0;
static char test_opal_log_sentinel[64];
static int test_opal_log_barrier_seen = 0;
static char test_opal_log_expected_name[TEST_OPAL_LOG_NAME_MAX];
static char test_opal_log_listener_name[TEST_OPAL_LOG_NAME_MAX];
static int test_opal_log_captured = 0;
static char test_opal_log_foreign_name[TEST_OPAL_LOG_NAME_MAX];
static int test_opal_log_foreign = 0;

/*
 * Bound logger.  Opens the barrier when this arming's sentinel comes back, and
 * thereafter classifies every listener line it is handed.
 *
 * Nothing is accepted before the barrier opens.  A node dispatched at that point
 * was enqueued no later than the sentinel, so it belongs to whatever ran before
 * this arming and is not this case's evidence - see the barrier note above for
 * why single-consumer FIFO order makes that a certainty rather than a guess.
 *
 * After the barrier, a line whose name matches the armed expectation exactly
 * satisfies the wait; only the FIRST such line is kept, so a later one cannot
 * overwrite an observation the waiter has not read yet.  A line naming anything
 * else is recorded once as foreign and deliberately does NOT satisfy the wait:
 * the case reports it instead, which is what turns a stray announcement from
 * something that could quietly become the answer into something that fails
 * loudly and names itself.
 *
 * The name comparison is exact: strncmp over the announced length, plus the
 * requirement that the expected name ends there, so no name can match a prefix
 * of another.  test_opal_log_capture_start() refuses an empty expectation, which
 * would otherwise match every name through that same zero-length comparison.
 *
 * The mutex pointer is checked before it is used, so a stray invocation arriving
 * when nothing is armed returns without touching capture state at all.
 */
static switch_status_t test_opal_listener_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	const char *marker = NULL;
	const char *start = NULL;
	const char *end = NULL;

	(void) level;

	if (!test_opal_log_mutex || !node || !node->content) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(test_opal_log_mutex);

	/* The barrier.  Until this arming's own sentinel has been observed, every
	 * node handed over predates the arming and is ignored. */
	if (!test_opal_log_barrier_seen) {
		if (strstr(node->content, test_opal_log_sentinel)) {
			test_opal_log_barrier_seen = 1;
			switch_thread_cond_broadcast(test_opal_log_cond);
		}

		switch_mutex_unlock(test_opal_log_mutex);

		return SWITCH_STATUS_SUCCESS;
	}

	marker = strstr(node->content, TEST_OPAL_LISTENER_LOG_MARKER);

	if (marker) {
		start = marker + (sizeof(TEST_OPAL_LISTENER_LOG_MARKER) - 1);
		end = strchr(start, '\'');

		if (end) {
			switch_size_t length = (switch_size_t) (end - start);

			if (length < TEST_OPAL_LOG_NAME_MAX) {
				if (!strncmp(start, test_opal_log_expected_name, length) && !test_opal_log_expected_name[length]) {
					if (!test_opal_log_captured) {
						memcpy(test_opal_log_listener_name, start, length);
						test_opal_log_listener_name[length] = '\0';
						test_opal_log_captured = 1;
						switch_thread_cond_broadcast(test_opal_log_cond);
					}
				} else if (!test_opal_log_foreign) {
					memcpy(test_opal_log_foreign_name, start, length);
					test_opal_log_foreign_name[length] = '\0';
					test_opal_log_foreign = 1;
				}
			}
		}
	}

	switch_mutex_unlock(test_opal_log_mutex);

	return SWITCH_STATUS_SUCCESS;
}

/*
 * Enqueue this arming's sentinel line.
 *
 * DEBUG is deliberate: the sentinel has to clear exactly the same two gates the
 * listener line clears, or the barrier would be measuring a different queue from
 * the one that carries the evidence.  Those gates are the core's runtime level,
 * which FST_TEST_BEGIN raises to SWITCH_LOG_DEBUG for the duration of every case
 * (switch_test.h:448-449, against src/switch_log.c:600-601), and the MAX_LEVEL
 * gate that binding this logger at DEBUG raises (src/switch_log.c:461-462, read
 * at :706).
 *
 * Calling this more than once is not merely tolerated, it is intended.  A node is
 * dropped outright when the queue is full (src/switch_log.c:732-734), and opening
 * the barrier on a later sentinel drains strictly more of the queue than opening
 * it on an earlier one, so a repeat can only strengthen the guarantee.
 */
static void test_opal_log_emit_sentinel(void)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s\n", test_opal_log_sentinel);
}

/*
 * Emit the sentinel and wait for the logger to report it back, which is the
 * moment the queue is known to be drained past this arming.
 *
 * The wait is split into slices so the sentinel can be repeated if it never
 * arrives, and the deadline is re-read from the clock on every iteration so a
 * spurious wakeup cannot extend it.  The clock is used ONLY to bound the wait -
 * it is never compared against a log node's own stamp, which is the comparison
 * this design exists to eliminate.
 */
static switch_status_t test_opal_log_barrier(void)
{
	switch_time_t expiration = switch_time_now() + (TEST_OPAL_LOG_BARRIER_TIMEOUT_MS * 1000);
	switch_time_t now = 0;
	int seen = 0;

	if (!test_opal_log_mutex || !test_opal_log_cond) {
		return SWITCH_STATUS_FALSE;
	}

	while (!seen && (now = switch_time_now()) < expiration) {
		switch_time_t slice = now + (TEST_OPAL_LOG_BARRIER_SLICE_MS * 1000);

		if (slice > expiration) {
			slice = expiration;
		}

		test_opal_log_emit_sentinel();

		switch_mutex_lock(test_opal_log_mutex);

		while (!test_opal_log_barrier_seen && (now = switch_time_now()) < slice) {
			switch_thread_cond_timedwait(test_opal_log_cond, test_opal_log_mutex, slice - now);
		}

		seen = test_opal_log_barrier_seen;

		switch_mutex_unlock(test_opal_log_mutex);
	}

	return seen ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*
 * Drop the capture's synchronisation objects and the pool they live in.  Only
 * ever called when no binding is live, because the pointers are cleared before
 * the pool goes: the mutex and condition variable are pool-backed, and APR
 * reclaims both through the pool's own cleanup, so destroying the pool is the
 * complete release.
 */
static void test_opal_log_capture_release(void)
{
	test_opal_log_mutex = NULL;
	test_opal_log_cond = NULL;

	if (test_opal_log_pool) {
		switch_core_destroy_memory_pool(&test_opal_log_pool);
	}
}

/*
 * Disarm capture.  Idempotent by design, because the suite's safety sweep calls
 * it without knowing whether anything is armed.
 *
 * The logger is unbound FIRST.  switch_log_unbind_logger() takes the same
 * BINDLOCK that log_thread() holds across every dispatch
 * (src/switch_log.c:511-518), so once it returns no invocation of this logger can
 * still be in flight and the memory behind the mutex and condition variable can
 * be released; doing it the other way round would be a use-after-free with a
 * window exactly one dispatch wide.
 *
 * The unbind status is returned rather than swallowed - an orphaned binding would
 * survive into every later case in the same process - but a call made when
 * nothing is armed reports success, since there is nothing to fail at.
 */
static switch_status_t test_opal_log_capture_stop(void)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (test_opal_log_bound) {
		status = switch_log_unbind_logger(test_opal_listener_logger);
		test_opal_log_bound = 0;
	}

	test_opal_log_capture_release();

	return status;
}

/*
 * Arm capture for one expected listener name: take a private pool, build the
 * mutex and condition variable in it, clear every observation, mint a fresh
 * sentinel token, bind the logger, and then hold until the barrier opens.
 *
 * Arming therefore reports success only once the queue is provably drained past
 * this point, so a caller that gets SUCCESS knows that every line it goes on to
 * see was produced after it asked.
 *
 * Safe to call twice in a row: any previous arming is wound down first, so a
 * second observation can inherit neither the first one's value nor a line the
 * first one produced.  The state is reset before the bind rather than after, so
 * no dispatch can ever observe a half-armed capture, and no lock is needed for
 * the reset because no binding exists yet.
 *
 * Every early return releases exactly what it had acquired, and a barrier that
 * fails to open disarms completely rather than leaving a live binding behind.
 */
static switch_status_t test_opal_log_capture_start(const char *expected_name)
{
	switch_status_t status = SWITCH_STATUS_FALSE;

	(void) test_opal_log_capture_stop();

	if (zstr(expected_name)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_core_new_memory_pool(&test_opal_log_pool) != SWITCH_STATUS_SUCCESS || !test_opal_log_pool) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(&test_opal_log_mutex, SWITCH_MUTEX_NESTED, test_opal_log_pool) != SWITCH_STATUS_SUCCESS) {
		test_opal_log_capture_release();

		return SWITCH_STATUS_FALSE;
	}

	if (switch_thread_cond_create(&test_opal_log_cond, test_opal_log_pool) != SWITCH_STATUS_SUCCESS) {
		test_opal_log_capture_release();

		return SWITCH_STATUS_FALSE;
	}

	test_opal_log_listener_name[0] = '\0';
	test_opal_log_foreign_name[0] = '\0';
	test_opal_log_captured = 0;
	test_opal_log_foreign = 0;
	test_opal_log_barrier_seen = 0;
	switch_copy_string(test_opal_log_expected_name, expected_name, sizeof(test_opal_log_expected_name));
	switch_snprintf(test_opal_log_sentinel, sizeof(test_opal_log_sentinel), "%s%u",
					TEST_OPAL_LOG_BARRIER_PREFIX, ++test_opal_log_epoch);

	status = switch_log_bind_logger(test_opal_listener_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);

	if (status != SWITCH_STATUS_SUCCESS) {
		test_opal_log_capture_release();

		return status;
	}

	test_opal_log_bound = 1;

	status = test_opal_log_barrier();

	if (status != SWITCH_STATUS_SUCCESS) {
		(void) test_opal_log_capture_stop();
	}

	return status;
}

/*
 * Wait up to timeout_ms for the awaited listener line, then return the captured
 * name, or NULL if it never arrived.  Re-reads the clock on every iteration so a
 * spurious wakeup cannot extend the deadline, which is the shape
 * tests/unit/switch_log.c uses.
 *
 * The clock is only ever used to measure an elapsed interval for a relative
 * switch_thread_cond_timedwait() timeout; nothing here is compared against a log
 * node's own stamp, so no two clocks can disagree about anything that matters.
 *
 * A NULL return is the real verdict this suite reads: the barrier and the
 * expected-name filter between them mean the wait can only be satisfied by a line
 * this arming provoked, carrying exactly the name it asked for.
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
 * The first listener line that appeared inside the capture window carrying a name
 * the arming did not ask for, or NULL if none did.
 *
 * This is the assertion surface for the hazard the barrier exists to close.  The
 * barrier already discards anything enqueued before arming, so a non-NULL answer
 * here means a listener was announced DURING the window that the case did not
 * account for - a second listener in the document, a leaked provider still
 * serving an earlier case's configuration, or a name the parser substituted
 * unexpectedly.  Reporting it by name is what makes the difference between a test
 * that quietly accepts the wrong evidence and one that says which evidence it
 * rejected.
 */
static const char *test_opal_log_foreign_listener_name(void)
{
	const char *foreign = NULL;

	if (!test_opal_log_mutex) {
		return NULL;
	}

	switch_mutex_lock(test_opal_log_mutex);

	if (test_opal_log_foreign) {
		foreign = test_opal_log_foreign_name;
	}

	switch_mutex_unlock(test_opal_log_mutex);

	return foreign;
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
 * ---------------------------------------------------------------------------
 * Containment of the toolkit's own side effects
 * ---------------------------------------------------------------------------
 *
 * Two side effects are not the suite's to want: PTLib's plugin search path and
 * OPAL's IAX2 listener.  Both are established by constructors this suite has to
 * call, so both have to be contained BEFORE the first of those constructors
 * runs.  Neither is contained by mod_opal for us in the cases that build objects
 * directly.
 *
 *
 * (1) PLUGIN SEARCH PATH
 *
 * Constructing any PProcess makes PTLib enumerate its plugin directory and
 * dlopen what it finds there.  The directory comes from the environment:
 * PTLIBPLUGINDIR is the current name and PWLIBPLUGINDIR the legacy one, and both
 * strings are present in the libpt this module links against, so both are live
 * inputs.  A test binary is run by `make check' out of an environment nobody
 * audits, which turns an inherited value into an arbitrary-code-execution seam
 * (CWE-427, CWE-829).
 *
 * mod_opal does set PTLIBPLUGINDIR to "/no/thanks" - but it does so inside
 * mod_opal_load() (mod_opal.cpp:108), which runs in exactly one case near the
 * end of this suite, and it never touches the legacy name at all.  Every case
 * that constructs an FSProcess directly - which is every case before the load
 * case - therefore ran unprotected.
 *
 * So both variables are pinned here, unconditionally and before the first
 * PProcess, to a fixed directory that does not exist: nothing can be enumerated
 * in a directory that is not there.  "/no/thanks" is deliberately the same value
 * and putenv() the same idiom the production module already uses, so harness and
 * module agree.  putenv() REPLACES an existing entry of the same name, which is
 * what makes this effective against an inherited value rather than merely a
 * default for an unset one; a string literal has static storage, which is what
 * makes putenv() safe here.
 *
 * It is wired in two places on purpose - the suite setup hook, which FCTX runs
 * before every case body, and the acquire helper below, which is the only place
 * in the suite that constructs a PProcess.  Either alone suffices today; both
 * means no re-ordering and no new case can reintroduce the exposure.  It is
 * idempotent, so paying twice costs nothing.
 */
#define TEST_OPAL_PLUGIN_DIR "/no/thanks"

static int test_opal_plugin_path_pinned = 0;

static void test_opal_pin_plugin_path(void)
{
	if (test_opal_plugin_path_pinned) {
		return;
	}

	(void) putenv((char *) "PTLIBPLUGINDIR=" TEST_OPAL_PLUGIN_DIR);
	(void) putenv((char *) "PWLIBPLUGINDIR=" TEST_OPAL_PLUGIN_DIR);

	test_opal_plugin_path_pinned = 1;
}

/*
 * True when both plugin-directory variables read back as the pinned value.  Used
 * by the containment case to assert the pinning rather than assume it.
 */
static int test_opal_plugin_path_is_pinned(void)
{
	const char *ptlib = getenv("PTLIBPLUGINDIR");
	const char *pwlib = getenv("PWLIBPLUGINDIR");

	return ptlib && pwlib && !strcmp(ptlib, TEST_OPAL_PLUGIN_DIR) && !strcmp(pwlib, TEST_OPAL_PLUGIN_DIR);
}

/*
 * (2) THE IAX2 WILDCARD LISTENER
 *
 * FSManager's constructor allocates an IAX2EndPoint unconditionally
 * (mod_opal.cpp:262-270).  IAX2EndPoint's own constructor calls its Initialise(),
 * which does `sock = new PUDPSocket(GetDefaultSignalPort())' followed by
 * `sock->Listen(INADDR_ANY, 0, sock->GetPort())' and, ONLY if that listen
 * succeeds, constructs the IAX2Transmit and IAX2Receiver threads.  So merely
 * constructing a manager opened UDP 4569 on the WILDCARD address - reachable
 * from off-box for as long as the suite ran - and started two live threads.  No
 * configuration parameter mod_opal reads narrows it, and the harness must not
 * change the module, so the address cannot be fixed at its source.
 *
 * It can, however, be made unavailable.  The suite takes the port itself, on
 * loopback only, before any manager exists.  Three measured properties of the
 * Linux UDP bind make that both sufficient and safe:
 *
 *   - a holder of 127.0.0.1:4569 blocks a subsequent bind of 0.0.0.0:4569,
 *     with or without SO_REUSEADDR on the second socket.  So OPAL's listen
 *     fails, sock is never bound, and Initialise() returns false before it can
 *     construct either thread.  Containment therefore removes the exposed
 *     socket AND the two threads, not just the socket;
 *
 *   - a bind of 127.0.0.1:4569 leaves any OTHER local address free, so the
 *     guard reaches no further than it must, and probing a routable local
 *     address is an exact test for "does anything own the wildcard" - it
 *     succeeds while only the loopback guard is held and fails the moment a
 *     wildcard owner exists.  That is what the containment case asserts;
 *
 *   - OPAL tolerates the failure: it is reported through PTRACE only, no status
 *     is propagated, and the suite passes unchanged.  Verified independently by
 *     holding the port from another process.
 *
 * SO_REUSEADDR is deliberately NOT set on the guard.  It is not needed to take
 * a free port, and setting it would weaken exactly the exclusion the guard
 * exists to create.
 *
 * The guard lives in its own pool rather than fst_pool because fst_pool lasts
 * one case (FST_SETUP_BEGIN creates it, FST_TEARDOWN_BEGIN destroys it) while
 * the guard must span the whole suite.  It is acquired from the setup hook, so
 * it is held before any case body runs, and released by the suite-wide sweep -
 * where re-acquisition by the next setup makes an early release harmless.
 */
#define TEST_OPAL_GUARD_ADDRESS "127.0.0.1"

static switch_memory_pool_t *test_opal_guard_pool = NULL;
static switch_socket_t *test_opal_iax2_guard = NULL;

/*
 * Set when the guard's bind was refused, which means some other holder already
 * owns the port.  That produces the same containment - a wildcard bind will be
 * refused too - so the containment case accepts it, while still refusing to
 * accept a guard that failed for any other reason.
 */
static int test_opal_iax2_port_has_foreign_owner = 0;

/*
 * Take UDP DefaultUdpPort on loopback.  Idempotent: reports success when the
 * guard is already held.  Modelled on test_port() in
 * src/switch_core_port_allocator.c, which is the tree's own way of asking
 * whether a port can be bound.
 */
static switch_status_t test_opal_iax2_guard_acquire(void)
{
	switch_sockaddr_t *guard_addr = NULL;

	if (test_opal_iax2_guard) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (!test_opal_guard_pool && switch_core_new_memory_pool(&test_opal_guard_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_sockaddr_new(&guard_addr, TEST_OPAL_GUARD_ADDRESS,
							(switch_port_t) IAX2EndPoint::DefaultUdpPort, test_opal_guard_pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_socket_create(&test_opal_iax2_guard, switch_sockaddr_get_family(guard_addr),
							 SOCK_DGRAM, 0, test_opal_guard_pool) != SWITCH_STATUS_SUCCESS) {
		test_opal_iax2_guard = NULL;
		return SWITCH_STATUS_FALSE;
	}

	/* No SWITCH_SO_REUSEADDR here, on purpose - see the note above. */

	if (switch_socket_bind(test_opal_iax2_guard, guard_addr) != SWITCH_STATUS_SUCCESS) {
		switch_socket_close(test_opal_iax2_guard);
		test_opal_iax2_guard = NULL;
		test_opal_iax2_port_has_foreign_owner = 1;
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

/*
 * Release the guard and its pool.  Idempotent.
 */
static void test_opal_iax2_guard_release(void)
{
	if (test_opal_iax2_guard) {
		switch_socket_close(test_opal_iax2_guard);
		test_opal_iax2_guard = NULL;
	}

	if (test_opal_guard_pool) {
		switch_core_destroy_memory_pool(&test_opal_guard_pool);
	}
}

/*
 * True when the IAX2 default port cannot be bound on the wildcard address by
 * anyone - either because this suite holds it on loopback or because another
 * holder already had it.
 */
static int test_opal_iax2_containment_in_effect(void)
{
	return (test_opal_iax2_guard != NULL) || test_opal_iax2_port_has_foreign_owner;
}

/*
 * Can `ip' be bound on UDP `port' right now?  Used as the containment probe: a
 * routable local address stays bindable while only the loopback guard is held
 * and stops being bindable the moment something owns the wildcard.  The socket
 * is closed again immediately, so the probe leaves nothing behind.
 */
static switch_bool_t test_opal_udp_port_is_bindable(const char *ip, switch_port_t port)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *addr = NULL;
	switch_socket_t *sock = NULL;
	switch_bool_t bindable = SWITCH_FALSE;

	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}

	if (switch_sockaddr_new(&addr, ip, port, pool) == SWITCH_STATUS_SUCCESS) {
		if (switch_socket_create(&sock, switch_sockaddr_get_family(addr), SOCK_DGRAM, 0, pool) == SWITCH_STATUS_SUCCESS) {
			if (switch_socket_bind(sock, addr) == SWITCH_STATUS_SUCCESS) {
				bindable = SWITCH_TRUE;
			}
			switch_socket_close(sock);
		}
	}

	switch_core_destroy_memory_pool(&pool);

	return bindable;
}

/*
 * Return the suite-local PTLib process, creating it on first use.  Every
 * FSManager needs one to exist, because OpalManager's constructor reads
 * PProcess::Current().
 *
 * The plugin search path is pinned FIRST, on every call, because constructing a
 * PProcess is what triggers PTLib's plugin enumeration and there is no second
 * chance once it has run.
 */
static FSProcess *test_opal_acquire_process(void)
{
	test_opal_pin_plugin_path();

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
 * Reclaim everything this suite can retain beyond a single case, in one call.
 *
 * WHY EVERY STEP IS IDEMPOTENT, so this is safe whether the state exists or not:
 *   - switch_xml_unbind_search_function_ptr() searches the binding list under the
 *     write lock and reports SWITCH_STATUS_FALSE without changing anything when
 *     the pointer is not registered, so calling it on an empty list is a no-op;
 *   - test_opal_log_capture_stop() unbinds only when a binding is actually live
 *     and reports success otherwise;
 *   - mod_opal_shutdown() is `delete opal_process; opal_process = NULL' and
 *     always returns success (mod_opal.cpp:136-138), and deleting a null pointer
 *     is a well-defined no-op, so it is safe with or without a prior load and
 *     safe to repeat.  It is also the ONLY way to reclaim a process the module
 *     created, including after a load that failed halfway;
 *   - test_opal_release_process(), test_opal_module_pool_destroy() and
 *     test_opal_iax2_guard_release() all test their pointer first and null it
 *     afterwards.
 *
 * ORDER IS LOAD-BEARING:
 *   1. unbind the provider first, so nothing that follows can still resolve a
 *      configuration lookup through this suite's document;
 *   2. stop the capture next, so no bound logger can observe the teardown that
 *      follows;
 *   3. shut the module down, which is the last thing that touches module state
 *      allocated from the module-lifetime pool;
 *   4. release the suite's own PTLib process, so at most one PProcess-derived
 *      object has existed at any instant and none outlives the suite;
 *   5. forget the module interface, which is memory owned by the pool destroyed
 *      on the next line, and only then destroy that pool;
 *   6. release the IAX2 port guard last of all, so that nothing this suite
 *      built can still be holding an endpoint when the port becomes free again.
 *
 * WHY THIS IS NOT INVOKED FROM FST_TEARDOWN.  FST_TEARDOWN runs after EVERY case,
 * and this suite deliberately hands live state from the load case to the shutdown
 * case so that shutting it down can be asserted rather than assumed.  A teardown
 * that swept unconditionally would destroy precisely the state the last case
 * exists to observe.  The sweep is therefore anchored where it cannot do that: in
 * the failure branch of the case that retains state, and unconditionally in the
 * LAST declared case.  That placement is sufficient because a fatal check only
 * breaks out of the case body it appears in (switch_fct.h:3668-3669) and the
 * framework re-enters the fixture suite once per declared case
 * (switch_fct.h:3507-3516), so no failure anywhere can prevent the last case from
 * running its own sweep.
 */
static void test_opal_suite_state_cleanup(void)
{
	(void) test_opal_unbind_config();

	(void) test_opal_log_capture_stop();

	(void) mod_opal_shutdown();

	test_opal_release_process();

	test_opal_module_interface = NULL;

	test_opal_module_pool_destroy();

	/*
	 * Release the IAX2 port guard last, so it outlives every manager this suite
	 * built.  Releasing it early is harmless anyway: the setup hook re-acquires
	 * it before the next case body runs.
	 */
	test_opal_iax2_guard_release();
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
 *   1. the containment case runs first, so the preconditions every later case
 *      relies on are established as facts before anything depends on them, and
 *      so the IAX2 listener is observed at the earliest moment it could exist;
 *   2. the configuration-absent failure follows, still before any successful
 *      parse or load can leave module state behind;
 *   3. the three cases that only build objects come next, so they observe a
 *      pristine OPAL media-format registry - FSManager::Initialise() mutates
 *      the process-global registry;
 *   4. the load case follows and takes over the PTLib process singleton;
 *   5. the shutdown case is declared last so it observes a fully initialised
 *      module, and its result is asserted rather than discarded.
 */
FST_CORE_BEGIN("conf_opal")
{
	FST_SUITE_BEGIN(mod_opal)
	{
		FST_SETUP_BEGIN()
		{
			/*
			 * Contain the toolkit's two unwanted side effects before any case
			 * body can trigger them.  FCTX runs this hook once per declared
			 * case, ahead of the body, which makes it the earliest point no
			 * case ordering can bypass.  Both calls are idempotent.  Neither
			 * asserts here: a fixture cannot fail a case, so the containment
			 * case below asserts both outcomes instead.
			 */
			test_opal_pin_plugin_path();
			(void) test_opal_iax2_guard_acquire();
		}
		FST_SETUP_END()

		/*
		 * Both fixtures are mandatory even though neither has a body: the setup
		 * hook is the only thing that creates fst_pool and starts the soft timer,
		 * and every case fatally requires both, while the teardown hook is the
		 * only thing that destroys that pool.
		 *
		 * The teardown body stays EMPTY on purpose, and the invariant that makes
		 * that safe is structural rather than accidental: no case in this suite
		 * makes a fatal check after acquiring a resource that nothing else
		 * reclaims.  Registrations and pool creations are performed as plain
		 * statements and checked non-fatally, each acquiring case ends with a
		 * single unconditional tail that releases what it took, and anything
		 * deliberately handed to a later case is swept by
		 * test_opal_suite_state_cleanup() in the last declared case.
		 *
		 * Two things a fatal check CAN still strand, and why neither matters.  The
		 * shared PTLib process, which every socket-free case acquires before its
		 * first assertion: it has two independent reclamation paths, the load
		 * case's own handover and the last case's sweep.  And anything allocated
		 * from fst_pool, which FST_TEARDOWN_BEGIN destroys on the way in before
		 * this body would ever run.
		 *
		 * A sweep here would also be actively wrong, not merely redundant: the
		 * load case hands a loaded module, its process and its pool to the
		 * shutdown case on purpose, and a teardown running after every case would
		 * demolish exactly the state the shutdown case exists to observe.
		 */
		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * Case 1 - the toolkit's side effects are contained.
		 *
		 * Declared first because it asserts the preconditions every later case
		 * depends on, and because the thing it is asserting about - the IAX2
		 * listener - appears the instant the first FSManager is constructed.
		 *
		 * Three independent observations, in increasing strength:
		 *
		 *   - both plugin-directory variables read back as the pinned
		 *     nonexistent directory, so no later PProcess can enumerate an
		 *     inherited path;
		 *
		 *   - a manager is constructed and its IAX2 endpoint reports
		 *     InitialisedOK() false.  That is the direct, in-process proof that
		 *     IAX2EndPoint::Initialise()'s `sock->Listen(INADDR_ANY, ...)' was
		 *     refused, because Initialise() returns before constructing the
		 *     transmitter and receiver when the listen fails and
		 *     InitialisedOK() is exactly `transmitter != NULL && receiver !=
		 *     NULL'.  So this single check covers both the unexposed socket and
		 *     the two threads that were never started;
		 *
		 *   - while that manager is alive, a routable local address is still
		 *     bindable on the same UDP port.  A wildcard owner would make that
		 *     bind fail, so its success is an out-of-process proof that nothing
		 *     is listening off-box.  It is skipped, not failed, when the only
		 *     address discoverable on the host is loopback - there the probe
		 *     would be indistinguishable from the guard itself and would prove
		 *     nothing either way.
		 */
		FST_TEST_BEGIN(toolkit_side_effects_are_contained)
		{
			char local_ip[80] = "";
			int local_mask = 0;
			switch_bool_t have_routable_ip = SWITCH_FALSE;

			/* the plugin search path was pinned before any PProcess existed */
			fst_requires(test_opal_plugin_path_is_pinned());

			/* the port is unavailable to a wildcard bind, by us or by another
			 * holder - either way OPAL cannot take it */
			fst_requires(test_opal_iax2_containment_in_effect());

			if (switch_find_local_ip(local_ip, sizeof(local_ip), &local_mask, AF_INET) == SWITCH_STATUS_SUCCESS
				&& *local_ip && strcmp(local_ip, TEST_OPAL_GUARD_ADDRESS)) {
				have_routable_ip = SWITCH_TRUE;
			}

			fst_requires(test_opal_acquire_process() != NULL);

			{
				FSManager manager;
				IAX2EndPoint *iax2_endpoint = manager.FindEndPointAs < IAX2EndPoint > ("iax2");

				fst_requires(iax2_endpoint != NULL);

				/* the listen was refused, so neither the socket nor the
				 * transmitter/receiver threads came into being */
				fst_check(iax2_endpoint->InitialisedOK() == PFalse);

				if (have_routable_ip) {
					/* nothing owns the wildcard while the manager is alive */
					fst_check(test_opal_udp_port_is_bindable(local_ip,
															(switch_port_t) IAX2EndPoint::DefaultUdpPort) == SWITCH_TRUE);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
									  "no routable IPv4 address on this host; off-box reachability probe skipped\n");
				}
			}

			/* and the guard still holds the port after the manager is gone */
			fst_check(test_opal_iax2_containment_in_effect());
		}
		FST_TEST_END()

		/*
		 * Case 2 - configuration absent.
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
			/* Fatal precondition: the IAX2 wildcard listener must be unable to
			 * bind before a manager is constructed.  Asserted per case, not just
			 * once, so no re-ordering can leave a manager built without it. */
			fst_requires(test_opal_iax2_containment_in_effect());

			{
				FSManager manager;

				status = manager.ReadConfig(false);
			}

			fst_check(status == SWITCH_STATUS_FALSE);
		}
		FST_TEST_END()

		/*
		 * Case 3 - dual endpoint construction.
		 *
		 * FSManager's constructor allocates an H.323, an IAX2 and a FreeSWITCH
		 * local endpoint.  The first two are private members, so they are
		 * observed through the inherited public OpalManager::FindEndPoint()
		 * lookup rather than reached for directly, which keeps the assertion
		 * valid across any refactoring of those members.
		 *
		 * Constructing a manager does attempt one bind - IAX2EndPoint's
		 * constructor tries to listen on the wildcard address - and case 1
		 * establishes that the attempt is refused before this case runs.  What
		 * is asserted here is only which endpoints the constructor allocated;
		 * an endpoint exists whether or not its listener started.
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
			/* Fatal precondition: the IAX2 wildcard listener must be unable to
			 * bind before a manager is constructed.  Asserted per case, not just
			 * once, so no re-ordering can leave a manager built without it. */
			fst_requires(test_opal_iax2_containment_in_effect());

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
		 * Case 4 - default signalling port.
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
		 * Case 5 - settings parsed from the injected configuration.
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
		 *
		 * The document served here is the settings variant, whose listener is
		 * named distinctly from every other listener this suite declares.  This
		 * case must declare a listener - no injected document may omit the stanza -
		 * and it therefore announces one on the log, which is the same channel the
		 * case after it observes.  A distinct name is what stops this
		 * announcement from ever being mistaken for that case's evidence.
		 */
		FST_TEST_BEGIN(settings_from_injected_configuration)
		{
			switch_status_t status = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);
			/* Fatal precondition: the IAX2 wildcard listener must be unable to
			 * bind before a manager is constructed.  Asserted per case, not just
			 * once, so no re-ordering can leave a manager built without it. */
			fst_requires(test_opal_iax2_containment_in_effect());
			fst_requires(test_opal_bind_config_document(TEST_OPAL_CONFIG_XML_SETTINGS) == SWITCH_STATUS_SUCCESS);

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
		 * Case 6 - listener name defaults to "unnamed".
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
		 * WHAT EACH HALF PROVES, now that arming states the name it expects.  The
		 * verdict is that the awaited line ARRIVED: the wait can only be satisfied
		 * by a post-barrier line naming exactly what was asked for, so a NULL
		 * return means the parser did not announce that name inside this case's
		 * own window.  The equality check that follows restates the verdict in the
		 * form a reader expects, and would catch a filter that ever loosened into
		 * a prefix test.  The foreign-name check is the third assertion and the
		 * one that closes the hazard: it fails if ANY other listener was announced
		 * during the window, naming the intruder instead of quietly adopting it.
		 *
		 * Each half is fully wound down before the next begins: the manager
		 * leaves scope, the logger is unbound and the provider is unregistered,
		 * so the loopback listener is closed again before the same port is
		 * reused.
		 *
		 * This case acquires TWO things that nothing else would reclaim - a
		 * registered provider and a bound logger holding its own pool - so
		 * neither acquisition is made inside a fatal assertion.  Each is performed
		 * as a plain statement, checked non-fatally, and released by an
		 * unconditional tail that runs whatever the observation did; the
		 * observation itself is skipped, with one explicit failure, if either
		 * acquisition did not succeed.  The only fatal check is the process
		 * precondition, which precedes both acquisitions: without a live PProcess
		 * the FSManager constructor would terminate the whole binary rather than
		 * fail a case.
		 */
		FST_TEST_BEGIN(listener_name_defaults_to_unnamed)
		{
			const char *observed_name = NULL;
			switch_status_t bound = SWITCH_STATUS_FALSE;
			switch_status_t armed = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);
			/* Fatal precondition: the IAX2 wildcard listener must be unable to
			 * bind before a manager is constructed.  Asserted per case, not just
			 * once, so no re-ordering can leave a manager built without it. */
			fst_requires(test_opal_iax2_containment_in_effect());

			/* Half one: a <listener> with no name attribute takes the default. */
			bound = test_opal_bind_config_document(TEST_OPAL_CONFIG_XML_UNNAMED_LISTENER);
			fst_xcheck(bound == SWITCH_STATUS_SUCCESS,
					   "the opal.conf provider must be registered before the unnamed-listener document is parsed");

			armed = test_opal_log_capture_start("unnamed");
			fst_xcheck(armed == SWITCH_STATUS_SUCCESS,
					   "the listener capture must be armed, and its queue barrier open, before the document is parsed");

			if (bound == SWITCH_STATUS_SUCCESS && armed == SWITCH_STATUS_SUCCESS) {
				FSManager manager;
				switch_status_t status = manager.ReadConfig(false);

				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					observed_name = test_opal_log_wait(TEST_OPAL_LOG_TIMEOUT_MS);
					fst_check(observed_name != NULL);

					if (observed_name) {
						fst_check_string_equals(observed_name, "unnamed");
					}

					fst_xcheck(test_opal_log_foreign_listener_name() == NULL,
							   "no listener other than the awaited one may be announced inside the capture window");
				}
			} else {
				fst_fail("the unnamed-listener observation was not attempted because a precondition failed");
			}

			/*
			 * Single unconditional tail.  Each resource is released only if it was
			 * actually acquired, so a precondition failure cannot turn into a
			 * second, misleading failure report about releasing something that was
			 * never taken.
			 */
			if (armed == SWITCH_STATUS_SUCCESS) {
				fst_check(test_opal_log_capture_stop() == SWITCH_STATUS_SUCCESS);
			}

			if (bound == SWITCH_STATUS_SUCCESS) {
				fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
			}

			/*
			 * Half two: the contrast.  An identical document that does carry a
			 * name attribute must yield that name verbatim, which is what proves
			 * "unnamed" above came from the default branch and not from a
			 * capture that reports the same string whatever it is given.  Nothing
			 * else in the suite announces this name, so the line satisfying this
			 * half can only be the one this half provoked.
			 */
			/*
			 * No half-one value is carried in: bound and armed are both
			 * reassigned below before anything reads them, observed_name is only
			 * read inside the branch that reassigns it first, and the parse
			 * status is block-local to each half - so resetting anything here
			 * would be a dead store rather than a safeguard.
			 */
			bound = test_opal_bind_config();
			fst_xcheck(bound == SWITCH_STATUS_SUCCESS,
					   "the opal.conf provider must be registered before the named-listener document is parsed");

			armed = test_opal_log_capture_start(TEST_OPAL_LISTEN_NAME);
			fst_xcheck(armed == SWITCH_STATUS_SUCCESS,
					   "the listener capture must be re-armed, and its queue barrier open, before the document is parsed");

			if (bound == SWITCH_STATUS_SUCCESS && armed == SWITCH_STATUS_SUCCESS) {
				FSManager manager;
				switch_status_t status = manager.ReadConfig(false);

				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					observed_name = test_opal_log_wait(TEST_OPAL_LOG_TIMEOUT_MS);
					fst_check(observed_name != NULL);

					if (observed_name) {
						fst_check_string_equals(observed_name, TEST_OPAL_LISTEN_NAME);
					}

					fst_xcheck(test_opal_log_foreign_listener_name() == NULL,
							   "no listener other than the awaited one may be announced inside the capture window");
				}
			} else {
				fst_fail("the named-listener contrast was not attempted because a precondition failed");
			}

			/* Single unconditional tail, as above. */
			if (armed == SWITCH_STATUS_SUCCESS) {
				fst_check(test_opal_log_capture_stop() == SWITCH_STATUS_SUCCESS);
			}

			if (bound == SWITCH_STATUS_SUCCESS) {
				fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/*
		 * Case 7 - module load and endpoint-interface registration.
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
		 * NOTHING THAT THIS CASE ACQUIRES IS ACQUIRED INSIDE A FATAL ASSERTION.
		 * The case registers a provider and creates the pool the loaded module
		 * lives in, and it is the only case that deliberately leaves state behind
		 * for a later one, so an exit that skipped its tail would orphan a binding
		 * into every subsequent case and leak a pool for the rest of the process.
		 * Both acquisitions are therefore plain statements whose status is checked
		 * non-fatally, the work that depends on them is gated on both having
		 * succeeded, and the case ends with a single unconditional tail.  The only
		 * fatal check is the process precondition, which precedes everything:
		 * without a live PProcess the FSManager constructor terminates the binary.
		 *
		 * The load is gated on the provider specifically, not merely for tidiness:
		 * FSManager::Initialise() discards ReadConfig()'s status, so loading with
		 * no provider registered would still report success while quietly falling
		 * through to StartListener("") - a wildcard bind on the default H.323 port
		 * that this suite exists never to perform.
		 */
		FST_TEST_BEGIN(module_load_and_endpoint_interface)
		{
			switch_loadable_module_interface_t *observed_interface = NULL;
			switch_loadable_module_interface_t *loaded_interface = NULL;
			switch_endpoint_interface_t *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			switch_status_t bound = SWITCH_STATUS_FALSE;
			switch_status_t pooled = SWITCH_STATUS_FALSE;

			fst_requires(test_opal_acquire_process() != NULL);
			/* Fatal precondition: the IAX2 wildcard listener must be unable to
			 * bind before a manager is constructed.  Asserted per case, not just
			 * once, so no re-ordering can leave a manager built without it. */
			fst_requires(test_opal_iax2_containment_in_effect());

			bound = test_opal_bind_config();
			fst_xcheck(bound == SWITCH_STATUS_SUCCESS,
					   "the opal.conf provider must be registered before the module reads its configuration");

			observed_interface = switch_loadable_module_create_module_interface(fst_pool, MODNAME);
			fst_check(observed_interface != NULL);

			if (bound == SWITCH_STATUS_SUCCESS && observed_interface) {
				FSManager manager;

				fst_check(manager.Initialise(observed_interface));

				endpoint = manager.GetSwitchInterface();
				fst_check(endpoint != NULL);

				if (endpoint) {
					fst_check_string_equals(endpoint->interface_name, TEST_OPAL_INTERFACE_NAME);
				}
			} else {
				fst_fail("the in-suite manager was not initialised because a precondition failed");
			}

			/* Hand the PTLib process singleton over to the module. */
			test_opal_release_process();

			/* The module is loaded against the module-lifetime pool, never against
			 * fst_pool: this case leaves the module loaded on purpose so that the
			 * shutdown case can assert its status, and fst_pool does not survive
			 * this case's teardown.  See test_opal_module_pool above. */
			pooled = test_opal_module_pool_create();
			fst_xcheck(pooled == SWITCH_STATUS_SUCCESS,
					   "the module-lifetime pool must be available before the module is loaded");
			fst_check(test_opal_module_pool != NULL);
			fst_check(test_opal_module_pool != fst_pool);

			if (bound == SWITCH_STATUS_SUCCESS && test_opal_module_pool != NULL) {
				status = mod_opal_load(&loaded_interface, test_opal_module_pool);
			} else {
				fst_fail("the module load was not attempted because a precondition failed");
			}

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

			/*
			 * SINGLE UNCONDITIONAL CLEANUP TAIL.
			 *
			 * The provider is unregistered on every path that registered it, so
			 * nothing this case bound can answer a lookup in any later case - an
			 * invariant the next case re-checks on entry rather than assuming.
			 *
			 * The retained state is handed on ONLY when the load actually took
			 * ownership of it.  When the load did not happen, or happened and
			 * failed, there is no shutdown case's worth of state to observe and
			 * keeping it would leak: mod_opal_load() deletes its own process on the
			 * Initialise failure path (mod_opal.cpp:128-129) but nothing else
			 * reclaims the pool, the suite's process or a half-built interface, so
			 * the sweep does it here and now.
			 */
			if (bound == SWITCH_STATUS_SUCCESS) {
				fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
			}

			if (status != SWITCH_STATUS_SUCCESS) {
				test_opal_suite_state_cleanup();
			}
		}
		FST_TEST_END()

		/*
		 * Case 8 - module shutdown.
		 *
		 * The module's shutdown entry point destroys the process object its
		 * load created and is unconditionally successful: deleting a null
		 * pointer is a no-op, so the call is safe even where no load ran.  It
		 * is declared last so it observes a fully initialised module, and
		 * because the suite is built on FST_SUITE_BEGIN there is no implicit
		 * unload at suite end to make the result unobservable.
		 *
		 * BEING DECLARED LAST GIVES THIS CASE TWO EXTRA DUTIES.
		 *
		 * It opens by proving that the case before it left no configuration
		 * provider registered.  The framework re-enters the fixture suite once per
		 * declared case and a fatal check only breaks the body it appears in, so
		 * this case always runs even when an earlier one exited early - which makes
		 * it the right place to state that invariant, and the only place a
		 * regression that orphaned a binding would be caught by name rather than
		 * silently answering some later lookup.
		 *
		 * And it closes with the suite's sweep, unconditionally.  Every
		 * precondition here is therefore non-fatal with a guarded dereference: the
		 * point of the sweep is that state gets reclaimed even when the state was
		 * not what this case hoped to find, and a fatal precondition would skip the
		 * very cleanup it was checking for.
		 */
		FST_TEST_BEGIN(module_shutdown_succeeds)
		{
			switch_status_t status = SWITCH_STATUS_FALSE;

			/*
			 * Permanent regression guard.  SWITCH_STATUS_FALSE is the expected
			 * answer, and it is a positive statement rather than an absence of one:
			 * the unbind searches the binding list under the write lock and reports
			 * FALSE precisely when the pointer is not registered, changing nothing.
			 * A SUCCESS here would mean the previous case left its provider behind.
			 */
			fst_xcheck(test_opal_unbind_config() == SWITCH_STATUS_FALSE,
					   "the preceding case must leave no opal.conf provider registered");

			/*
			 * Observe the loaded module BEFORE the call, so the assertions after it
			 * measure a transition rather than restating a fact.  A shutdown that
			 * did nothing would leave every one of these unchanged and fail below.
			 */
			fst_check(test_opal_module_pool != NULL);
			fst_check(test_opal_module_pool != fst_pool);

			fst_check(test_opal_module_interface != NULL);

			if (test_opal_module_interface) {
				fst_check(test_opal_module_interface->endpoint_interface != NULL);

				if (test_opal_module_interface->endpoint_interface) {
					fst_check_string_equals(test_opal_module_interface->endpoint_interface->interface_name,
											TEST_OPAL_INTERFACE_NAME);
				}
			}

			fst_check(PProcess::IsInitialised());
			fst_check(test_opal_module_manager() != NULL);

			status = mod_opal_shutdown();
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*
			 * mod_opal.cpp:136-137 deletes the process object, and ~FSProcess
			 * deletes the manager with it (mod_opal.cpp:246).  Deleting the one live
			 * PProcess-derived object is what makes PProcess::IsInitialised() false
			 * again, so these two public observations prove both the process and the
			 * manager it owned are gone.
			 */
			fst_check(!PProcess::IsInitialised());
			fst_check(test_opal_module_manager() == NULL);

			/* the module interface itself is pool-backed, not process-backed, so it
			 * is still readable after shutdown - the sweep below is what releases it */
			if (test_opal_module_interface && test_opal_module_interface->endpoint_interface) {
				fst_check_string_equals(test_opal_module_interface->endpoint_interface->interface_name,
										TEST_OPAL_INTERFACE_NAME);
			}

			/* shutdown is idempotent: deleting a null pointer is a no-op */
			status = mod_opal_shutdown();
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(!PProcess::IsInitialised());

			/*
			 * THE SUITE'S UNCONDITIONAL SWEEP.
			 *
			 * Nothing above it is fatal, so this runs on every path through the
			 * case, and because it is the last declared case it runs even when an
			 * earlier case exited early.  It reclaims everything that can outlive a
			 * single case - the provider, the log capture, the module's process, the
			 * suite's own process and the module-lifetime pool - in the one order
			 * that is safe, and each step is idempotent, so repeating work the case
			 * has already done above costs nothing.  Shutdown was the last thing to
			 * touch pool-backed module state, which is why the pool goes last.
			 *
			 * The three checks that follow are the positive statement that the suite
			 * exits owning nothing: no PTLib process, no module pool, and - already
			 * established on entry - no registered provider.
			 */
			test_opal_suite_state_cleanup();

			fst_check(test_opal_process == NULL);
			fst_check(test_opal_module_pool == NULL);
			fst_check(!PProcess::IsInitialised());
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
