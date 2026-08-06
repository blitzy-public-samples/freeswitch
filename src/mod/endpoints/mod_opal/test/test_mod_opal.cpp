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
 * Blitzy Agent <agent@blitzy.com>
 *
 * mod_opal_test -- mod_opal tests
 *
 */

#include <switch.h>
#include <test/switch_test.h>
#include "../mod_opal.h"

/*
 * Process isolation primitives, for the configuration-absent case only.  fork(),
 * execve() and alarm() come from <unistd.h>, waitpid() and the WIF* decoders from
 * <sys/wait.h>, and kill()/SIGKILL from <signal.h>.  Nothing else in this suite
 * spawns a process.
 */
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/*
 * open() and O_WRONLY, for the /dev/null descriptor the PARENT opens so that the
 * helper's console output can be redirected with nothing but dup2() inside the
 * async-signal-safe region after the fork.  Without that redirection the helper
 * runs the same FCTX driver as this process and its case line is duplicated into
 * the output this run is judged by.
 */
#include <fcntl.h>

/*
 * fstat() and S_ISFIFO(), and poll().  Both belong to the helper-provenance
 * handshake described at test_opal_helper_provenance_ok() below: before the helper
 * believes a descriptor number handed to it in the environment, it establishes
 * that the descriptor is actually a pipe (fstat + S_ISFIFO) and then reads the
 * parent's one-time record from it under a bounded wait (poll), so that no
 * descriptor that merely happens to be open can wedge the helper.
 */
#include <sys/stat.h>
#include <poll.h>

/*
 * ---------------------------------------------------------------------------
 * Suite design notes
 * ---------------------------------------------------------------------------
 *
 * This harness includes only "../mod_opal.h" and reaches its subjects through
 * legitimate C++ access.  Nothing is de-staticised, no symbol is re-exported, no
 * `friend' declaration is added, mod_opal.h is unmodified, and the only change to
 * mod_opal.cpp is the OOS-9 co-load guard, which this suite asserts rather than
 * introduces.
 *
 * HOW TO READ THE mod_opal.cpp LINE NUMBERS BELOW.  Every `mod_opal.cpp:<line>' anchor
 * in this file numbers the module as it stood BEFORE that guard was added, and the guard
 * shifted the file by +9 to +97 depending on where in it you are.  The anchors were
 * deliberately left on the pre-guard numbering rather than mass-rewritten, so translate
 * them through the piecewise mapping tabulated in
 * blitzy/documentation/oos9-coload-determination.md, "Reading the line numbers".  The
 * two most-cited: mod_opal.cpp:104, the load function's entry log line, is :182 in the
 * guarded tree, and mod_opal.cpp:116, where the FSProcess is constructed, is :199.
 *
 * Link shape.  Because the suite includes the header alone, the module's translation
 * unit reaches the link through the libmodopal.la convenience library that
 * src/mod/endpoints/mod_opal/Makefile.am declares, following the tree's C++
 * module-test precedent (src/mod/codecs/mod_openh264/Makefile.am).  That target
 * carries the production module's own compile flags, because a binary built with
 * different flags is not testing the same code.
 *
 * Entry points.  The module load and shutdown macros expand to plain definitions with
 * no storage class, so mod_opal_load() and mod_opal_shutdown() have external linkage
 * and are called directly here, which makes their switch_status_t results genuine
 * assertions rather than the unasserted load and unload a module-scoped bootstrap
 * performs.  mod_opal.cpp wraps them in SWITCH_BEGIN_EXTERN_C, so the declarations
 * below must carry C linkage or this C++ unit would not resolve them.  The
 * module-interface definition macro is deliberately not invoked: the module's own
 * translation unit already emits mod_opal_module_interface.
 *
 * Configuration injection.  switch_xml_open_cfg() resolves "opal.conf" through
 * switch_xml_locate(), which consults the registered XML search bindings before the
 * static configuration root, and a binding cannot suppress a static-root hit.  The
 * failure outcome is therefore produced by the fixture root
 * (test/conf_opal/freeswitch.xml) deliberately carrying no
 * <configuration name="opal.conf"> child: with no binding registered both the binding
 * list and the static root miss, switch_xml_open_cfg() returns NULL and
 * FSManager::ReadConfig() takes its error branch.  The success outcome comes from a
 * binding, registered for the duration of the case, that returns a freshly parsed copy
 * of TEST_OPAL_CONFIG_XML below.
 *
 * PTLib process singleton.  OpalManager's constructor reads PProcess::Current(), which
 * terminates the process outright when no PProcess-derived object exists.  Every
 * FSManager therefore needs a live PProcess while PTLib permits at most one, so the
 * suite keeps a single suite-local FSProcess for the cases that build managers
 * directly and releases it before mod_opal_load() creates its own.
 *
 * Determinism.  No case dlopens or dlcloses a module, talks to a third party, opens a
 * random port or depends on the wall clock.
 *
 * Threads.  mod_opal declares no FreeSWITCH runtime entry point - the module
 * definition macro takes (name, load, shutdown, runtime) (switch_types.h:2650) and
 * mod_opal.cpp:100 passes NULL for runtime - so no case here can start a module
 * runtime thread.  The OPAL toolkit does start threads of its own: IAX2EndPoint's
 * constructor calls Initialise(), which on a successful wildcard listen starts an
 * IAX2Transmit and an IAX2Receiver thread, and FSManager::Initialise() hands every
 * configured listener to H323EndPoint::StartListener() (mod_opal.cpp:284-292) where an
 * OpalListener owns a PThread for as long as it is open (opal/transports.h:491).
 * Case 7 is the only case that reaches Initialise(), and it does so twice - a scoped
 * local FSManager, then mod_opal_load() - so it accounts for up to two listener
 * threads over its lifetime, one at a time, both on 127.0.0.1:21720 TCP.
 *
 * Those listener threads are reclaimed by ownership.  The scoped manager is destroyed
 * at the end of its own block, before mod_opal_load() runs, so its listener closes
 * before the module's own opens on the same address.  The module's listener is left
 * alive on purpose, so that case 8 has a loaded module to assert a shutdown status
 * against, and goes away when mod_opal_shutdown() deletes opal_process: FSProcess's
 * destructor deletes the manager (mod_opal.cpp:244-246), ~OpalManager() deletes every
 * endpoint still attached after ShutDown() has been called on each
 * (opal/manager.h:163-167, opal/endpoint.h:92-96) - the ownership mod_opal itself
 * relies on (mod_opal.cpp:266) - and closing an OpalListener joins its thread
 * (opal/transports.h:469-474).  The sweep calls mod_opal_shutdown() unconditionally,
 * so that reclamation does not depend on case 8 reaching its assertions.
 *
 * Socket footprint.  Every socket this suite is responsible for is bound to loopback:
 *
 *   127.0.0.1:21720 TCP - the H.323 call-signalling listener the injected
 *             configuration declares, held only while the FSManager that read that
 *             configuration lives.
 *
 *   127.0.0.1:4569 UDP - the suite's own guard on the IAX2 default port, which exists
 *             to make a socket NOT happen.  FSManager's constructor allocates an
 *             IAX2EndPoint unconditionally (mod_opal.cpp:262-270) and that constructor
 *             calls Initialise(), which listens on the WILDCARD address on this port
 *             and, on success, starts the two IAX2 threads.  Nothing mod_opal reads
 *             narrows the address and the harness must not change the module, so the
 *             suite takes the port on loopback itself before any manager exists; a
 *             loopback holder is enough to refuse a wildcard bind.
 *
 *             The guard is not one socket held from the first case to the last: the
 *             sweep releases it, deliberately last so that it outlives every manager
 *             the suite built, and the setup hook re-acquires it ahead of the next case
 *             body.  It is in force whenever a case body runs, which is the only window
 *             in which a manager can exist.  Only a socket the constructing process
 *             holds ITSELF counts as containment, and the one place ownership moves is
 *             case 1's isolated helper.  The measured bind semantics and the full
 *             ownership argument are recorded at the containment helpers below.
 *
 *             The containment is observable: the IAX2 endpoint stays UNINITIALISED for
 *             the whole of its manager's life, and case 2 asserts that from inside the
 *             process and from outside it.  OPAL reports the refused listen through
 *             PTRACE and propagates no status, so FSManager construction still runs to
 *             completion and the endpoint is still allocated and attached - case 3
 *             finds it there.
 *
 *   0.0.0.0:4569 UDP - what the containment prevents, and what must never appear.  An
 *             IAX2 listener on the wildcard address would be reachable from off-box for
 *             as long as the process lived (CWE-668).  No case constructs an FSManager
 *             without first requiring that the port is already unavailable to a wildcard
 *             bind, so a run in which containment could not be established ends as
 *             SKIPPED - the Automake skip status, 77 - rather than exposing the socket.
 *
 * What the listener cases assert.  FSManager::Initialise() reports a StartListener()
 * failure only through PTRACE and propagates no status (mod_opal.cpp:287-291), so a
 * listener that cannot bind leaves neither a FreeSWITCH log line nor a failed return
 * behind.  The cases below therefore observe the listener *configuration* that reached
 * ReadConfig() - its name, address and port - and not a socket proven to be bound.
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
 * A third configuration document whose single <listener> element carries no name
 * attribute, which drives FSManager::ReadConfig() down its listener-name default
 * branch (mod_opal.cpp:418-419): switch_xml_attr_soft() yields "" for an absent
 * attribute, PString::IsEmpty() is therefore true, and the name becomes "unnamed".
 *
 * Identical to the default document in every other respect, so the missing attribute
 * is the only difference an assertion can be responding to.  The port is declared
 * explicitly rather than left to FSListener's constructor default, which case 4
 * already covers.
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
 * FSManager::m_listeners is private and no accessor exposes it or any element of it;
 * FSManager's public surface is GetSwitchInterface / GetContext / GetDialPlan /
 * GetCodecPrefs / GetDisableTranscoding only.  Constructing an FSListener directly, as
 * case 4 does, cannot reach the default either: "unnamed" is applied by ReadConfig(),
 * not by the constructor, which initialises only m_port.
 *
 * The one thing ReadConfig() makes publicly observable about a listener it stored is
 * the SWITCH_LOG_DEBUG line it emits for each one at mod_opal.cpp:431, carrying
 * listener.m_name.  Capturing that through switch_log_bind_logger() is therefore the
 * only legitimate seam, and it needs no private access, no friend declaration, no
 * de-staticising and no production edit.  The technique is sanctioned in-tree:
 * tests/unit/switch_log.c binds a logger and waits for a matching line under a mutex
 * and condition variable, which is the shape reproduced below.
 *
 * DELIVERY IS ASYNCHRONOUS, and the consequence is a cross-case hazard rather than a
 * mere need to wait.  switch_log_printf() only enqueues the node
 * (switch_queue_trypush(LOG_QUEUE, node), src/switch_log.c:732); bound loggers are
 * invoked later from log_thread(), which pops the queue and calls each binding whose
 * level admits the node (src/switch_log.c:501-518).  Reading the captured value
 * straight after ReadConfig() returned would race that thread, and a listener line
 * produced by an earlier case can still be sitting in LOG_QUEUE when this case binds
 * its logger and would then be handed to it as though this case had produced it.
 *
 * Two independent mechanisms make the observation exact, and neither consults the
 * clock:
 *
 *   1. A QUEUE BARRIER.  Arming binds the logger, emits a sentinel line carrying a
 *      token unique to that arming, and refuses any listener line until the logger has
 *      seen that sentinel come back.  LOG_QUEUE is one FIFO (src/switch_log.c:61,
 *      created at :755) drained by exactly one consumer (log_thread's blocking
 *      switch_queue_pop at :501), so once the sentinel has been dispatched every node
 *      enqueued before it necessarily has been too.  Anything pending from an earlier
 *      case therefore arrives strictly before the barrier opens and is discarded, and
 *      everything this case produces is enqueued strictly after the sentinel and is
 *      admitted.  That is a proof about queue order rather than an estimate.
 *
 *   2. AN EXPECTED-NAME FILTER.  Arming also states the listener name the case is about
 *      to provoke, and only that name satisfies the wait.  A post-barrier line naming
 *      anything else is recorded separately as foreign, so the case can assert that no
 *      other listener line appeared inside its window.  Together with the deliberately
 *      distinct name the settings case announces (TEST_OPAL_SETTINGS_LISTEN_NAME), a
 *      leaked line can neither be mistaken for this case's line nor vacuously satisfy
 *      it.
 *
 * The binding level is SWITCH_LOG_DEBUG because the dispatch test is
 * "binding->level >= node->level" and DEBUG is the highest-numbered real level, so a
 * lower binding level would filter this line out.  Binding at DEBUG also raises the
 * core's MAX_LEVEL (src/switch_log.c:461-463), which gates whether a node is enqueued
 * for bound loggers at all (src/switch_log.c:706); and FST_TEST_BEGIN independently
 * raises the core log level to SWITCH_LOG_DEBUG for the duration of each case.  All
 * three conditions therefore hold by construction.
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

/*
 * The closing barrier's prefix.  Distinct from the opening prefix, and sharing no
 * substring with it or with the listener marker, so that a surplus copy of one
 * sentinel can never open the other's barrier.  That surplus is real rather than
 * hypothetical: the wait loop re-emits its sentinel once per slice, so extra
 * copies of the opening sentinel may still be in the queue when the closing
 * barrier begins.
 */
