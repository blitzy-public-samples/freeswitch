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
 * opens a random port or depends on the wall clock.
 *
 * Threads, stated as two separate facts because they are two separate things.
 * mod_opal declares no FreeSWITCH runtime entry point - SWITCH_MODULE_DEFINITION
 * takes (name, load, shutdown, runtime) (switch_types.h:2650) and
 * mod_opal.cpp:100 passes NULL as that fourth (runtime) argument - so no case
 * here can start a module runtime thread, because there is none to start.  The
 * OPAL toolkit is a separate matter, and it does start threads of its own:
 *
 *   - IAX2EndPoint's constructor calls Initialise(), which on a successful
 *     wildcard listen starts an IAX2Transmit and an IAX2Receiver thread.  Left
 *     to itself, constructing an FSManager therefore starts two.  Neither ever
 *     runs here, but that is an outcome of the containment described next
 *     rather than something the module does for us, and case 2 asserts it
 *     instead of assuming it.
 *
 *   - FSManager::Initialise() hands every configured listener to
 *     H323EndPoint::StartListener() (mod_opal.cpp:284-292), and an OpalListener
 *     owns a PThread for as long as it is open (opal/transports.h:491), so a
 *     listener that binds IS a live toolkit thread.  Case 7 is the only case
 *     that reaches Initialise(), and it reaches it twice - once for a scoped
 *     local FSManager and once inside mod_opal_load() - so that case accounts
 *     for up to two listener threads over its lifetime, one at a time.  Both
 *     are confined to 127.0.0.1:21720 TCP, the only listener either manager is
 *     ever told about.
 *
 * Those listener threads are reclaimed by ownership rather than by hope.  The
 * scoped manager is destroyed at the end of its own block, before
 * mod_opal_load() runs, so its listener is closed before the module's own opens
 * on the same address.  The module's listener is then left alive on purpose, so
 * that case 8 has a loaded module to assert a shutdown status against, and it
 * goes away when mod_opal_shutdown() deletes opal_process: FSProcess's
 * destructor deletes the manager (mod_opal.cpp:244-246), ~OpalManager() deletes
 * every endpoint still attached to it after ShutDown() has been called on each
 * (opal/manager.h:163-167, opal/endpoint.h:92-96) - which is exactly the
 * ownership mod_opal itself relies on (mod_opal.cpp:266) - and closing an
 * OpalListener joins its thread (opal/transports.h:469-474).  The suite's sweep
 * calls mod_opal_shutdown() unconditionally, so that reclamation does not depend
 * on case 8 reaching its own assertions.
 *
 * Socket footprint, stated so that nobody has to rediscover it.  Every socket
 * this suite is responsible for is bound to loopback, and the guard below is not
 * opened at all in one of the two containment states:
 *
 *   127.0.0.1:21720 TCP - the H.323 call-signalling listener the injected
 *             configuration declares, held only for as long as the FSManager
 *             that read that configuration lives.
 *
 *   127.0.0.1:4569 UDP - the suite's own guard on the IAX2 default port.  It
 *             exists to make a socket NOT happen.  FSManager's constructor
 *             allocates an IAX2EndPoint unconditionally (mod_opal.cpp:262-270)
 *             and IAX2EndPoint's constructor calls Initialise(), which listens
 *             on the WILDCARD address on this port and, if that succeeds, starts
 *             an IAX2Transmit and an IAX2Receiver thread.  Nothing mod_opal
 *             reads narrows the address, and the harness must not change the
 *             module, so instead the suite makes the port unavailable before any
 *             manager exists - normally by taking it on loopback itself.  A
 *             loopback holder is enough to refuse a wildcard bind, so the listen
 *             fails and Initialise() returns before creating either thread.
 *
 *             The guard is NOT one socket held continuously from the first case
 *             to the last.  The suite's sweep releases it, deliberately last so
 *             that it outlives every manager the suite built, and the setup hook
 *             re-acquires it ahead of the next case body.  What is guaranteed is
 *             that it is in force whenever a case body runs - the only window in
 *             which a manager can exist - not that a single socket persists for
 *             the whole run.
 *
 *             And there are TWO states that count as contained, either of which
 *             test_opal_iax2_containment_in_effect() accepts: this suite holds
 *             the port on loopback, or something outside this suite already
 *             holds it.  The second state is established ONLY by an EADDRINUSE
 *             refusal of the guard's own bind, which is itself the proof that
 *             the port is taken, and in that state the suite owns no guard
 *             socket at all.  Any other bind failure leaves the port free, is
 *             logged as an error and does not count, so the manager-constructing
 *             cases refuse to run rather than proceed on a maybe.  The verdict
 *             is recomputed on every acquire and discarded on every release, so
 *             it is never a remembered claim about the past.
 *
 *             What the containment changes is one thing, and it is observable:
 *             the IAX2 endpoint stays UNINITIALISED.  InitialisedOK() is false
 *             for it for the whole of its manager's life, and case 2 asserts
 *             exactly that rather than assuming it, from inside the process and
 *             from outside it.  OPAL reports the refused listen through PTRACE
 *             and propagates no status, so FSManager construction still runs to
 *             completion and the endpoint is still allocated and still attached
 *             to the manager - case 3 finds it there.  Nothing in this suite
 *             treats an uninitialised IAX2 endpoint as equivalent to an
 *             initialised one; it is not, and the suite asserts the difference
 *             instead of papering over it.  See the containment section further
 *             down for the measured bind semantics all of this relies on.
 *
 *   0.0.0.0:4569 UDP - what the containment prevents, and what must never
 *             appear.  An IAX2 listener on the wildcard address would be
 *             reachable from off-box for as long as the process lived
 *             (CWE-668).  It is not prevented by hope: no case in this suite
 *             constructs an FSManager without first fatally requiring that the
 *             port is already unavailable to a wildcard bind, so a run in which
 *             neither containment state could be established aborts rather than
 *             exposing the socket, and case 2 is what fails if a wildcard owner
 *             ever does appear.
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
 * default, because case 4 already covers that default and mixing the two would
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
 * GetDisableTranscoding only.  Constructing an FSListener directly, as case 4
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

/* The OPENING barrier: everything queued before this arming is drained away. */
static switch_status_t test_opal_log_barrier(void)
{
	return test_opal_log_await_sentinel(test_opal_log_sentinel, &test_opal_log_barrier_seen);
}

/*
 * The CLOSING barrier: everything the action queued has been dispatched.
 *
 * WHY THIS IS REQUIRED AND WHAT IT BUYS
 * -------------------------------------
 * Waiting for the AWAITED listener line proves that line arrived; it proves
 * nothing about lines queued after it.  The log queue has a single consumer
 * thread, so it is strictly FIFO: once a sentinel enqueued after the action has
 * been handed to this logger, every node the action enqueued before it has
 * already been handed over too.  Only then is "no OTHER listener was announced"
 * a decidable question - read any earlier and a foreign line still sitting in the
 * queue would be silently missed, which is an observation race rather than a
 * genuine absence (CWE-362).
 *
 * A FRESH sentinel is minted per close, under the mutex the logger holds while it
 * reads it, so the write is synchronised against that read.  Minting rather than
 * reusing matters: the wait loop re-emits once per slice, so a surplus copy of an
 * earlier sentinel may still be in the queue, and reusing a string would let the
 * barrier open on a stale node that predates the action.  The epoch counter is
 * shared with the opening barrier so no two sentinels of either kind can ever
 * collide.
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
	/* Disarm any closing barrier from a previous arming, so this arming's window
	 * cannot be closed by a sentinel that predates it. */
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
 * the production module already uses, so harness and module agree.
 *
 * WHY setenv() AND NOT putenv()
 * -----------------------------
 * setenv() COPIES both the name and the value into storage the C library owns,
 * so nothing belonging to this file is retained by the environment.  putenv()
 * instead RETAINS the caller's buffer, which is what makes the common
 * `putenv((char *) "NAME=value")` idiom a const-correctness violation: it casts
 * away const from a string literal and hands the result to an interface that is
 * entitled to write through it, so any later write - including a putenv() of the
 * same name from another library - is undefined behaviour (CWE-758).  setenv()
 * removes the hazard rather than reasoning about why it might not bite.
 *
 * The overwrite flag is 1 deliberately: this must REPLACE an inherited value,
 * not merely supply a default for an unset one, which is the whole point of
 * pinning against a hostile or stale environment.
 *
 * It is wired in two places on purpose - the suite setup hook, which FCTX runs
 * before every case body, and the acquire helper below, which is the only place
 * in the suite that constructs a PProcess.  Either alone suffices today; both
 * means no re-ordering and no new case can reintroduce the exposure.  It is
 * idempotent, so paying twice costs nothing.
 *
 * NOTHING ABOUT THE PIN IS CACHED, AND THAT IS THE POINT.  setenv() can fail -
 * it returns non-zero and sets errno on an allocation failure - so a helper that
 * assumed success and remembered it would report containment that does not
 * exist, and every assertion resting on that memory would pass while the process
 * came up against an unaudited search path.  The pin is therefore re-applied and
 * RE-VERIFIED BY READBACK on every call, and the verdict is derived from the
 * environment as it is at that instant rather than from a flag.  Two setenv()
 * calls and two getenv()/strcmp() pairs are far too cheap for the saving to be
 * worth the failure mode.
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
 * Set ONLY when the guard's bind was refused BECAUSE THE ADDRESS IS ALREADY IN
 * USE, which is the one failure that means some other holder owns the port.
 * That produces the same containment this suite would have produced itself - a
 * wildcard bind will be refused too - so it is an acceptable substitute for
 * holding the guard.
 *
 * NO OTHER FAILURE QUALIFIES, and the distinction is the whole point.  A bind
 * that failed because the process ran out of descriptors, or lacked permission,
 * or was handed an address that does not exist locally, leaves the port FREE:
 * treating that as containment would let a manager construct and open UDP 4569
 * on the wildcard address while the suite asserted that it could not.  Every
 * such failure is therefore a hard containment failure, which the fatal
 * precondition in each manager-constructing case turns into an aborted case.
 *
 * The verdict is recomputed from scratch on every acquire and discarded on every
 * release, so it always describes the port as it is now rather than as it once
 * was.
 */