#define TEST_OPAL_LOG_CLOSE_PREFIX "mod-opal-test-log-drained-"

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
/*
 * The CLOSING barrier's own sentinel and flag, deliberately separate from the
 * opening barrier's pair above.  They cannot be shared: until
 * test_opal_log_barrier_seen is set, the logger below discards every node it is
 * handed, which is exactly the behaviour an opening barrier needs and exactly the
 * behaviour a closing barrier must not have.  Re-arming the opening flag to close
 * a window would therefore throw away the very evidence the window exists to
 * collect.
 */
static char test_opal_log_close_sentinel[64];
static int test_opal_log_close_seen = 0;
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

	/*
	 * The CLOSING barrier, recognised here rather than in the arming gate above
	 * precisely because this point is past that gate: a node reaching this line
	 * is already inside the observation window, so noticing the closing sentinel
	 * here reports that the queue has drained past it WITHOUT discarding
	 * anything.  The gate above must discard to do its job; this must not.
	 *
	 * The empty-buffer test keeps the search inert until a closing barrier has
	 * actually been armed, so an arming that never closes one costs nothing.
	 */
	if (!test_opal_log_close_seen && test_opal_log_close_sentinel[0]
		&& strstr(node->content, test_opal_log_close_sentinel)) {
		test_opal_log_close_seen = 1;
		switch_thread_cond_broadcast(test_opal_log_cond);
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
static void test_opal_log_emit_sentinel(const char *sentinel)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s\n", sentinel);
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
static switch_status_t test_opal_log_await_sentinel(const char *sentinel, int *seen_flag)
{
	switch_time_t expiration = switch_time_now() + (TEST_OPAL_LOG_BARRIER_TIMEOUT_MS * 1000);
	switch_time_t now = 0;
	int seen = 0;

	if (!test_opal_log_mutex || !test_opal_log_cond || !sentinel || !*sentinel || !seen_flag) {
		return SWITCH_STATUS_FALSE;
	}

	while (!seen && (now = switch_time_now()) < expiration) {
		switch_time_t slice = now + (TEST_OPAL_LOG_BARRIER_SLICE_MS * 1000);

		if (slice > expiration) {
			slice = expiration;
		}

		test_opal_log_emit_sentinel(sentinel);

		switch_mutex_lock(test_opal_log_mutex);

		while (!*seen_flag && (now = switch_time_now()) < slice) {
			switch_thread_cond_timedwait(test_opal_log_cond, test_opal_log_mutex, slice - now);
		}

		seen = *seen_flag;

		switch_mutex_unlock(test_opal_log_mutex);
	}

	return seen ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static switch_status_t test_opal_log_barrier(void)
{
	return test_opal_log_await_sentinel(test_opal_log_sentinel, &test_opal_log_barrier_seen);
}

/*
 * The CLOSING barrier: everything the action queued has been dispatched.
 *
 * Waiting for the awaited listener line proves that line arrived and nothing about
 * lines queued after it.  The log queue has a single consumer thread, so it is strictly
 * FIFO: once a sentinel enqueued after the action has been handed to this logger, every
 * node the action enqueued before it has been handed over too.  Only then is "no OTHER
 * listener was announced" decidable - read any earlier and a foreign line still sitting
 * in the queue would be silently missed, which is an observation race rather than a
 * genuine absence (CWE-362).
 *
 * A fresh sentinel is minted per close, under the mutex the logger holds while reading
 * it, so the write is synchronised against that read.  Minting rather than reusing
 * matters because the wait loop re-emits once per slice, so a surplus copy of an earlier
 * sentinel may still be queued and reuse would let the barrier open on a stale node that
 * predates the action.  The epoch counter is shared with the opening barrier so no two
 * sentinels of either kind can collide.
 */
static switch_status_t test_opal_log_barrier_close(void)
{
	if (!test_opal_log_mutex || !test_opal_log_cond) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(test_opal_log_mutex);
	test_opal_log_close_seen = 0;
	switch_snprintf(test_opal_log_close_sentinel, sizeof(test_opal_log_close_sentinel), "%s%u",
					TEST_OPAL_LOG_CLOSE_PREFIX, ++test_opal_log_epoch);
	switch_mutex_unlock(test_opal_log_mutex);

	return test_opal_log_await_sentinel(test_opal_log_close_sentinel, &test_opal_log_close_seen);
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
	test_opal_log_close_sentinel[0] = '\0';
	test_opal_log_close_seen = 0;
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
 * Two side effects have to be contained before the first constructor this suite calls
 * runs, because mod_opal contains neither of them for the cases that build objects
 * directly: PTLib's plugin search path, and OPAL's IAX2 listener.
 *
 *
 * (1) PLUGIN SEARCH PATH
 *
 * Constructing any PProcess makes PTLib enumerate its plugin directory and dlopen what
 * it finds there.  The directory comes from the environment: PTLIBPLUGINDIR is the
 * current name and PWLIBPLUGINDIR the legacy one, and both strings are present in the
 * libpt this module links against, so both are live inputs.  A test binary is run by
 * `make check' out of an environment nobody audits, which turns an inherited value into
 * an arbitrary-code-execution seam (CWE-427, CWE-829).
 *
 * mod_opal sets PTLIBPLUGINDIR to "/no/thanks" inside mod_opal_load()
 * (mod_opal.cpp:108), which runs in exactly one case near the end of this suite, and it
 * never touches the legacy name.  Both variables are therefore pinned here,
 * unconditionally and before the first PProcess, to a fixed directory that does not
 * exist, since nothing can be enumerated in a directory that is not there.
 * "/no/thanks" is deliberately the value the production module uses, so harness and
 * module agree.
 *
 * setenv() rather than putenv(): setenv() copies both name and value into storage the C
 * library owns, whereas putenv() retains the caller's buffer, which is what makes the
 * common `putenv((char *) "NAME=value")' idiom a const-correctness violation - it casts
 * away const from a string literal and hands the result to an interface entitled to
 * write through it, so any later write is undefined behaviour (CWE-758).  The overwrite
 * flag is 1 because this must replace an inherited value, not supply a default for an
 * unset one.
 *
 * It is wired in two places on purpose - the suite setup hook, which FCTX runs before
 * every case body, and the acquire helper below, the only place in the suite that
 * constructs a PProcess - so that no re-ordering and no new case can reintroduce the
 * exposure.  It is idempotent, so paying twice costs nothing.
 *
 * Nothing about the pin is cached.  setenv() can fail, returning non-zero on an
 * allocation failure, so a helper that assumed success and remembered it would report
 * containment that does not exist while the process came up against an unaudited search
 * path.  The pin is re-applied and re-verified by readback on every call, and the
 * verdict is derived from the environment as it is at that instant rather than from a
 * flag.
 */
#define TEST_OPAL_PLUGIN_DIR "/no/thanks"

/*
 * True when both plugin-directory variables read back as the pinned value.  This
 * is the only definition of "pinned" in this file: it interrogates the
 * environment and believes nothing else.
 */
static int test_opal_plugin_path_is_pinned(void)
{
	const char *ptlib = getenv("PTLIBPLUGINDIR");
	const char *pwlib = getenv("PWLIBPLUGINDIR");

	return ptlib && pwlib && !strcmp(ptlib, TEST_OPAL_PLUGIN_DIR) && !strcmp(pwlib, TEST_OPAL_PLUGIN_DIR);
}

/*
 * Pin both variables and return whether the pin is VERIFIED in place: 1 only
 * when both setenv() calls reported success AND both variables read back as the
 * pinned directory, 0 otherwise.  A caller that ignores the result gets no
 * guarantee, and a caller that honours it fails closed.
 */
static int test_opal_pin_plugin_path(void)
{
	if (setenv("PTLIBPLUGINDIR", TEST_OPAL_PLUGIN_DIR, 1) != 0) {
		return 0;
	}

	if (setenv("PWLIBPLUGINDIR", TEST_OPAL_PLUGIN_DIR, 1) != 0) {
		return 0;
	}

	return test_opal_plugin_path_is_pinned();
}

/*
 * (2) THE IAX2 WILDCARD LISTENER
 *
 * FSManager's constructor allocates an IAX2EndPoint unconditionally
 * (mod_opal.cpp:262-270).  IAX2EndPoint's own constructor calls its Initialise(),
 * which does `sock = new PUDPSocket(GetDefaultSignalPort())' followed by
 * `sock->Listen(INADDR_ANY, 0, sock->GetPort())' and, ONLY if that listen
 * succeeds, constructs the IAX2Transmit and IAX2Receiver threads.  So merely
 * constructing a manager opens UDP 4569 on the WILDCARD address - reachable from
 * off-box for as long as the process lives - and starts two live threads, unless
 * the port is already unavailable when the constructor runs.  No configuration
 * parameter mod_opal reads narrows the address, and the harness must not change
 * the module, so it cannot be fixed at its source.
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
 * True for a bind status that means "already in use".
 *
 * switch_socket_bind() is a thin wrapper over fspr_socket_bind()
 * (src/switch_apr.c:744) and the Unix implementation returns the platform's own error
 * code unchanged - `if (bind(...) == -1) return errno;'
 * (libs/apr/network_io/unix/sockets.c:160).  fspr defines canonical predicates for a
 * handful of codes but none for address-in-use, so the comparison is made against the
 * platform's EADDRINUSE directly; <errno.h> arrives through switch.h and the cast is
 * needed only because switch_status_t is an enum.
 *
 * Diagnosis only.  A port some other holder already owns is not a substitute for
 * holding the guard - see test_opal_iax2_containment_in_effect() - so this predicate
 * decides which of two error messages explains a refusal, never whether the refusal is
 * tolerable.
 */
static int test_opal_status_is_addr_in_use(switch_status_t status)
{
	return (int) status == EADDRINUSE;
}

/*
 * Take UDP DefaultUdpPort on loopback.  Idempotent: reports success when the
 * guard is already held.  Modelled on test_port() in
 * src/switch_core_port_allocator.c, which is the tree's own way of asking
 * whether a port can be bound.
 */
static switch_status_t test_opal_iax2_guard_acquire(void)
{
	switch_sockaddr_t *guard_addr = NULL;
	switch_status_t bound = SWITCH_STATUS_FALSE;

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

	bound = switch_socket_bind(test_opal_iax2_guard, guard_addr);

	if (bound != SWITCH_STATUS_SUCCESS) {
		switch_socket_close(test_opal_iax2_guard);
		test_opal_iax2_guard = NULL;

		/*
		 * Every refusal is a containment failure, including address-in-use; the two are
		 * separated only so the log says which one happened.
		 *
		 * Address-in-use proves the port is taken right now, and a wildcard bind would be
		 * refused for as long as that lasts - but that is not a property this suite
		 * controls.  The holder is another process, free to close its socket at any
		 * instant, including between this refusal and an FSManager being constructed a few
		 * statements later, after which IAX2EndPoint::Initialise() binds the WILDCARD
		 * address and starts its transmitter and receiver while the case that checked
		 * carries on believing it cannot.  That is the check-then-use gap this guard
		 * exists to close, so only a socket THIS process holds counts.
		 */
		if (test_opal_status_is_addr_in_use(bound)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "IAX2 containment guard could not take %s:%d because another holder already owns it; containment "
							  "cannot be established by this suite, so no FSManager may be constructed. Release the port and "
							  "run the suite again.\n", TEST_OPAL_GUARD_ADDRESS, (int) IAX2EndPoint::DefaultUdpPort);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "IAX2 containment guard could not take %s:%d and the refusal was not address-in-use (status=%d)\n",
							  TEST_OPAL_GUARD_ADDRESS, (int) IAX2EndPoint::DefaultUdpPort, (int) bound);
		}

		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

/*
 * Release the guard and its pool.  Idempotent.
 *
 * Nothing about the port is remembered across a release, because after this
 * returns there is nothing to remember: containment is the socket, so giving up
 * the socket gives up the containment.  The next acquire establishes it again
 * from scratch or fails.
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
 * True when THIS PROCESS holds the IAX2 default port, and therefore when a wildcard bind
 * of that port is guaranteed to be refused for as long as the caller keeps running.
 *
 * A live check of owned state, which is the whole of the contract: a guard already held
 * answers immediately, otherwise one is taken now and the answer is whether that
 * succeeded.  There is no third answer, because a port held by somebody else stops
 * qualifying the moment that holder closes its socket and nothing informs the caller
 * when it does.  Every case that constructs an FSManager consults this beforehand, through
 * test_opal_require_containment_or_skip() below.
 *
 * The one place ownership legitimately moves is the isolated helper: the parent releases
 * the guard before spawning it, the helper takes it as its own precondition and builds
 * nothing without it, and the parent takes it back after reaping.  No FSManager exists anywhere in that window,
 * so whenever a manager exists the process that built it is the one holding the port.
 */
static int test_opal_iax2_containment_in_effect(void)
{
	if (test_opal_iax2_guard) {
		return 1;
	}

	return test_opal_iax2_guard_acquire() == SWITCH_STATUS_SUCCESS;
}

/*
 * Automake's skip status.  A test binary that ends with this status is recorded as SKIPPED
 * rather than failed - 0 is a pass, 77 a skip, 99 a hard framework error - and 77 is the only
 * non-pass outcome this tree sanctions.  No FST wrapper reaches it: fst_requires() and
 * fst_requires_module() both expand to fct_req() (switch_test.h:149 and :154), which breaks
 * out of the case and records a FAILURE (switch_fct.h:3668-3669), so a skip has to be the
 * process's own exit status.
 */
#define TEST_OPAL_AUTOMAKE_SKIP 77

/*
 * End the run as SKIPPED because a condition this suite cannot arrange for itself is absent.
 * Never returns.
 *
 * The condition it exists for is the IAX2 guard.  Containment is a socket THIS PROCESS holds
 * on the IAX2 default UDP port, so a port already owned by something else cannot be taken
 * however correct the code under test is; on a shared host that is a routine condition, not a
 * defect, and failing the cases for it would report a fault that is not there.  Leaving
 * instead of asserting keeps the two distinguishable: a red case means mod_opal misbehaved, a
 * skip means the host could not be arranged.  It is also the safe direction, because the run
 * stops BEFORE any FSManager is constructed, which is what keeps the wildcard IAX2 listener
 * the containment exists to prevent from ever coming into being.
 *
 * _exit() rather than exit(), for the three reasons the isolated helper uses it as well: no
 * atexit handler runs, no sanitizer at-exit leak report is produced over a core that is still
 * up, and no core teardown is attempted - so the status the harness observes is exactly this
 * one and nothing between here and the kernel can overwrite it.
 *
 * The diagnostic goes to stderr rather than only through switch_log_printf(), because the
 * core's logger is asynchronous and a queued line is not guaranteed to have drained by the
 * time this hands the status back.  stderr is the one stream nothing in this suite redirects.
 * Everything buffered is flushed FIRST so that an already-queued explanation - the guard's own
 * ERROR line, which names the port and the remedy - is not stranded behind this message.
 */
static void test_opal_skip_run(const char *reason)
{
	fflush(NULL);

	fprintf(stderr, "SKIP (exit %d): mod_opal test suite: %s\n", TEST_OPAL_AUTOMAKE_SKIP,
			reason ? reason : "an environment condition this suite cannot arrange");
	fprintf(stderr, "SKIP (exit %d): the suite must hold UDP %s:%d itself for the whole of every case that "
			"constructs an FSManager; release that port and run the suite again.\n",
			TEST_OPAL_AUTOMAKE_SKIP, TEST_OPAL_GUARD_ADDRESS, (int) IAX2EndPoint::DefaultUdpPort);

	fflush(stderr);

	_exit(TEST_OPAL_AUTOMAKE_SKIP);
}

/*
 * Containment or skip - what every case that constructs an FSManager calls before it does so.
 *
 * Returns only when this process holds the IAX2 default port.  It is deliberately not an
 * assertion: the predicate it wraps re-establishes the containment live, so a false answer
 * means the port is owned elsewhere, which is a property of the host rather than of the module
 * under test.  Consulted per case, not once for the suite, so no re-ordering can leave a
 * manager built without it.
 */
static void test_opal_require_containment_or_skip(void)
{
	if (test_opal_iax2_containment_in_effect()) {
		return;
	}

	test_opal_skip_run("the IAX2 default UDP port could not be taken on loopback, so the containment an "
					   "FSManager's unconditional IAX2 listener requires cannot be established");
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
 * Return the suite-local PTLib process, creating it on first use - or NULL when
 * the plugin-search-path containment cannot be verified.  Every FSManager needs
 * a process to exist, because OpalManager's constructor reads
 * PProcess::Current().
 *
 * FAILS CLOSED.  The pin is applied and VERIFIED first, on every call, because
 * constructing a PProcess is what triggers PTLib's plugin enumeration and there
 * is no second chance once it has run.  Returning NULL is what makes an
 * unverifiable pin visible: every caller reaches this helper through
 * fst_requires(), so the case aborts instead of proceeding against an unaudited
 * search path.  The check also gates the already-created path, which does not
 * enumerate anything by itself; that is deliberate, because it keeps the rule
 * "no PProcess is handed out without verified containment" true without a caller
 * having to know which branch it took.
 */
static FSProcess *test_opal_acquire_process(void)
{
	if (!test_opal_pin_plugin_path()) {
		return NULL;
	}

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
 * ---------------------------------------------------------------------------
 * ISOLATING THE CONFIGURATION-ABSENT BRANCH IN ITS OWN PROCESS
 * ---------------------------------------------------------------------------
 *
 * FSManager::ReadConfig() creates an event with switch_event_create() and, on the path
 * where switch_xml_open_cfg() cannot locate the module's configuration, returns without
 * ever destroying it (mod_opal.cpp:347-357).  That is a deterministic leak in production
 * code, which this suite must not modify, so it can neither be fixed at its source nor
 * avoided while still asserting the branch.  Executing it in the principal suite process
 * would fail the address sanitizer the CI unit-test arm always enables (ci.sh:76-79).
 * Running it in a process of its own traverses and asserts the branch while confining the
 * leak to a short-lived process that exits through _exit() and never reaches a leak
 * check; the principal process never constructs the FSManager that would leak.
 *
 * fork() IMMEDIATELY FOLLOWED BY exec()
 * ------------------------------------
 * By the time any case body runs, FST_CORE_BEGIN has brought a multithreaded core up.
 * fork() duplicates only the calling thread, so every mutex the other threads happened
 * to hold is duplicated locked and can never be released in the child - the classic
 * inherited-lock deadlock (CWE-667).  The core allocator, the logging queue and PTLib's
 * factory registries are all lock-protected, so any allocator, logger, XML or PTLib call
 * in such a child can block forever.
 *
 * The child therefore does not do the work.  It reaches execve() immediately, touching
 * nothing but async-signal-safe calls on the way, and the assertions run in the fresh
 * image after that exec has replaced the address space - one thread, its own core, its
 * own allocator and no inherited lock state.  The helper is selected by an environment
 * marker and announces itself by exit code.
 */

/* Exit codes the helper returns.  Distinct values so an unexpected outcome names its own
 * failure mode in the parent's diagnostic rather than merely being "non-zero". */
#define TEST_OPAL_CHILD_OK              0
#define TEST_OPAL_CHILD_UNPINNED        50
#define TEST_OPAL_CHILD_NO_CONTAINMENT  51
#define TEST_OPAL_CHILD_NO_PROCESS      52
#define TEST_OPAL_CHILD_NOT_FALSE       53
#define TEST_OPAL_CHILD_NOT_FALSE_AGAIN 54
#define TEST_OPAL_CHILD_EXEC_FAILED     57
#define TEST_OPAL_CHILD_BAD_PROVENANCE  58

/*
 * The marker is not, by itself, authority to run as the helper.  It is an ordinary
 * environment variable, so it is inherited by anything this binary is run under and
 * survives in any shell that exported it once.  Dispatching the helper body on the
 * marker alone would run exactly one case and exit with that case's status - zero when
 * the case passes - so the run would be recorded as a clean pass with every later case
 * silently never executed.
 *
 * The marker is therefore paired with a second credential that cannot be inherited
 * usefully: a one-time record the parent writes into an anonymous pipe before it forks.
 * The descriptor number and a freshly generated token travel in the child's environment,
 * and the helper insists that the descriptor really is a pipe and that the record on it
 * matches the token exactly.  A token from another run does not match, and a marker with
 * no pipe behind it has nothing to match against.  Anything less than both credentials is
 * refused outright with TEST_OPAL_CHILD_BAD_PROVENANCE.
 */
#define TEST_OPAL_HELPER_ENV            "FST_MOD_OPAL_ISOLATED_HELPER"
#define TEST_OPAL_HELPER_READCONFIG     "readconfig-missing-config"

/* The out-of-band half of the credential: the pipe's descriptor number, the
 * one-time token, and the tag that opens the record so a foreign writer cannot
 * satisfy the check by accident. */
#define TEST_OPAL_HELPER_FD_ENV         "FST_MOD_OPAL_HELPER_FD"
#define TEST_OPAL_HELPER_TOKEN_ENV      "FST_MOD_OPAL_HELPER_TOKEN"
#define TEST_OPAL_HELPER_RECORD_TAG     "fst-mod-opal-helper/1"

/* The three environment names this suite owns.  Every one is dropped from the
 * inherited environment before the exec plan appends its own, so no copy the
 * parent inherited can reach the helper and be mistaken for provenance. */
#define TEST_OPAL_EXEC_OWNED_NAMES      3

/* The handshake read is bounded so a descriptor that is a pipe but carries
 * nothing cannot wedge the helper: the record is already in the pipe buffer
 * before the fork, so this budget is never approached on a real spawn. */
#define TEST_OPAL_HANDSHAKE_MS          2000
#define TEST_OPAL_HANDSHAKE_POLL_MS     20

/* An upper bound on a plausible descriptor number, so a hostile or corrupt value
 * is rejected by arithmetic before it ever reaches fstat(). */
#define TEST_OPAL_HELPER_MAX_FD         (1 << 20)

#define TEST_OPAL_SELF_EXE              "/proc/self/exe"

/*
 * The helper's own watchdog, and the parent's independent bound on it.  The alarm is
 * armed before the exec because a pending alarm survives exec, so it covers the helper's
 * bootstrap as well as its body.  The parent's deadline is deliberately longer, so the
 * self-watchdog normally fires first and the parent reports a signal rather than a
 * timeout, which is the more precise diagnosis.
 */
#define TEST_OPAL_CHILD_ALARM_SECONDS   60
#define TEST_OPAL_CHILD_DEADLINE_MS     90000
#define TEST_OPAL_CHILD_POLL_MS         20

/*
 * True when this process is the helper, decided by an EXACT value match.
 *
 * The value is compared rather than merely detected so that a marker left behind
 * by some other tool, or one naming a mode this build does not implement, cannot
 * silently divert the suite into helper behaviour.
 */
static int test_opal_in_helper_mode(void)
{
	const char *mode = getenv(TEST_OPAL_HELPER_ENV);

	return mode && !strcmp(mode, TEST_OPAL_HELPER_READCONFIG) ? 1 : 0;
}

/*
 * Presence of the marker whatever its value, which is a deliberately different question
 * from the exact-value test above: that one decides whether this process should RUN the
 * helper body, this one decides whether it may SPAWN one.  Recursion is then impossible
 * unless both are wrong at once.
 *
 * Were the value test to govern both, a marker that failed to match - a mistyped value,
 * a diverged mode name, a regression in the comparison - would leave a process that is a
 * helper but does not know it, and it would spawn a helper of its own, which would do
 * the same.  Each generation arms a fresh watchdog, so the recursion would sustain itself
 * instead of expiring at any deadline.
 */
static int test_opal_helper_marker_present(void)
{
	return getenv(TEST_OPAL_HELPER_ENV) != NULL ? 1 : 0;
}

/*
 * The second credential: proof that the marker was written by the parent of this run and
 * not inherited, exported by hand or left behind by another tool.  Each step is load
 * bearing.
 *
 * All three names must be present and non-empty and the mode must match exactly, so a
 * partial or mistyped environment is refused rather than half-believed.
 *
 * The descriptor number is parsed with strtol() and the whole string must be consumed, so
 * "9x" and " 9" are rejected rather than read as 9.  It must also be above standard error
 * and below a plausible ceiling: a handshake over one of the three standard descriptors
 * would be reading the suite's own console, and a wild value has no business reaching
 * fstat().
 *
 * fstat() plus S_ISFIFO() establishes that the number really names a pipe, so a regular
 * file, socket or terminal that merely happens to be open at that number is refused.
 * This path deliberately does NOT close the descriptor: one that is not the pipe this
 * suite created is not this suite's to close.
 *
 * The record itself is then read and compared byte for byte against the tag, the mode and
 * the token rendered in the order the parent renders them.  The read is bounded by poll()
 * so a pipe carrying nothing cannot wedge the helper, and it asks for one byte more than
 * the expected record, so a longer record fails the exact-length test instead of matching
 * on its prefix.  Because the parent closes the write end before forking, end-of-file
 * arrives as soon as the record has been consumed, so the normal case terminates on data
 * rather than on time.
 */
static int test_opal_helper_provenance_ok(void)
{
	const char *mode = getenv(TEST_OPAL_HELPER_ENV);
	const char *fd_text = getenv(TEST_OPAL_HELPER_FD_ENV);
	const char *token = getenv(TEST_OPAL_HELPER_TOKEN_ENV);
	char expected[SWITCH_UUID_FORMATTED_LENGTH + 128] = "";
	char actual[SWITCH_UUID_FORMATTED_LENGTH + 129] = "";
	struct stat descriptor;
	struct pollfd waiter;
	char *parse_end = NULL;
	long parsed = 0;
	int fd = -1;
	int expected_len = 0;
	int capacity = 0;
	int filled = 0;
	int waited_ms = 0;
	int ready = 0;
	int ok = 0;
	ssize_t got = 0;

	if (!mode || !fd_text || !*fd_text || !token || !*token) {
		return 0;
	}

	if (strcmp(mode, TEST_OPAL_HELPER_READCONFIG)) {
		return 0;
	}

	if (strlen(token) > SWITCH_UUID_FORMATTED_LENGTH) {
		return 0;
	}

	parsed = strtol(fd_text, &parse_end, 10);

	if (!parse_end || *parse_end || parsed <= STDERR_FILENO || parsed > TEST_OPAL_HELPER_MAX_FD) {
		return 0;
	}

	fd = (int) parsed;

	memset(&descriptor, 0, sizeof(descriptor));

	/* Not closed on this path on purpose: a descriptor that is not the pipe this
	 * suite created is not this suite's to close. */
	if (fstat(fd, &descriptor) != 0 || !S_ISFIFO(descriptor.st_mode)) {
		return 0;
	}

	expected_len = switch_snprintf(expected, sizeof(expected), "%s %s %s\n", TEST_OPAL_HELPER_RECORD_TAG, mode, token);
	capacity = expected_len + 1;

	if (expected_len <= 0 || capacity > (int) sizeof(actual)) {
		close(fd);
		return 0;
	}

	while (filled < capacity && waited_ms < TEST_OPAL_HANDSHAKE_MS) {
		memset(&waiter, 0, sizeof(waiter));
		waiter.fd = fd;
		waiter.events = POLLIN;

		ready = poll(&waiter, 1, TEST_OPAL_HANDSHAKE_POLL_MS);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		if (ready == 0) {
			waited_ms += TEST_OPAL_HANDSHAKE_POLL_MS;
			continue;
		}

		got = read(fd, actual + filled, (size_t) (capacity - filled));

		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		if (got == 0) {
			/* End of file: the parent closed the write end, so whatever has been
			 * read is the whole record and the length test below judges it. */
			break;
		}

		filled += (int) got;
	}

	ok = (filled == expected_len && !memcmp(actual, expected, (size_t) expected_len)) ? 1 : 0;

	close(fd);

	return ok;
}

/*
 * Render one "NAME=VALUE" entry for the exec plan's environment.
 *
 * Bounded up front rather than relying on the truncation behaviour of the
 * formatter, because a silently truncated credential would be a credential the
 * helper cannot match - a confusing failure in place of a clear refusal.
 */
static char *test_opal_env_entry(const char *name, const char *value)
{
	char rendered[SWITCH_UUID_FORMATTED_LENGTH + 128] = "";

	if (!name || !value) {
		return NULL;
	}

	if (strlen(name) + strlen(value) + 2 > sizeof(rendered)) {
		return NULL;
	}

	switch_snprintf(rendered, sizeof(rendered), "%s=%s", name, value);

	return strdup(rendered);
}

/*
 * Write the whole buffer or report failure, retrying a short write and an EINTR.
 *
 * write() is permitted to transfer less than it was asked for, so a bare call
 * would leave the handshake record truncated on a pipe that is perfectly healthy.
 */
static int test_opal_write_all(int fd, const char *data, switch_size_t len)
{
	switch_size_t sent = 0;
	ssize_t wrote = 0;

	while (sent < len) {
		wrote = write(fd, data + sent, len - sent);

		if (wrote < 0) {
			if (errno == EINTR) {
				continue;
			}

			return 0;
		}

		if (wrote == 0) {
			return 0;
		}

		sent += (switch_size_t) wrote;
	}

	return 1;
}

/*
 * Everything execve() needs, built in the parent so that the forked child can
 * reach exec without touching the allocator.
 *
 * argv[0] deliberately ALIASES path rather than owning a second copy, so the
 * release helper frees the argv ARRAY but never its elements.
 */
typedef struct {
	char *path;
	char **argv;
	char **envp;
} test_opal_exec_plan_t;

static void test_opal_exec_plan_release(test_opal_exec_plan_t * plan)
{
	int i = 0;

	if (!plan) {
		return;
	}

	if (plan->envp) {
		for (i = 0; plan->envp[i]; i++) {
			free(plan->envp[i]);
		}

		free(plan->envp);
		plan->envp = NULL;
	}

	if (plan->argv) {
		free(plan->argv);
		plan->argv = NULL;
	}

	switch_safe_free(plan->path);
}

/*
 * Build the plan.  Returns 1 with every member owned by *plan, or 0 with nothing owned
 * and nothing leaked.
 *
 * The environment is copied entry by entry, dropping all three of the names this suite
 * owns whatever their values, and this run's own triple is appended last.  Dropping all
 * three rather than only the marker matters, because a descriptor number and a token
 * surviving from two different runs are exactly what the handshake exists to rule out.
 */
static int test_opal_exec_plan_build(test_opal_exec_plan_t * plan, const char *argv0, const char *mode, int handshake_fd, const char *token)
{
	extern char **environ;
	static const char *const owned[TEST_OPAL_EXEC_OWNED_NAMES] = {
		TEST_OPAL_HELPER_ENV,
		TEST_OPAL_HELPER_FD_ENV,
		TEST_OPAL_HELPER_TOKEN_ENV
	};
	const char *chosen = NULL;
	char *added[TEST_OPAL_EXEC_OWNED_NAMES] = { NULL, NULL, NULL };
	char fd_text[32] = "";
	switch_size_t name_len = 0;
	switch_size_t count = 0;
	switch_size_t i = 0;
	switch_size_t out = 0;
	int slot = 0;
	int drop = 0;

	if (!plan || !mode || !token || handshake_fd <= STDERR_FILENO) {
		return 0;
	}

	memset(plan, 0, sizeof(*plan));

	if (access(TEST_OPAL_SELF_EXE, X_OK) == 0) {
		chosen = TEST_OPAL_SELF_EXE;
	} else if (argv0 && *argv0 && access(argv0, X_OK) == 0) {
		chosen = argv0;
	} else {
		return 0;
	}

	if (!(plan->path = strdup(chosen))) {
		return 0;
	}

	/* Exactly two slots: the program path and the NULL terminator.  Anything more
	 * would be read by FCTX as a test-name filter. */
	if (!(plan->argv = (char **) calloc(2, sizeof(char *)))) {
		test_opal_exec_plan_release(plan);
		return 0;
	}

	plan->argv[0] = plan->path;
	plan->argv[1] = NULL;

	for (count = 0; environ && environ[count]; count++) {
		;
	}

	/* +TEST_OPAL_EXEC_OWNED_NAMES for the appended triple, +1 for the NULL
	 * terminator.  The drop loop below can only ever shorten the copy, so this is
	 * an upper bound rather than an exact size. */
	if (!(plan->envp = (char **) calloc(count + TEST_OPAL_EXEC_OWNED_NAMES + 1, sizeof(char *)))) {
		test_opal_exec_plan_release(plan);
		return 0;
	}

	switch_snprintf(fd_text, sizeof(fd_text), "%d", handshake_fd);

	added[0] = test_opal_env_entry(TEST_OPAL_HELPER_ENV, mode);
	added[1] = test_opal_env_entry(TEST_OPAL_HELPER_FD_ENV, fd_text);
	added[2] = test_opal_env_entry(TEST_OPAL_HELPER_TOKEN_ENV, token);

	for (slot = 0; slot < TEST_OPAL_EXEC_OWNED_NAMES; slot++) {
		if (!added[slot]) {
			goto fail;
		}
	}

	for (i = 0; i < count; i++) {
		drop = 0;

		for (slot = 0; slot < TEST_OPAL_EXEC_OWNED_NAMES; slot++) {
			name_len = strlen(owned[slot]);

			/* Name match only: compare up to the name and require the very next
			 * byte to be the '=', so FST_MOD_OPAL_HELPER_FD cannot be mistaken for
			 * a longer name that merely starts with it. */
			if (!strncmp(environ[i], owned[slot], name_len) && environ[i][name_len] == '=') {
				drop = 1;
				break;
			}
		}

		if (drop) {
			continue;
		}

		if (!(plan->envp[out] = strdup(environ[i]))) {
			goto fail;
		}

		out++;
	}

	/* Ownership of each appended entry transfers to the plan as it is stored, so
	 * the failure path below cannot double-free one that already landed. */
	for (slot = 0; slot < TEST_OPAL_EXEC_OWNED_NAMES; slot++) {
		plan->envp[out++] = added[slot];
		added[slot] = NULL;
	}

	plan->envp[out] = NULL;

	return 1;

  fail:

	for (slot = 0; slot < TEST_OPAL_EXEC_OWNED_NAMES; slot++) {
		switch_safe_free(added[slot]);
	}

	test_opal_exec_plan_release(plan);

	return 0;
}

/*
 * The whole of the configuration-absent assertion set, evaluated in the HELPER.
 *
 * Runs in a freshly exec'd process with a core of its own, returns the exit code the
 * parent decodes, and touches no state the parent can observe.  The helper is a real
 * bootstrap rather than a duplicated image, so there is no inherited-lock hazard here and
 * no restriction on what this may call.  Each of the three preconditions carries its own
 * exit code, so a precondition failure cannot be mistaken for the substantive assertion
 * failing.
 *
 * The containment precondition is load-bearing here in a way it is not in the parent:
 * this is the process that constructs the manager, so this is the process that must own
 * the IAX2 port.  Its own setup hook has already taken the port - the parent released it
 * before the spawn so that this could succeed - and the check below confirms the socket is
 * held by THIS process rather than inferring an exclusion from somebody else's.  A helper
 * that could not take the port builds nothing and reports
 * TEST_OPAL_CHILD_NO_CONTAINMENT, which the parent turns into the run's skip verdict
 * rather than into a failure of its subject.
 *
 * The branch is driven twice over the same manager, which makes the repeatability of the
 * failure path the property under observation and is the cheapest proof that nothing on
 * that path is consumed on first use.  The two verdicts carry distinct exit codes so the
 * parent's log names which of them diverged.
 *
 * The FSManager is scoped so it is destroyed before the status is judged.  The PProcess is
 * deliberately not released, because the address space is about to be discarded wholesale
 * and PTLib's global teardown would add risk without adding information.  The event
 * production leaks on this branch is likewise left alone - confining it is the entire
 * purpose of this process.
 */
static int test_opal_readconfig_child_body(void)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_status_t repeated = SWITCH_STATUS_SUCCESS;

	if (!test_opal_plugin_path_is_pinned()) {
		return TEST_OPAL_CHILD_UNPINNED;
	}

	if (!test_opal_iax2_containment_in_effect()) {
		return TEST_OPAL_CHILD_NO_CONTAINMENT;
	}

	if (test_opal_acquire_process() == NULL) {
		return TEST_OPAL_CHILD_NO_PROCESS;
	}

	{
		FSManager manager;

		status = manager.ReadConfig(false);

		repeated = manager.ReadConfig(false);
	}

	if (status != SWITCH_STATUS_FALSE) {
		return TEST_OPAL_CHILD_NOT_FALSE;
	}

	return repeated == SWITCH_STATUS_FALSE ? TEST_OPAL_CHILD_OK : TEST_OPAL_CHILD_NOT_FALSE_AGAIN;
}

/*
 * Spawn the helper, wait for it under a bounded deadline, and report how it ended:
 * SWITCH_STATUS_SUCCESS with *code set when it exited normally, SWITCH_STATUS_TIMEOUT
 * when it had to be killed at the deadline, and SWITCH_STATUS_FALSE when the fork or the
 * wait failed or it died on a signal, in which case *sig names the signal.
 *
 * *helper_pid is reported so the caller can address the working directory the helper's
 * own core created for itself, which is named after that pid.  It is set as soon as the
 * fork succeeds, so it is available on the failure paths too - which are precisely the
 * paths on which that directory must be preserved rather than removed, because it holds
 * the helper's own account of what went wrong.
 *
 * The deadline exists so that no failure mode of the helper can hang the suite: one that
 * wedges before its alarm can fire, or that inherited an ignored SIGALRM, is killed and
 * reaped here.
 */
static switch_status_t test_opal_run_readconfig_isolated(const char *argv0, int *code, int *sig, pid_t *helper_pid)
{
	test_opal_exec_plan_t plan;
	switch_uuid_t uuid;
	char token[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
	char record[SWITCH_UUID_FORMATTED_LENGTH + 128] = "";
	pid_t pid = -1;
	pid_t reaped = 0;
	int handshake[2];
	int devnull = -1;
	int record_len = 0;
	int status = 0;
	int waited_ms = 0;

	handshake[0] = handshake[1] = -1;

	*code = -1;
	*sig = 0;
	*helper_pid = -1;

	/* NEVER NEST.  A helper that somehow reached this point would spawn a helper
	 * of its own, and so on, each generation arming a fresh watchdog - so the
	 * recursion would sustain itself rather than expire at the deadline.  The
	 * refusal is keyed on the marker's PRESENCE, not on its value, so that it
	 * still holds for a marker this build does not recognise; see
	 * test_opal_helper_marker_present() for why the two questions are separate. */
	if (test_opal_helper_marker_present()) {
		return SWITCH_STATUS_FALSE;
	}

	/*
	 * THE PROVENANCE THE HELPER WILL CHECK, created before anything else, because
	 * the exec plan has to carry the descriptor number and the token into the new
	 * image's environment.  See test_opal_helper_provenance_ok() for what the other
	 * side does with them and why a marker on its own is not enough.
	 */
	if (pipe(handshake) != 0) {
		return SWITCH_STATUS_FALSE;
	}

	/* A handshake conducted over one of the three standard descriptors would be
	 * reading the suite's own console.  The helper refuses such a number outright,
	 * so refuse to create one here rather than spawning a helper that could not
	 * possibly authenticate. */
	if (handshake[0] <= STDERR_FILENO || handshake[1] <= STDERR_FILENO) {
		close(handshake[0]);
		close(handshake[1]);
		return SWITCH_STATUS_FALSE;
	}

	memset(&uuid, 0, sizeof(uuid));
	switch_uuid_get(&uuid);
	switch_uuid_format(token, &uuid);

	record_len = switch_snprintf(record, sizeof(record), "%s %s %s\n",
								 TEST_OPAL_HELPER_RECORD_TAG, TEST_OPAL_HELPER_READCONFIG, token);

	/*
	 * Written, and the write end closed, before the fork; both halves matter.  Writing
	 * first means the record is already in the pipe buffer when the new image starts, so
	 * the helper never waits on this process.  Closing the write end first means the helper
	 * sees end-of-file the instant it has consumed the record, so its read terminates on
	 * data rather than on a timeout and a longer record is impossible.  The record is under
	 * a hundred bytes against a pipe buffer of at least 4 KiB, so this write cannot block.
	 */
	if (record_len <= 0 || record_len >= (int) sizeof(record)
		|| !test_opal_write_all(handshake[1], record, (switch_size_t) record_len)) {
		close(handshake[0]);
		close(handshake[1]);
		return SWITCH_STATUS_FALSE;
	}

	close(handshake[1]);
	handshake[1] = -1;

	if (!test_opal_exec_plan_build(&plan, argv0, TEST_OPAL_HELPER_READCONFIG, handshake[0], token)) {
		close(handshake[0]);
		return SWITCH_STATUS_FALSE;
	}

	/* Opened in the PARENT, so that redirecting the helper's console output needs
	 * nothing but dup2() in the child.  A failure here is not fatal: the
	 * redirection is output hygiene, not correctness, and the verdict travels in
	 * the exit code either way. */
	devnull = open("/dev/null", O_WRONLY);

	/* Flush before forking so no buffered parent output can be duplicated into
	 * the child image.  The exec discards those buffers anyway, but flushing here
	 * removes the possibility rather than relying on that detail. */
	fflush(NULL);

	pid = fork();

	if (pid < 0) {
		test_opal_exec_plan_release(&plan);

		if (devnull >= 0) {
			close(devnull);
		}

		close(handshake[0]);

		return SWITCH_STATUS_FALSE;
	}

	if (pid == 0) {
		/*
		 * ASYNC-SIGNAL-SAFE REGION - DO NOT ADD ANYTHING TO THIS BLOCK.
		 *
		 * Exactly four call sites and no others, all on POSIX's async-signal-safe list:
		 * dup2(), alarm() and execve() on the path that works, plus _exit() only where
		 * execve failed.  Nothing here allocates, locks, logs or constructs, which is why
		 * this fork is safe in a process whose core threads are already running, and the
		 * descriptor dup2() needs was opened by the parent so the child never calls open().
		 *
		 * Only standard output is discarded, because the helper runs the same FCTX driver
		 * and its console output would otherwise interleave with this run's and corrupt the
		 * collected result.  Standard error is left alone: that is where FST_CORE_BEGIN
		 * writes when a core fails to come up at all (switch_test.h:296-298).
		 *
		 * The alarm is armed before the exec because a pending alarm survives an exec, so
		 * it covers the helper's own bootstrap.  _exit() and never exit(), so that no
		 * inherited atexit handler or stdio buffer runs in this duplicated image.
		 */
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
		}

		alarm(TEST_OPAL_CHILD_ALARM_SECONDS);
		execve(plan.path, plan.argv, plan.envp);
		_exit(TEST_OPAL_CHILD_EXEC_FAILED);
	}

	*helper_pid = pid;

	/* Safe the instant the fork returned: the child holds its own copy of this
	 * memory, so releasing the parent's cannot affect the exec. */
	test_opal_exec_plan_release(&plan);

	if (devnull >= 0) {
		close(devnull);
	}

	/* The child inherited the read end across the exec - a pipe descriptor is not
	 * close-on-exec - so this process has no further use for it.  Closing it here
	 * also means the helper is the only reader, and that this function leaks no
	 * descriptor on any path. */
	close(handshake[0]);
	handshake[0] = -1;

	while (waited_ms < TEST_OPAL_CHILD_DEADLINE_MS) {
		reaped = waitpid(pid, &status, WNOHANG);

		if (reaped == pid) {
			break;
		}

		if (reaped < 0 && errno != EINTR) {
			/* Not something the helper did, and it may well still be running:
			 * terminate and reap before reporting, so no failure path of this
			 * function can leave a process behind.  EINTR is excluded on purpose - a
			 * signal delivered to the PARENT says nothing about the helper. */
			kill(pid, SIGKILL);

			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
				;
			}

			return SWITCH_STATUS_FALSE;
		}

		/* reaped == 0 (still running), or EINTR (a signal interrupted the poll,
		 * which is not information about the helper): keep waiting. */
		switch_yield(TEST_OPAL_CHILD_POLL_MS * 1000);
		waited_ms += TEST_OPAL_CHILD_POLL_MS;
	}

	if (reaped != pid) {
		kill(pid, SIGKILL);

		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			;
		}
		return SWITCH_STATUS_TIMEOUT;
	}

	if (WIFEXITED(status)) {
		*code = WEXITSTATUS(status);
		return SWITCH_STATUS_SUCCESS;
	}

	if (WIFSIGNALED(status)) {
		*sig = WTERMSIG(status);
	}

	return SWITCH_STATUS_FALSE;
}

/*
 * Remove the working directory the HELPER's own core created for itself.
 *
 * FST_CORE_BEGIN derives its log and database directories from the test base directory
 * plus the running process's pid (switch_test.h:98-104), so every core bootstrap makes a
 * directory named after its own pid.  The helper is a real bootstrap in a real process, so
 * it makes a second one alongside the one this run owns, named after a pid that will never
 * recur - one directory of residue per run in a source tree, for a process that exited
 * before the case finished asserting.
 *
 * Only the named artefacts the core writes there are removed, and then the directory
 * itself.  rmdir() refuses a non-empty directory, so anything this cleanup does not
 * enumerate keeps the directory alive and visible rather than being deleted unseen.
 *
 * Every removal is checked and the aggregate verdict is returned, because a cleanup that
 * silently failed would leave a directory behind every run while the suite still reported
 * a clean pass.  Only absence is tolerated - ENOENT means the artefact was never written,
 * which is legitimate for the ".tmp" sibling in particular - and anything else is logged
 * with the exact path and the errno text before the verdict reaches the caller, which
 * asserts on it.
 *
 * The caller removes nothing when the helper failed, timed out or died on a signal.  On
 * those paths the directory is the helper's own account of what happened and is named in
 * the diagnostic instead of being deleted.
 */
static int test_opal_remove_helper_path(const char *path, int is_dir)
{
	int failed = 0;

	failed = is_dir ? (rmdir(path) != 0) : (unlink(path) != 0);

	if (failed && errno != ENOENT) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "helper working directory cleanup failed for %s: %s\n", path, strerror(errno));
		return 0;
	}

	return 1;
}