static int test_opal_iax2_port_has_foreign_owner = 0;

/*
 * True for a bind status that means "already in use".
 *
 * switch_socket_bind() is a thin wrapper over fspr_socket_bind()
 * (src/switch_apr.c:744), and the Unix implementation returns the platform's own
 * error code unchanged - `if (bind(...) == -1) return errno;'
 * (libs/apr/network_io/unix/sockets.c:160).  fspr defines canonical predicates
 * for a handful of codes (APR_STATUS_IS_EACCES, _EAGAIN, _EINTR) but NONE for
 * address-in-use, so there is no portable macro to defer to and the comparison
 * is made against the platform's EADDRINUSE directly.  <errno.h> arrives through
 * switch.h, and the value compared is the one the kernel gave the failing bind.
 * The cast is needed only because switch_status_t is an enum.
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

	/* No guard is held, so nothing is yet known about the port on this attempt:
	 * discard any earlier verdict so that what follows is the only thing that can
	 * establish one.  This is what stops a single historical refusal from being
	 * remembered as containment for the rest of the run. */
	test_opal_iax2_port_has_foreign_owner = 0;

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

		/* ONLY an address-in-use refusal establishes that someone else owns the
		 * port and therefore that the containment exists without this guard.  Any
		 * other failure leaves the port free, so the verdict stays false and the
		 * manager-constructing cases refuse to run. */
		test_opal_iax2_port_has_foreign_owner = test_opal_status_is_addr_in_use(bound);

		if (!test_opal_iax2_port_has_foreign_owner) {
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
 * The foreign-owner verdict is discarded too.  Once the guard is gone this suite
 * knows nothing about who holds the port, and a remembered verdict would be a
 * claim about the past presented as a fact about the present.  The next acquire
 * establishes it again from scratch.
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

	test_opal_iax2_port_has_foreign_owner = 0;
}

/*
 * True when the IAX2 default port cannot be bound on the wildcard address by
 * anyone - either because this suite holds it on loopback or because another
 * holder already had it.
 *
 * A LIVE CHECK, not a reading of remembered state.  A held guard answers
 * immediately; otherwise the guard is taken NOW, which both establishes the
 * containment when the port is free and recomputes the foreign-owner verdict
 * from that attempt alone.  Only then is the verdict consulted.  So a caller can
 * never be told "contained" on the strength of a stale flag, and every
 * manager-constructing case - each of which requires this fatally before it
 * constructs anything - is gated on the state of the port at that instant.
 */
static int test_opal_iax2_containment_in_effect(void)
{
	if (test_opal_iax2_guard) {
		return 1;
	}

	if (test_opal_iax2_guard_acquire() == SWITCH_STATUS_SUCCESS) {
		return 1;
	}

	return test_opal_iax2_port_has_foreign_owner;
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
 * WHY THIS EXISTS
 * ---------------
 * FSManager::ReadConfig() creates an event with switch_event_create() and, on the
 * path where switch_xml_open_cfg() cannot locate the module's configuration,
 * returns without ever destroying it (mod_opal.cpp:347-357).  That is a
 * deterministic leak in PRODUCTION code, and production code is immutable for
 * this engagement (AAP 0.8.1), so it cannot be fixed at its source and it cannot
 * be avoided while still asserting the branch - which is a required behaviour.
 *
 * Executing it in the principal suite process would therefore make this binary
 * fail the address sanitizer that the CI unit-test arm always enables
 * (ci.sh:76-79), turning a required assertion into a red build.  Running it in a
 * process of its own resolves the conflict exactly: the branch is genuinely
 * traversed and genuinely asserted, while the leak is confined to a short-lived
 * process that exits through _exit() and so never reaches a leak check at all.
 * The principal process stays sanitizer-clean because it never constructs the
 * FSManager that would leak.
 *
 * HOW THE CHILD IS ISOLATED: fork() IMMEDIATELY FOLLOWED BY exec()
 * ---------------------------------------------------------------
 * By the time any case body runs, FST_CORE_BEGIN has brought a multithreaded
 * FreeSWITCH core up.  fork() duplicates only the calling thread, so every mutex
 * the other threads happened to hold is duplicated LOCKED and can never be
 * released in the child - the classic inherited-lock deadlock (CWE-667).  Any
 * allocator, logger, XML or PTLib call in a forked child of such a process can
 * therefore block forever, because all three of the core allocator, the logging
 * queue and PTLib's factory registries are lock-protected.
 *
 * The child consequently does NOT do the work.  It reaches execve() immediately,
 * touching nothing but async-signal-safe calls on the way, and the assertions run
 * in the fresh image AFTER that exec has replaced the address space - a process
 * with one thread, its own core, its own allocator and no inherited lock state at
 * all.  The helper is selected by an environment variable, and it announces itself
 * by exit code.
 */

/* Exit codes the helper returns.  Distinct values so an unexpected outcome names
 * its own failure mode in the parent's diagnostic rather than merely being
 * "non-zero". */
#define TEST_OPAL_CHILD_OK              0	/* ReadConfig() reported SWITCH_STATUS_FALSE */
#define TEST_OPAL_CHILD_UNPINNED        50	/* the plugin search path was not verifiably pinned */
#define TEST_OPAL_CHILD_NO_CONTAINMENT  51	/* the IAX2 wildcard listener was not contained */
#define TEST_OPAL_CHILD_NO_PROCESS      52	/* no PTLib process could be acquired */
#define TEST_OPAL_CHILD_NOT_FALSE       53	/* ReadConfig() did NOT report failure */
#define TEST_OPAL_CHILD_NOT_FALSE_AGAIN 54	/* the repeated ReadConfig() did NOT report failure */
#define TEST_OPAL_CHILD_EXEC_FAILED     57	/* fork() succeeded, execve() did not */
#define TEST_OPAL_CHILD_BAD_PROVENANCE  58	/* helper marker without parent provenance   */

/*
 * THE MARKER IS NOT, BY ITSELF, AUTHORITY TO RUN AS THE HELPER
 * -----------------------------------------------------------
 * The marker below names the mode, and nothing more.  It is an ordinary
 * environment variable, so it is inherited by anything this binary is run under
 * and it survives in any shell that exported it once.  A process that dispatched
 * the helper body on the marker alone would therefore run exactly one case and
 * exit with that case's status - and because that status is zero when the case
 * passes, the run would be recorded as a clean pass with every later case
 * silently never executed.  A test suite that can be reduced to one case by a
 * stale environment variable, without saying so, is not a safe suite.
 *
 * So the marker is paired with a SECOND credential that cannot be inherited
 * usefully: a one-time record the parent writes into an anonymous pipe before it
 * forks.  The descriptor number and a freshly generated token travel in the
 * child's environment, and the helper insists that the descriptor really is a
 * pipe and that the record on it matches the token exactly.  A token from another
 * run does not match; a marker with no pipe behind it has nothing to match
 * against.  Both credentials together mean the parent of THIS run asked for a
 * helper; anything less is refused outright with TEST_OPAL_CHILD_BAD_PROVENANCE.
 */

/* The marker that turns an ordinary run of this binary into the helper. */
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

/* Preferred image path; argv[0] is the fallback when /proc is not mounted. */
#define TEST_OPAL_SELF_EXE              "/proc/self/exe"

/*
 * The helper's own watchdog, and the parent's independent bound on it.
 *
 * The alarm is armed BEFORE the exec because a pending alarm survives exec, so it
 * covers the helper's own bootstrap as well as its body.  The parent's deadline is
 * deliberately longer, so the helper's self-watchdog normally fires first and the
 * parent reports a signal rather than a timeout - the more precise diagnosis.
 * The measured helper runtime is about a second, so both bounds carry a very large
 * safety factor and neither can be reached by ordinary slowness.
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
 * Presence of the marker WHATEVER ITS VALUE, which is a deliberately different
 * question from the one above.
 *
 * WHY TWO PREDICATES AND NOT ONE
 * ------------------------------
 * The exact-value test decides whether this process should RUN the helper body;
 * this presence test decides whether it is allowed to SPAWN one.  Keeping them
 * separate removes a single point of failure: recursion is impossible unless BOTH
 * are wrong at once.
 *
 * If the value test alone governed both, a marker that failed to match - a
 * mistyped value, a future mode name that diverged, or a regression in the
 * comparison itself - would leave a process that is a helper but does not know
 * it, and it would spawn a helper of its own, which would do the same.  Each
 * generation arms a fresh watchdog, so the recursion sustains itself instead of
 * expiring at any deadline.  The refusal is therefore keyed on presence, which
 * holds for any value the parent might have written.
 */
static int test_opal_helper_marker_present(void)
{
	return getenv(TEST_OPAL_HELPER_ENV) != NULL ? 1 : 0;
}

/*
 * The SECOND credential: proof that the marker was written by the parent of this
 * very run, and not inherited, exported by hand or left behind by another tool.
 *
 * WHAT IS CHECKED, AND WHY EACH STEP IS THERE
 * -------------------------------------------
 * All three names must be present and non-empty, and the mode must match exactly,
 * so a partial or mistyped environment is refused rather than half-believed.
 *
 * The descriptor number is parsed with strtol() and the WHOLE string must be
 * consumed, so "9x" and " 9" are rejected rather than silently read as 9.  It
 * must also be above standard error and below a plausible ceiling: a handshake
 * conducted over one of the three standard descriptors would be reading the
 * suite's own console, and a wild value has no business reaching fstat().
 *
 * fstat() plus S_ISFIFO() then establishes that the number really names a pipe.
 * A regular file, a socket or a terminal that merely happens to be open at that
 * number is refused.  Note that this path does NOT close the descriptor: a
 * descriptor that is not the pipe this suite created is not this suite's to close.
 *
 * Finally the record itself is read and compared BYTE FOR BYTE against the tag,
 * the mode and the token rendered in the same order the parent renders them.  The
 * read is bounded by poll() so a pipe that carries nothing cannot wedge the helper,
 * and it asks for one byte MORE than the expected record: a longer record fails
 * the exact-length test instead of matching on its prefix.  Because the parent
 * closes the write end before forking, end-of-file arrives as soon as the record
 * has been consumed, so the normal case terminates on data rather than on time.
 *
 * A token from a different run does not match.  A marker with no pipe behind it
 * has nothing to match against.  That is the whole property being bought.
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

	/* The array only; argv[0] aliases path, which is freed just below. */
	if (plan->argv) {
		free(plan->argv);
		plan->argv = NULL;
	}

	switch_safe_free(plan->path);
}