static switch_status_t test_opal_remove_helper_dir(pid_t helper_pid)
{
	char dir[1024] = "";
	char path[1152] = "";
	int ok = 1;

	if (helper_pid <= 0) {
		return SWITCH_STATUS_FALSE;
	}

	switch_snprintf(dir, sizeof(dir), "%s%s%lu", SWITCH_TEST_BASE_DIR_OVERRIDE, SWITCH_PATH_SEPARATOR, (unsigned long) helper_pid);

	/* &= rather than && throughout: every removal is attempted and every offender
	 * is named, so one failure cannot hide the next. */
	switch_snprintf(path, sizeof(path), "%s%s%s", dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml.fsxml");
	ok &= test_opal_remove_helper_path(path, 0);

	switch_snprintf(path, sizeof(path), "%s%s%s", dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml.fsxml.tmp");
	ok &= test_opal_remove_helper_path(path, 0);

	ok &= test_opal_remove_helper_path(dir, 1);

	return ok ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
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
 * that swept unconditionally would destroy precisely the state the shutdown case
 * exists to observe.  The sweep is therefore anchored where it cannot do that: in
 * the failure branch of the case that retains state, and unconditionally in the
 * SHUTDOWN case.  That placement is sufficient because a fatal check only
 * breaks out of the case body it appears in (switch_fct.h:3668-3669) and the
 * framework re-enters the fixture suite once per declared case
 * (switch_fct.h:3507-3516), so no failure anywhere can prevent the shutdown case
 * from running its own sweep.  The two OOS-9 cases declared after it are outside
 * that argument and do not need it: neither makes a fatal check, so neither can
 * break out early, and each releases in its own body the single thing it plants -
 * a registered stand-in module, or a planted reservation.
 *
 * The one exit that does bypass this sweep needs it least.  An environmental skip
 * leaves the process outright through test_opal_skip_run(), and everything this
 * sweep reclaims - the provider, the log capture, the two processes, the pool and
 * the port guard - is reclaimed by the kernel when that address space is
 * discarded.  A skip also hands nothing to a later case, because it ends the run.
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
 * OOS-9 co-load guard: the sibling stand-in
 * ---------------------------------------------------------------------------
 *
 * mod_opal_load() refuses to load when mod_h323 is already in the process, because the
 * two modules link different PTLib runtimes and the second FSProcess constructed
 * SIGSEGVs on the conflicting PProcess singleton (mod_opal.cpp, top of mod_opal_load;
 * evidence in blitzy/documentation/oos9-coload-determination.md).
 *
 * The refusal is asserted through the module-load API against a sibling that is genuinely
 * present, which is what the guard's predicate actually reads:
 * switch_loadable_module_exists() answers from loadable_modules.module_hash
 * (switch_loadable_module.c:1846-1863), and switch_loadable_module_build_dynamic()
 * (switch_loadable_module.h:169) registers a module in that hash under the filename it is
 * handed - the same key switch_loadable_module_process() uses for a dlopen'd module
 * (switch_loadable_module.c:1830).  Registering this two-function stand-in under
 * "mod_h323" therefore makes the sibling present to the guard EXACTLY as the real module
 * would be, while linking no second PTLib into this binary.
 *
 * That distinction is the whole reason the stand-in exists.  Loading the real mod_h323
 * here is precisely the crash the guard prevents: it would map libpt.so.2.10.9 beside
 * this binary's libpt.so.2.12-beta10 and kill the test process before it could assert
 * anything.  So the negative branch is asserted hermetically against the predicate, and
 * the end-to-end refusal of the real module - the ERROR line, the surviving process and a
 * still-UP instance - is proven in the runtime evidence archived with the determination
 * (blitzy/documentation/oos9-coload-evidence/), not here.
 *
 * The stand-in is deliberately inert: it creates a module interface, because
 * switch_loadable_module_build_dynamic() dereferences one, and does nothing else.  Its
 * shutdown routine succeeds so that switch_loadable_module_unload_module() can remove it
 * again and hand its pool back.  Both carry C language linkage so their types match
 * switch_module_load_t and switch_module_shutdown_t (switch_types.h:2608-2610) exactly.
 */
#define TEST_OPAL_SIBLING_MODULE "mod_h323"

/*
 * The process-global reservation name mod_opal.cpp and mod_h323.cpp both use for the
 * OOS-9 atomic exclusion.  Spelled out here rather than included, because the module
 * defines it in its own translation unit: this suite links the module rather than
 * including its source, so the literal is the interface.  It must stay identical to
 * OPAL_PTLIB_RESERVATION in mod_opal.cpp and H323_PTLIB_RESERVATION in mod_h323.cpp.
 *
 * WHY THAT INVARIANT IS NOT ENFORCED AT COMPILE TIME, AND WHAT ENFORCES IT INSTEAD.
 * A compile-time check would need the module's own macro in scope, which would mean
 * either including the module translation unit - it is already linked in through
 * libmodopal.la, so a second copy would be a duplicate-definition error - or adding a
 * -D to this target's flags, and this module's Makefile.am is frozen.  mod_h323's suite
 * has no such problem: it white-box-includes ../mod_h323.cpp, so it aliases the module's
 * macro directly and cannot drift.  Here the invariant is instead machine-checked at run
 * time, in BOTH directions, so drift cannot pass silently:
 *
 *   - POSITIVELY, in the module-load case: a successful mod_opal_load() must leave the
 *     reservation held under exactly this name, with this module's own name as its value.
 *     A module that reserved some other name would leave this one unset.
 *   - NEGATIVELY, in case 10: a reservation planted under exactly this name must make
 *     mod_opal_load() refuse.  A module reading some other name would not see it and the
 *     load would succeed.
 *
 * Either half fails loudly and names the drift, so the two together are equivalent in
 * effect to a static assertion that this suite is not in a position to write.
 */
#define TEST_OPAL_PTLIB_RESERVATION "_fs_ptlib_endpoint_reservation"

SWITCH_BEGIN_EXTERN_C
static switch_status_t test_opal_sibling_stub_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, TEST_OPAL_SIBLING_MODULE);

	return *module_interface != NULL ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_MEMERR;
}

static switch_status_t test_opal_sibling_stub_shutdown(void)
{
	return SWITCH_STATUS_SUCCESS;
}
SWITCH_END_EXTERN_C

/*
 * ---------------------------------------------------------------------------
 * The suite
 * ---------------------------------------------------------------------------
 *
 * A real core is required: switch_xml_open_cfg() asserts that the main XML root exists,
 * which only the full bootstrap provides.  "conf_opal" names this module's own fixture
 * root: the bootstrap builds the configuration directory as
 * SWITCH_TEST_BASE_DIR_FOR_CONF, a path separator, then that name (switch_test.h:92-93),
 * so the target must define -DSWITCH_TEST_BASE_DIR_FOR_CONF and
 * -DSWITCH_TEST_BASE_DIR_OVERRIDE to ${abs_builddir}/test.  Without those defines the
 * bootstrap falls back to "./conf_opal" (switch_test.h:94-99), which resolves against the
 * working directory, so the suite would only find its fixtures when run from inside test/.
 *
 * FCTX runs cases in declaration order within a single process, and that order is load
 * bearing:
 *
 *   1. config_absent_read_config_fails - first, so its verdict cannot be an artefact of a
 *      successful parse or load having left module state behind.  It checks both
 *      containment properties itself rather than inheriting them - the plugin-path pin
 *      fatally, the IAX2 port guard through the skip wrapper - which also makes it the
 *      point at which an occupied port ends the run before any other case has begun;
 *   2. toolkit_side_effects_are_contained - examines those same properties in detail ahead
 *      of every other case, so the IAX2 endpoint is observed at the earliest moment it
 *      could matter;
 *   3. dual_endpoint_construction - constructs a manager and looks its H.323 and IAX2
 *      endpoints up through the inherited public accessor, calling neither ReadConfig()
 *      nor Initialise();
 *   4. listener_default_signalling_port - the only case needing no manager and no
 *      PProcess: it constructs a bare FSListener and reads back the port its constructor
 *      supplied;
 *   5. settings_from_injected_configuration - ReadConfig() once against the registered
 *      provider, then the four public settings accessors;
 *   6. listener_name_defaults_to_unnamed - one manager per configuration document, two
 *      documents, ReadConfig() once on each, both stored listener names observed through
 *      captured log output.
 *
 *      Cases 3 to 6 all precede the load case and none calls FSManager::Initialise(), so
 *      every case up to this point observes an OPAL media-format registry that no
 *      Initialise() has mutated - that registry is process global and Initialise() adds to
 *      it - even though cases 5 and 6 do drive ReadConfig(), which is socket free and
 *      touches no registry;
 *
 *   7. module_load_and_endpoint_interface - the first and only case that calls
 *      Initialise(), and therefore the only case with a listener thread.  It takes the
 *      PTLib process singleton over from the suite and deliberately leaves the module
 *      loaded;
 *   8. module_shutdown_succeeds - declared after every case that builds module state, so it
 *      observes a fully initialised module and its result is asserted rather than
 *      discarded, and so that its two extra duties come last: proving the case before it
 *      orphaned no configuration provider, and running the suite's unconditional sweep;
 *   9. coload_guard_refuses_when_sibling_is_loaded - the OOS-9 refusal reached through the
 *      module hash, which needs a shut-down module and adds no state for case 8's sweep
 *      to release.
 *  10. coload_reservation_refuses_a_reserved_ptlib_runtime - last; the OOS-9 refusal
 *      reached through the ATOMIC reservation instead, which is the branch a concurrent
 *      load takes and which the module hash cannot express.
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
			 * case ordering can bypass.  Both calls are idempotent.
			 *
			 * Neither result is asserted HERE, and only here: a fixture runs
			 * outside any test body, so a failed check would have nothing to
			 * attribute itself to.  These calls are the early installation, not
			 * the guarantee.  The guarantee is enforced where it matters - every
			 * case that constructs an FSManager calls
			 * test_opal_require_containment_or_skip() first, which re-establishes
			 * the containment live and ends the run as SKIPPED when the port
			 * cannot be taken, and test_opal_acquire_process() returns NULL
			 * unless the pin verifies at that instant.
			 */
			(void) test_opal_pin_plugin_path();
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
		 * test_opal_suite_state_cleanup() in the shutdown case.
		 *
		 * Two things a fatal check CAN still strand, and why neither matters.  The
		 * shared PTLib process, which every socket-free case acquires before its
		 * first assertion: it has two independent reclamation paths, the load
		 * case's own handover and the shutdown case's sweep.  And anything allocated
		 * from fst_pool, which FST_TEARDOWN_BEGIN destroys on the way in before
		 * this body would ever run.
		 *
		 * An environmental skip strands nothing at all, whatever a case was holding
		 * when it happened: test_opal_skip_run() leaves the process, so the kernel
		 * performs the only reclamation still required.
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
		 * Case 1 - configuration absent, declared first and run in a process of its own.
		 *
		 * ReadConfig() is asserted directly rather than through mod_opal_load(), because
		 * FSManager::Initialise() discards ReadConfig()'s status, so a load would still
		 * report success and the error branch would go unobserved.  No binding is
		 * registered, so the fixture root's deliberate omission of the module's
		 * configuration section is what makes the miss deterministic.
		 *
		 * Declared first so its verdict cannot be an artefact of anything else: no
		 * configuration has been injected, no successful parse has happened and no module
		 * has been loaded.  Both containment properties are established by the setup hook,
		 * which FCTX runs ahead of every case body, and both are checked here rather than
		 * inherited from the case that examines them in detail - the plugin-path pin as a
		 * fatal precondition, the IAX2 port guard through the skip wrapper.
		 *
		 * This is the one case that cannot run in the principal process.
		 * FSManager::ReadConfig() creates a request-parameters event unconditionally and
		 * destroys it only on the success path, so every traversal of this branch orphans
		 * one malloc-backed switch_event_t.  The module source, its header and its shipped
		 * configuration must not change, and the harness cannot close the leak either,
		 * because this target links a separate compilation of the module (libmodopal.la)
		 * rather than including it, so no preprocessor seam declared here reaches that
		 * translation unit.
		 *
		 * The allocation is therefore CONTAINED.  The whole assertion set runs in a freshly
		 * exec'd image of this same binary, which _exit()s, so the address space that made
		 * the allocation is discarded without an at-exit leak check and the principal
		 * process - the one CI observes under the address sanitizer - never traverses the
		 * branch.  No suppression file, no detect_leaks=0 and no ASAN_OPTIONS edit is
		 * involved, and the allocation stays fully visible to anyone who runs the helper
		 * directly.
		 *
		 * Inside that helper the branch is driven twice over the same manager, so the
		 * repeatability of the failure verdict is the property under observation rather
		 * than a single sample.
		 */
		FST_TEST_BEGIN(config_absent_read_config_fails)
		{
			switch_status_t isolated = SWITCH_STATUS_FALSE;
			switch_status_t cleaned = SWITCH_STATUS_SUCCESS;
			pid_t helper_pid = -1;
			int child_code = -1;
			int child_signal = 0;
			int guard_regained = 0;

			/*
			 * The helper's own entry point.  When this process is the exec'd helper, its
			 * whole work is the body below and its whole result is an exit code.  It sits in
			 * the first declared case because FCTX runs cases in declaration order, so
			 * _exit()ing here guarantees no later case runs in the helper: it cannot load
			 * the module, take over the PTLib process singleton, or report a verdict into
			 * the parent's tally.
			 *
			 * _exit() rather than return, so the helper neither tears a core down through
			 * FST_CORE_END nor prints a summary the parent prints a moment later.  It is
			 * also what keeps the confinement total: no atexit handler and no
			 * leak-sanitizer at-exit check fires in the one process that deliberately
			 * traverses the leaking branch.  This is reached after the setup hook, so the
			 * plugin pin and the IAX2 containment are already installed as they are for any
			 * other case, and the body re-verifies both before relying on them.
			 *
			 * Two credentials are required, not one: the marker names the mode and the
			 * provenance handshake proves it came from the parent of this run.  Helper mode
			 * _exit()s from inside this first case, so a top-level run that entered it on an
			 * inherited or stale marker alone would run one case, exit with that case's
			 * status, and be recorded as a clean pass with the nine later cases never run.
			 *
			 * A marker without valid provenance is therefore a hard refusal rather than a
			 * fallback to an ordinary run: the marker's presence also disarms the spawn
			 * below, since test_opal_run_readconfig_isolated() refuses to nest on presence
			 * alone, so this case could not do its work either way.  A distinct non-zero
			 * code makes the misconfiguration unmistakable; 58 is outside the range a signal
			 * or libc failure produces and is neither of automake's reserved 77 (skip) or 99
			 * (framework error).
			 *
			 * The diagnostic goes to standard error rather than the core logger, for the
			 * same reason FST_CORE_BEGIN reports a failed core there
			 * (switch_test.h:296-298): the process is about to _exit(), so a message queued
			 * for the logging thread might never be written, whereas stderr is flushed here
			 * and is the one stream the helper path never redirects.
			 */
			if (test_opal_helper_marker_present()) {
				if (test_opal_in_helper_mode() && test_opal_helper_provenance_ok()) {
					fflush(NULL);
					_exit(test_opal_readconfig_child_body());
				}

				fprintf(stderr,
						"%s is set in this environment but carries no valid parent provenance, so this process "
						"refuses to run as the isolated helper and refuses to continue as an ordinary run. "
						"Unset %s, %s and %s and run the suite again.\n",
						TEST_OPAL_HELPER_ENV, TEST_OPAL_HELPER_ENV, TEST_OPAL_HELPER_FD_ENV, TEST_OPAL_HELPER_TOKEN_ENV);
				fflush(NULL);
				_exit(TEST_OPAL_CHILD_BAD_PROVENANCE);
			}

			/* Preconditions established in the PARENT before it spawns anything.
			 *
			 * The child inherits this environment, and it is the process that brings
			 * a PProcess up and constructs the manager, so an unpinned parent would
			 * hand that hazard straight to it.  The pin is a fatal assertion because
			 * it is entirely this suite's own doing - nothing outside the process can
			 * take it away - and it is asserted per case, not just once, so no
			 * re-ordering can leave a manager built without it.
			 *
			 * The containment is consulted here for a different reason than in every
			 * other case: this case builds no manager, so what it needs to establish
			 * is not an exclusion to construct under but that the port is takeable by
			 * this suite AT ALL before it gives the port up for the handover below.
			 * Proving that first is what makes the release safe: without it, a run in
			 * which the port was permanently unavailable would release nothing, spawn
			 * a helper that could not take it either, and report the outcome one layer
			 * further away from its cause.  Being the first declared case, this is also
			 * where an occupied port is discovered before any other case has run, and
			 * an occupied port ends the run as SKIPPED rather than failing it. */
			fst_requires(test_opal_plugin_path_is_pinned());
			test_opal_require_containment_or_skip();

			/*
			 * NOTE what is deliberately NOT done here: the parent does not acquire a
			 * PProcess and does not construct an FSManager.  Constructing the manager
			 * is what traverses production's leaking branch, so doing it here would
			 * put the leak in the principal process and fail the sanitizer.  Every
			 * one of those steps happens in the helper instead, and the parent's role
			 * is reduced to spawning it and decoding its verdict.
			 */

			/*
			 * The guard is handed over rather than inherited.  The helper is the process
			 * that constructs the manager, so it is the process that must own the
			 * exclusion, and it cannot be handed a bound socket because it is a freshly
			 * exec'd image needing a socket of its own in its own address space.  Nor can
			 * the parent keep holding the port while the helper runs, because the helper
			 * would then be asserting containment it does not own on the strength of an
			 * address-in-use refusal it cannot renew.
			 *
			 * So ownership moves.  The parent has already proved the port is takeable by
			 * this suite and releases it here; the helper's setup hook takes it, and the
			 * helper's body checks it before acquiring a PProcess or constructing
			 * anything, so a helper that failed to take it reports
			 * TEST_OPAL_CHILD_NO_CONTAINMENT and builds no manager.
			 *
			 * The window in which the port is free contains no manager at all: the parent
			 * constructs none - asserted at the end of this case, where it still owns no
			 * PTLib process - and the helper constructs one only after taking the port.  A
			 * third party that grabs the port inside that window is therefore never a
			 * safety problem, only a lost opportunity to observe: it stops the helper at
			 * its own containment check before anything is built, and the parent turns
			 * that verdict into a skip below rather than into a failure this suite's
			 * subject did not cause.
			 */
			test_opal_iax2_guard_release();

			/* argv[0] is FCTX's own main() parameter and is the fallback image path;
			 * /proc/self/exe is preferred where it exists.  Passing it in rather than
			 * reaching for a global keeps the spawn helper free of hidden inputs. */
			isolated = test_opal_run_readconfig_isolated(argv[0], &child_code, &child_signal, &helper_pid);

			/*
			 * Ownership comes straight back, as the very next statement after the
			 * helper has been reaped, so the free window closes here and not at the
			 * mercy of an assertion below breaking out of the case body first.  The
			 * helper's guard died with its address space - a UDP socket has no
			 * lingering state to wait out - so this is expected to succeed on the
			 * failure paths too, and it is recorded rather than asserted inline so
			 * that the diagnosis of a failed helper is logged before any verdict is
			 * pronounced on the handover.
			 */
			guard_regained = test_opal_iax2_containment_in_effect();

			if (isolated == SWITCH_STATUS_SUCCESS && child_code == TEST_OPAL_CHILD_OK) {
				/* Clean run: the helper's own pid-named log directory is residue and
				 * nothing more, so it goes - and the verdict is kept, because a
				 * cleanup that silently failed would let residue accumulate one
				 * directory per run while the suite still reported a pass.  On the
				 * failure branch below no cleanup is attempted at all, and `cleaned'
				 * keeps its initial SWITCH_STATUS_SUCCESS so the assertion further
				 * down says nothing about a directory that is being preserved on
				 * purpose. */
				cleaned = test_opal_remove_helper_dir(helper_pid);
			} else {
				/* Emitted before the assertions so the diagnosis is on the log even
				 * when the run is later truncated, and naming the helper's preserved
				 * log directory because that is where its own account of the failure
				 * is. */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
								  "isolated configuration-absent check: status=%d exit=%d signal=%d; helper log kept at %s%s%lu\n",
								  (int) isolated, child_code, child_signal, SWITCH_TEST_BASE_DIR_OVERRIDE, SWITCH_PATH_SEPARATOR,
								  (unsigned long) helper_pid);
			}

			/* SWITCH_STATUS_TIMEOUT here means the helper had to be killed at the
			 * deadline; SWITCH_STATUS_FALSE means the fork or the wait failed, or the
			 * helper died on a signal - SIGALRM being its own watchdog firing. */
			fst_xcheck(isolated == SWITCH_STATUS_SUCCESS,
					   "the isolated configuration-absent check must run to completion in its own process");
			fst_xcheck(child_signal == 0, "the isolated configuration-absent check must not be terminated by a signal");

			/* An occupied port is not a verdict on the module.  A third party that took
			 * the IAX2 default port inside the handover window stopped the helper at its
			 * own containment check before it built anything, which says nothing about
			 * ReadConfig() - so the run reports the Automake skip status rather than
			 * failing a case for a condition of the host.  Placed after the two checks
			 * above, so a helper killed at the deadline or by a signal is still a
			 * failure, and after the ERROR line above, so the diagnosis reaches the log
			 * first.  Every OTHER non-zero helper code stays a failure below. */
			if (isolated == SWITCH_STATUS_SUCCESS && child_signal == 0
				&& child_code == TEST_OPAL_CHILD_NO_CONTAINMENT) {
				test_opal_skip_run("the isolated helper could not take the IAX2 default UDP port during the "
								   "handover window, so it could not construct the FSManager this case observes");
			}

			fst_xcheck(child_code == TEST_OPAL_CHILD_OK,
					   "ReadConfig() must report SWITCH_STATUS_FALSE when no opal.conf can be located");

			/* RESIDUE NEUTRALITY, asserted rather than assumed.  A clean helper run
			 * must leave this suite with exactly the one pid-named working directory a
			 * single-core suite leaves; test_opal_remove_helper_dir() has already
			 * logged the offending path and errno for anything it could not remove. */
			fst_xcheck(cleaned == SWITCH_STATUS_SUCCESS,
					   "the helper's pid-named working directory must be removed in full after a clean isolated run");

			/*
			 * THE HANDOVER COMPLETED.  Establishing this is what makes the release
			 * above safe to have done: the port is owned by this process again, so
			 * every case declared after this one - each of which constructs a manager -
			 * inherits a real exclusion rather than a port the helper might have left
			 * bound or a third party might have taken during the window.
			 *
			 * A failure to regain it cannot be the helper's doing: it has been reaped,
			 * its address space is gone, and a UDP socket leaves no lingering state to
			 * wait out - so the only way the port is still unavailable at this point is
			 * that something else on the host owns it now.  That is the same condition
			 * the guard reports everywhere else, so it ends the run as SKIPPED, and it
			 * ends it HERE, before any case that would otherwise construct a manager
			 * without the exclusion it needs.
			 */
			if (!guard_regained) {
				test_opal_skip_run("the IAX2 default UDP port could not be re-taken once the isolated helper had "
								   "been reaped, so a later case would construct an FSManager without containment");
			}

			/* THE ISOLATION PROPERTY ITSELF.  The parent never built a manager, so
			 * it still owns no PTLib process - which is what makes the containment
			 * case declared next able to observe an FSManager's very first IAX2
			 * attempt, and what keeps this process free of the leak the helper
			 * absorbed. */
			fst_check(test_opal_process == NULL);
		}
		FST_TEST_END()

		/*
		 * Case 2 - the toolkit's side effects are contained.
		 *
		 * Declared immediately after the configuration-absent branch, and before
		 * every other case, because it examines in detail the preconditions those
		 * cases depend on, and because the thing it is asserting about - the IAX2
		 * listener - appears the instant an FSManager is constructed.  The
		 * containment itself is installed by the setup hook rather than by this
		 * case, so being second costs nothing: case 1 asserts the same two
		 * properties as bare preconditions, and this case is where they are
		 * proved rather than merely required.
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

			test_opal_require_containment_or_skip();

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

			fst_check(test_opal_iax2_containment_in_effect());
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
		 * constructor tries to listen on the wildcard address - and case 2
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
			/* The IAX2 wildcard listener must be unable to bind before a manager is
			 * constructed.  Consulted per case, not just once, so no re-ordering can
			 * leave a manager built without it; a port this suite cannot take is a
			 * condition of the host, so the run reports the Automake skip status
			 * rather than failing the case. */
			test_opal_require_containment_or_skip();

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
			/* The IAX2 wildcard listener must be unable to bind before a manager is
			 * constructed.  Consulted per case, not just once, so no re-ordering can
			 * leave a manager built without it; a port this suite cannot take is a
			 * condition of the host, so the run reports the Automake skip status
			 * rather than failing the case. */
			test_opal_require_containment_or_skip();
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
			/* The IAX2 wildcard listener must be unable to bind before a manager is
			 * constructed.  Consulted per case, not just once, so no re-ordering can
			 * leave a manager built without it; a port this suite cannot take is a
			 * condition of the host, so the run reports the Automake skip status
			 * rather than failing the case. */
			test_opal_require_containment_or_skip();

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
				switch_status_t drained = SWITCH_STATUS_FALSE;

				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					/* CLOSING BARRIER, before either observation is read.  Placed
					 * here rather than after the wait so that ONE barrier makes
					 * both reads decidable: once it opens, every line ReadConfig()
					 * queued has been dispatched, so the awaited name is already
					 * captured and the foreign slot is final rather than merely
					 * empty-so-far. */
					drained = test_opal_log_barrier_close();
					fst_xcheck(drained == SWITCH_STATUS_SUCCESS,
							   "the log queue must be drained past ReadConfig() before the listener observations are read");

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
				switch_status_t drained = SWITCH_STATUS_FALSE;

				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status == SWITCH_STATUS_SUCCESS) {
					/* CLOSING BARRIER, exactly as in half one and for the same
					 * reason: the contrast is only meaningful if the foreign slot
					 * is read after the queue has drained past ReadConfig(). */
					drained = test_opal_log_barrier_close();
					fst_xcheck(drained == SWITCH_STATUS_SUCCESS,
							   "the log queue must be drained past ReadConfig() before the listener observations are read");

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
			char *reservation_holder = NULL;

			fst_requires(test_opal_acquire_process() != NULL);
			/* The IAX2 wildcard listener must be unable to bind before a manager is
			 * constructed.  Consulted per case, not just once, so no re-ordering can
			 * leave a manager built without it; a port this suite cannot take is a
			 * condition of the host, so the run reports the Automake skip status
			 * rather than failing the case. */
			test_opal_require_containment_or_skip();

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

			if (status == SWITCH_STATUS_SUCCESS) {
				/* The POSITIVE half of the reservation-name drift check described above
				 * TEST_OPAL_PTLIB_RESERVATION.  A load that succeeded claimed the PTLib
				 * runtime, and it claimed it under the module's OPAL_PTLIB_RESERVATION
				 * with `modname' as the value (mod_opal.cpp:169), so the variable this
				 * suite spells out independently must now read back as "mod_opal".  If
				 * the module's spelling ever moves, this reads NULL and says so - which
				 * is the check case 10 cannot make, because an unset reservation is
				 * indistinguishable from a correctly released one there. */
				reservation_holder = switch_core_get_variable_dup(TEST_OPAL_PTLIB_RESERVATION);
				fst_xcheck(reservation_holder != NULL && !strcmp(reservation_holder, TEST_OPAL_MODULE_NAME),
						   "a successful mod_opal_load must hold the PTLib reservation under exactly "
						   "TEST_OPAL_PTLIB_RESERVATION; an empty or foreign holder means that literal has "
						   "drifted from OPAL_PTLIB_RESERVATION in mod_opal.cpp");
				switch_safe_free(reservation_holder);
			}

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
		 * this case always runs even when an earlier one exited early - the sole
		 * exception being an environmental skip, which ends the process and so
		 * leaves no binding behind for anything to find.  That makes this the right
		 * place to state the invariant, and the only place a regression that
		 * orphaned a binding would be caught by name rather than silently answering
		 * some later lookup.
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
			 * case, and because it is declared after every case that builds module
			 * state it runs even when an earlier case exited early; only an
			 * environmental skip bypasses it,
			 * and that discards the address space instead of leaving state behind.
			 * It reclaims everything that can outlive a single case - the provider,
			 * the log capture, the module's process, the suite's own process and the
			 * module-lifetime pool - in the one order that is safe, and each step is
			 * idempotent, so repeating work the case has already done above costs
			 * nothing.  Shutdown was the last thing to touch pool-backed module
			 * state, which is why the pool goes last.
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

		/*
		 * CASE 9 - the OOS-9 co-load refusal, declared last.
		 *
		 * Last for two reasons.  It needs a module that is NOT loaded, which is exactly
		 * what case 8 leaves behind, and it must not perturb the eight cases before it:
		 * the sibling stand-in it registers would make the load case refuse if it ever
		 * outlived this case.  Being last also means the stand-in cannot strand
		 * anything, since the only assertion after its removal is the removal itself,
		 * and no check here is fatal.
		 *
		 * What is asserted is the guard's contract as the refine directive states it:
		 * the predicate reports the sibling's absence and presence correctly, a load
		 * attempted with the sibling present returns SWITCH_STATUS_FALSE, the module
		 * interface is never created, and no FSProcess is constructed - which is the
		 * property that matters, because constructing one is what crashes.  The
		 * refusal's ERROR line names the PProcess conflict and both libpt versions; it
		 * is written by the guard itself and read back in the archived runtime proof.
		 *
		 * fst_pool is the pool handed to the refused load deliberately: the guard
		 * returns before switch_loadable_module_create_module_interface() is reached, so
		 * nothing is ever allocated from it, and using the per-case pool keeps this case
		 * clear of the module-lifetime pool that case 8's sweep destroyed.
		 *
		 * See the sibling stand-in above for why the real mod_h323 is not loaded here
		 * and where the end-to-end proof lives instead.
		 */
		FST_TEST_BEGIN(coload_guard_refuses_when_sibling_is_loaded)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			const char *err = NULL;

			/* The positive control.  Nothing in this binary has ever registered the
			 * sibling, so the guard's predicate must report absence - which is why the
			 * load case was allowed to proceed. */
			fst_check(switch_loadable_module_exists(TEST_OPAL_SIBLING_MODULE) == SWITCH_STATUS_FALSE);

			/* Registration is a statement whose status is checked afterwards, never the
			 * expression of a fatal check: a fatal check here would break out of the
			 * body and leave the stand-in registered. */
			status = switch_loadable_module_build_dynamic((char *) TEST_OPAL_SIBLING_MODULE,
														 test_opal_sibling_stub_load, NULL,
														 test_opal_sibling_stub_shutdown, SWITCH_FALSE);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "the mod_h323 stand-in must register before the co-load guard is exercised");

			if (status == SWITCH_STATUS_SUCCESS) {
				fst_check(switch_loadable_module_exists(TEST_OPAL_SIBLING_MODULE) == SWITCH_STATUS_SUCCESS);

				status = mod_opal_load(&module_interface, fst_pool);

				fst_xcheck(status == SWITCH_STATUS_FALSE, "mod_opal must refuse to load while mod_h323 is present (OOS-9)");
				fst_check(module_interface == NULL);
				fst_check(!PProcess::IsInitialised());

				/* Cleanup tail: remove the stand-in and confirm the predicate answers
				 * absence again, so the refusal is provably a function of the sibling's
				 * presence rather than of anything permanent this case did. */
				fst_check(switch_loadable_module_unload_module("", TEST_OPAL_SIBLING_MODULE, SWITCH_FALSE, &err) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_loadable_module_exists(TEST_OPAL_SIBLING_MODULE) == SWITCH_STATUS_FALSE);
			}
		}
		FST_TEST_END()

		/*
		 * CASE 10 - the OOS-9 refusal reached through the ATOMIC RESERVATION, declared last.
		 *
		 * Case 9 asserts the guard that reads the module hash, and that guard is a snapshot:
		 * switch_loadable_module_exists() takes loadable_modules.mutex for its own lookup and
		 * releases it, while the core holds no single lock across a sibling observation, a
		 * module's load routine and the publication of its result
		 * (switch_loadable_module_load_module_ex()).  Two concurrent loads can therefore both
		 * see the sibling absent.  What stops them is the second guard: a compare-and-swap on
		 * a process-global reservation, performed by switch_core_set_var_conditional() under
		 * runtime.global_var_rwlock in write mode.
		 *
		 * That branch cannot be reached by registering a sibling, because a registered sibling
		 * is refused by case 9's guard first and the reservation is never consulted.  So it is
		 * reached the way a losing concurrent claimant reaches it: the reservation is taken
		 * under the SIBLING's name while the sibling is NOT in the module hash - exactly the
		 * state the window produces - and mod_opal_load() is then asked to load into it.
		 *
		 * Asserted: the sibling really is absent from the hash, so case 9's guard cannot be
		 * what refuses; the load returns SWITCH_STATUS_FALSE; no module interface is created;
		 * and no FSProcess is constructed, which is the property that matters because
		 * constructing a second one is what crashes the process.  The reservation this case
		 * planted is then released and its absence confirmed, so nothing is stranded.
		 */
		FST_TEST_BEGIN(coload_reservation_refuses_a_reserved_ptlib_runtime)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			char *holder = NULL;

			/* The reservation must start out unheld: case 8 shut the module down, and that
			 * shutdown releases it.  If it were still held, this case would be asserting a
			 * leak rather than the guard. */
			holder = switch_core_get_variable_dup(TEST_OPAL_PTLIB_RESERVATION);
			fst_xcheck(holder == NULL, "the PTLib reservation must be unheld once mod_opal has shut down");
			switch_safe_free(holder);

			/* The distinguishing control: the sibling is NOT registered, so the module-hash
			 * guard asserted in case 9 cannot be the thing that refuses below. */
			fst_check(switch_loadable_module_exists(TEST_OPAL_SIBLING_MODULE) == SWITCH_STATUS_FALSE);

			/* Take the reservation under the sibling's name, the way the sibling's own load
			 * would have taken it a moment before publishing itself. */
			fst_xcheck(switch_core_set_var_conditional(TEST_OPAL_PTLIB_RESERVATION,
													  TEST_OPAL_SIBLING_MODULE, "") == SWITCH_TRUE,
					   "the sibling stand-in must be able to claim the PTLib reservation");

			status = mod_opal_load(&module_interface, fst_pool);

			fst_xcheck(status == SWITCH_STATUS_FALSE,
					   "mod_opal must refuse to load while another endpoint holds the PTLib reservation (OOS-9)");
			fst_check(module_interface == NULL);
			fst_check(!PProcess::IsInitialised());

			/* The refused load must not have taken the reservation from its holder either. */
			holder = switch_core_get_variable_dup(TEST_OPAL_PTLIB_RESERVATION);
			fst_check(holder != NULL && !strcmp(holder, TEST_OPAL_SIBLING_MODULE));
			switch_safe_free(holder);

			/* Cleanup tail: release what this case planted, and confirm the release, so the
			 * refusal is provably a function of the reservation rather than of anything
			 * permanent this case did. */
			fst_check(switch_core_set_var_conditional(TEST_OPAL_PTLIB_RESERVATION, NULL,
													 TEST_OPAL_SIBLING_MODULE) == SWITCH_TRUE);

			holder = switch_core_get_variable_dup(TEST_OPAL_PTLIB_RESERVATION);
			fst_check(holder == NULL);
			switch_safe_free(holder);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