/*
 * Build the plan.  Returns 1 with every member owned by *plan, or 0 with nothing
 * owned and nothing leaked.
 *
 * The environment is copied entry by entry, DROPPING every one of the three names
 * this suite owns whatever its value, so neither the marker nor either half of the
 * provenance credential can be inherited by accident or be stale, and this run's
 * own triple is appended last.  Dropping all three rather than only the marker
 * matters: a descriptor number and a token surviving from two different runs would
 * be exactly the situation the handshake exists to rule out.  Every allocation is
 * checked, because this runs under the CI static analyser as well as the sanitizer.
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
 * Runs in a freshly exec'd process with a core of its own, returns the exit code
 * the parent will decode, and touches no state the parent can observe.  Because
 * the helper is a real bootstrap rather than a duplicated image, everything here
 * is ordinary code against a healthy process - there is no inherited-lock hazard
 * to reason about and no restriction on what it may call.
 *
 * The three preconditions are the same ones the in-process version asserted, in
 * the same order, each with its own exit code so a precondition failure cannot be
 * mistaken for the substantive assertion failing.
 *
 * The branch is driven TWICE over the same manager.  Once would only show that the
 * status is reported; twice makes the repeatability of the failure path the property
 * under observation, and it is the cheapest proof that nothing on that path is
 * consumed on first use.  The two verdicts carry distinct exit codes so the parent's
 * log names which of the two diverged.
 *
 * The FSManager is scoped so it is destroyed before the status is judged, exactly
 * as before.  The PProcess is deliberately NOT released: the address space is
 * about to be discarded wholesale, and running PTLib's global teardown here would
 * add risk without adding information.  The event that production leaks on this
 * branch is likewise left alone - confining it is the entire purpose of this
 * process.
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

		/* Same manager, same absent configuration: the branch is driven a SECOND time
		 * so the failure path is exercised as a repeatable property rather than once.
		 * Nothing is set before the failure return, so the call is idempotent by
		 * construction, and a second identical verdict is what proves it. */
		repeated = manager.ReadConfig(false);
	}

	if (status != SWITCH_STATUS_FALSE) {
		return TEST_OPAL_CHILD_NOT_FALSE;
	}

	return repeated == SWITCH_STATUS_FALSE ? TEST_OPAL_CHILD_OK : TEST_OPAL_CHILD_NOT_FALSE_AGAIN;
}

/*
 * Spawn the helper, wait for it under a bounded deadline, and report how it ended.
 *
 * Returns SWITCH_STATUS_SUCCESS with *code set when the helper exited normally,
 * SWITCH_STATUS_TIMEOUT when it had to be killed at the deadline, and
 * SWITCH_STATUS_FALSE when the fork or the wait failed or the helper died on a
 * signal - in which case *sig names the signal.
 *
 * *helper_pid is reported so the caller can address the working directory the
 * helper's own core created for itself, which is named after that pid.  It is set
 * as soon as the fork succeeds, so it is available on the failure paths too - and
 * those are precisely the paths on which that directory must be PRESERVED rather
 * than removed, because it holds the helper's own account of what went wrong.
 *
 * The deadline exists so that no failure mode of the helper can hang the suite:
 * one that wedges before its alarm can fire, or that inherited an ignored
 * SIGALRM, is killed and reaped here.
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
	 * Written, and the write end CLOSED, BEFORE the fork.  Both halves of that
	 * matter.  Writing first means the record is already in the pipe buffer when the
	 * new image starts, so the helper never waits on this process and the two need
	 * no ordering between them at all.  Closing the write end first means the helper
	 * sees end-of-file the instant it has consumed the record, so its read
	 * terminates on data rather than on a timeout, and a record longer than expected
	 * is impossible rather than merely unlikely.  The record is under a hundred
	 * bytes against a pipe buffer of at least 4 KiB, so this write cannot block.
	 */
	if (record_len <= 0 || record_len >= (int) sizeof(record)
		|| !test_opal_write_all(handshake[1], record, (switch_size_t) record_len)) {
		close(handshake[0]);
		close(handshake[1]);
		return SWITCH_STATUS_FALSE;
	}

	close(handshake[1]);
	handshake[1] = -1;

	/* Built BEFORE the fork: the child must not need the allocator. */
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
		 * Exactly four call sites and no others, all on POSIX's async-signal-safe
		 * list: dup2(), alarm() and execve() on the path that works, plus _exit()
		 * only where execve failed - so three of the four run on a successful spawn.
		 * Nothing here allocates, locks, logs or constructs, which is the entire
		 * reason this fork is safe in a process whose core threads are already
		 * running.  The descriptor dup2() needs was opened by the parent before the
		 * fork, so the child never calls open() either.
		 *
		 * Only STANDARD OUTPUT is discarded, and only because the helper runs the
		 * same FCTX driver, so its console output would otherwise interleave with
		 * this run's and corrupt the collected result - concretely, the helper would
		 * print this very case's name a second time.  Standard error is left alone:
		 * that is where FST_CORE_BEGIN writes when a core fails to come up at all
		 * (switch_test.h:296-298), and losing it would turn a diagnosable failure
		 * into a bare exit code.
		 *
		 * The alarm is armed before the exec on purpose: a pending alarm survives an
		 * exec, so it covers the helper's own bootstrap as well as its body.
		 *
		 * _exit(), never exit(): if the exec fails, no inherited atexit handler
		 * and no inherited stdio buffer may run in this duplicated image.
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
		devnull = -1;
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
		/* Backstop: kill and reap, so no zombie and no orphan survive the case. */
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
 * WHY THERE IS ANYTHING TO REMOVE
 * ------------------------------
 * FST_CORE_BEGIN derives its log and database directories from the test base
 * directory plus the running process's pid (switch_test.h:98-104), so every core
 * bootstrap makes a directory named after its own pid.  The helper is a real
 * bootstrap in a real process, so it makes one too - a SECOND directory alongside
 * the one this run already owns, named after a pid that will never recur.
 *
 * Left alone, that is one directory of residue per run, accumulating forever in a
 * source tree, for a process that exited before the case even finished asserting.
 * The suite that caused it is the only party that knows it is disposable, so the
 * suite removes it.
 *
 * WHAT IS REMOVED, AND ONLY THAT
 * ------------------------------
 * The named artefacts the core writes there, and then the directory itself.
 * rmdir() refuses a non-empty directory, so anything this cleanup does not
 * enumerate keeps the directory alive and VISIBLE rather than being deleted
 * unseen - which is the right behaviour towards a tree the test did not author.
 *
 * EVERY REMOVAL IS CHECKED, AND THE VERDICT IS RETURNED
 * ----------------------------------------------------
 * Discarding these results would make the residue claim above an intention rather
 * than a property: a cleanup that silently failed would leave a directory behind
 * every run, the count would creep, and the suite would still report a clean pass.
 * So each removal is checked, ONLY absence is tolerated - ENOENT means the
 * artefact was never written, which is a legitimate outcome for the ".tmp" sibling
 * in particular - and anything else is logged with the exact path and the errno
 * text before the aggregate verdict comes back to the caller, which asserts on it.
 *
 * ONLY AFTER A CLEAN HELPER RUN
 * -----------------------------
 * The caller removes nothing when the helper failed, timed out or died on a
 * signal.  On those paths the directory is the helper's own account of what
 * happened, and it is named in the diagnostic instead of being deleted.
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

	/* The directory itself last, for the reason given above. */
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
 * is load bearing.  All eight, in the order they are declared:
 *
 *   1. config_absent_read_config_fails - runs first, so its verdict cannot be an
 *      artefact of a successful parse or load having left module state behind.
 *      It stands alone in the ordering: the containment it needs is installed by
 *      the setup hook, which runs ahead of every case body, and it asserts both
 *      containment properties itself as fatal preconditions;
 *   2. toolkit_side_effects_are_contained - follows immediately and examines
 *      those same properties in detail, ahead of every other case, so the IAX2
 *      endpoint is observed at the earliest moment it could matter;
 *   3. dual_endpoint_construction - constructs a manager and looks its H.323 and
 *      IAX2 endpoints up through the inherited public accessor.  It calls
 *      neither ReadConfig() nor Initialise();
 *   4. listener_default_signalling_port - the only case needing no manager and
 *      no PProcess at all: it constructs a bare FSListener and reads back the
 *      port its constructor supplied;
 *   5. settings_from_injected_configuration - constructs a manager and calls
 *      ReadConfig() once against the registered provider, then reads the four
 *      public settings accessors back;
 *   6. listener_name_defaults_to_unnamed - constructs one manager per
 *      configuration document, two documents in all, calls ReadConfig() once on
 *      each, and observes both stored listener names through captured log output;
 *
 *      cases 3 to 6 all precede the load case, and none of them calls
 *      FSManager::Initialise().  So every case up to this point observes an OPAL
 *      media-format registry that no Initialise() has mutated - that registry is
 *      process global and Initialise() adds to it - even though cases 5 and 6 do
 *      each drive ReadConfig(), which is socket free and touches no registry;
 *
 *   7. module_load_and_endpoint_interface - the first and only case that calls
 *      Initialise(), and therefore the only case with a listener thread.  It
 *      takes the PTLib process singleton over from the suite and deliberately
 *      leaves the module loaded;
 *   8. module_shutdown_succeeds - declared last so it observes a fully
 *      initialised module and its result is asserted rather than discarded, and
 *      so that its two extra duties come last: proving the case before it
 *      orphaned no configuration provider, and running the suite's unconditional
 *      sweep.
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
			 * case that constructs an FSManager fatally requires
			 * test_opal_iax2_containment_in_effect(), which re-establishes the
			 * containment live, and test_opal_acquire_process() returns NULL
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
		 * Case 1 - configuration absent.  DECLARED FIRST.
		 *
		 * ReadConfig() is asserted directly rather than through
		 * mod_opal_load(), because FSManager::Initialise() discards
		 * ReadConfig()'s status: a load would still report success and the
		 * error branch would go unobserved.  No binding is registered, so the
		 * fixture root's deliberate omission of the module's configuration
		 * section is what makes the miss deterministic.
		 *
		 * DECLARED FIRST so its verdict cannot be an artefact of anything that
		 * ran before it: no configuration has been injected, no successful parse
		 * has happened and no module has been loaded.  It is self-sufficient in
		 * the ordering as well - both containment properties are established by
		 * the setup hook, which FCTX runs ahead of every case body, and both are
		 * asserted here as fatal preconditions rather than inherited from the
		 * case that examines them in detail.
		 *
		 * THIS IS THE ONE CASE THAT CANNOT RUN IN THE PRINCIPAL PROCESS, and the
		 * reason is a defect in production this harness deliberately does not
		 * repair: FSManager::ReadConfig() creates a request-parameters event
		 * unconditionally and destroys it only on the success path, so every time
		 * this branch runs it orphans one malloc-backed switch_event_t.  The module
		 * source, its header and its shipped configuration are frozen - the plan
		 * lists mod_opal.cpp under "Source that must not change" and puts the
		 * engagement's production-edit budget at zero - and the leak cannot be
		 * closed from the harness either, because this target links a separate
		 * compilation of the module (libmodopal.la, see Makefile.am) rather than
		 * including it, so no preprocessor seam declared here reaches that
		 * translation unit.
		 *
		 * The defect is therefore CONTAINED rather than corrected.  The whole
		 * assertion set runs in a freshly exec'd image of this same binary, which
		 * _exit()s: the address space that made the allocation is discarded without
		 * an at-exit leak check, so the principal process - the one the CI job
		 * observes under the address sanitizer - never traverses the branch at all.
		 * No suppression file, no detect_leaks=0 and no ASAN_OPTIONS edit is
		 * involved, and the leak is still fully visible to anyone who runs the
		 * helper directly.
		 *
		 * Inside that helper the branch is driven TWICE over the same manager, so
		 * the repeatability of the failure verdict is the property under
		 * observation rather than a single sample.  Nothing is set before the
		 * failure return, so the call is idempotent by construction.
		 */
		FST_TEST_BEGIN(config_absent_read_config_fails)
		{
			switch_status_t isolated = SWITCH_STATUS_FALSE;
			switch_status_t cleaned = SWITCH_STATUS_SUCCESS;
			pid_t helper_pid = -1;
			int child_code = -1;
			int child_signal = 0;

			/*
			 * ---------------------------------------------------------------
			 * THE HELPER'S OWN ENTRY POINT.
			 * ---------------------------------------------------------------
			 *
			 * When this process IS the exec'd helper, the whole of its work is the
			 * body below and its whole result is an exit code.  It is placed in the
			 * FIRST declared case because FCTX runs cases in declaration order, so
			 * _exit()ing here guarantees no later case ever runs in the helper: the
			 * helper cannot load the module, cannot take over the PTLib process
			 * singleton, and cannot report a verdict of its own into the parent's
			 * tally.
			 *
			 * _exit() rather than return, deliberately: returning would run
			 * FST_CORE_END's switch_core_destroy() and then FCTX's final report, and
			 * the helper has no business tearing a core down or printing a summary
			 * the parent will print properly a moment later.  It is also what keeps
			 * the confinement total - no atexit handler and no leak-sanitizer
			 * at-exit check fires in the one process that deliberately traverses
			 * production's leaking branch.
			 *
			 * This is reached AFTER the setup hook, so the plugin pin and the IAX2
			 * containment are already installed exactly as they are for any other
			 * case, and the body re-verifies both before relying on them.
			 *
			 * TWO CREDENTIALS ARE REQUIRED, NOT ONE.  The marker names the mode; the
			 * provenance handshake proves the marker came from the parent of THIS
			 * run.  Only both together dispatch the helper body, because helper mode
			 * _exit()s from inside this first case: a top-level run that entered it on
			 * the strength of an inherited or stale marker alone would run one case,
			 * exit with that case's status, and be recorded as a clean pass with the
			 * seven later cases silently never run.
			 *
			 * A MARKER WITHOUT VALID PROVENANCE IS THEREFORE A HARD REFUSAL, not a
			 * fallback to an ordinary run.  Continuing as an ordinary run would be the
			 * friendlier-looking choice and the wrong one: the marker's presence also
			 * disarms the spawn below (test_opal_run_readconfig_isolated refuses to
			 * nest on presence alone), so this case could not do its work anyway, and
			 * the environment it found itself in is one nothing should silently
			 * tolerate.  Exiting with a DISTINCT NON-ZERO code makes the
			 * misconfiguration impossible to miss and impossible to mistake for any
			 * other outcome; 58 is outside the range a signal or a libc failure
			 * produces and is neither of automake's reserved 77 (skip) or 99
			 * (framework error).
			 *
			 * The diagnostic goes to STANDARD ERROR rather than through the core
			 * logger, for the same reason FST_CORE_BEGIN reports a failed core there
			 * (switch_test.h:296-298): the process is about to _exit(), so a message
			 * queued for the logging thread might never be written, whereas stderr is
			 * flushed here and is the one stream the helper path never redirects.
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

			/* Fatal preconditions, asserted in the PARENT before it spawns anything.
			 *
			 * The child inherits this environment, and it is the process that brings
			 * a PProcess up and constructs the manager, so an unpinned or uncontained
			 * parent would hand those hazards straight to it.  Asserted per case, not
			 * just once, so no re-ordering can leave a manager built without either. */
			fst_requires(test_opal_plugin_path_is_pinned());
			fst_requires(test_opal_iax2_containment_in_effect());

			/*
			 * NOTE what is deliberately NOT done here: the parent does not acquire a
			 * PProcess and does not construct an FSManager.  Constructing the manager
			 * is what traverses production's leaking branch, so doing it here would
			 * put the leak in the principal process and fail the sanitizer.  Every
			 * one of those steps happens in the helper instead, and the parent's role
			 * is reduced to spawning it and decoding its verdict.
			 */

			/* argv[0] is FCTX's own main() parameter and is the fallback image path;
			 * /proc/self/exe is preferred where it exists.  Passing it in rather than
			 * reaching for a global keeps the spawn helper free of hidden inputs. */
			isolated = test_opal_run_readconfig_isolated(argv[0], &child_code, &child_signal, &helper_pid);

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

			/* The substantive assertion: ReadConfig() reported SWITCH_STATUS_FALSE
			 * when no opal.conf could be located.  The distinct exit codes make an
			 * unexpected value name its own failure mode in the log line above. */
			fst_xcheck(child_code == TEST_OPAL_CHILD_OK,
					   "ReadConfig() must report SWITCH_STATUS_FALSE when no opal.conf can be located");

			/* RESIDUE NEUTRALITY, asserted rather than assumed.  A clean helper run
			 * must leave this suite with exactly the one pid-named working directory a
			 * single-core suite leaves; test_opal_remove_helper_dir() has already
			 * logged the offending path and errno for anything it could not remove. */
			fst_xcheck(cleaned == SWITCH_STATUS_SUCCESS,
					   "the helper's pid-named working directory must be removed in full after a clean isolated run");

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
