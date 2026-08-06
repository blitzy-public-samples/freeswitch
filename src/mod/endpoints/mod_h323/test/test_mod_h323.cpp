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
 * HOW TO READ THE mod_h323.cpp LINE NUMBERS IN THIS FILE
 * -----------------------------------------------------
 * Every `mod_h323.cpp:<line>' anchor below numbers the module as it stood BEFORE the
 * OOS-9 co-load guard was added, and the guard shifted the file by +9 to +94 depending
 * on where in it you are.  The anchors were deliberately left on the pre-guard numbering
 * rather than mass-rewritten, so translate them through the piecewise mapping tabulated
 * in blitzy/documentation/oos9-coload-determination.md, "Reading the line numbers".  The
 * two most-cited: mod_h323.cpp:157, the load function's entry log line, is :234 in the
 * guarded tree, and mod_h323.cpp:167, where the FSProcess is constructed, is :249.
 *
 * HARNESS ARCHITECTURE
 * --------------------
 * This suite observes mod_h323; it does not alter it.  No line of mod_h323.h or
 * h323.conf.xml is modified, and the only change to mod_h323.cpp is the OOS-9 co-load
 * guard, which this suite asserts rather than introduces.
 *
 * Subjects: the two module entry points, which are the complete entry-point set --
 * SWITCH_MODULE_DEFINITION(mod_h323, mod_h323_load, mod_h323_shutdown, NULL) passes
 * NULL as its runtime argument, so the module declares no switch_module_runtime --
 * plus FSH323EndPoint::ReadConfig() and FSH323EndPoint::Initialise() driven directly.
 *
 * ONE DEFINITION, ONE PRODUCTION COPY
 * -----------------------------------
 * The module translation unit is compiled into this one because a convenience library
 * cannot link against it.  mod_h323.h declares FSH323_T38Capability::CreateChannel
 * inside the class (mod_h323.h:568-572) but defines it at namespace scope
 * (mod_h323.h:588-601) without `inline', so every translation unit including the
 * header emits a strong definition, and Initialise() constructs
 * FSH323_T38Capability, which always pulls in the archive member carrying the second
 * one.  The header also instantiates H323_REGISTER_CAPABILITY nine times at namespace
 * scope (mod_h323.h:620-628), so two translation units would register the same
 * capability names into the global H323CapabilityFactory twice.  This program
 * therefore has one source file, and the module must not also arrive through a
 * convenience library or _LDADD: two live copies of `h323_process' and a
 * doubly-populated capability factory would be a silent failure where the link error
 * is a loud one.
 *
 * VERDICTS COME FROM PUBLIC SEAMS
 * -------------------------------
 * Single-translation-unit compilation makes the header-static `h323_process'
 * (mod_h323.h:630) and the file-static `mod_h323_globals' (mod_h323.cpp:42)
 * reachable, but no verdict rests on either, because an assertion on internal linkage
 * breaks on refactors that change nothing observable.  Verdicts come from
 * ReadConfig()'s returned switch_status_t, the public m_listeners list
 * (mod_h323.h:270), the public m_ai and m_pi members (mod_h323.h:271-272), protected
 * members reached through the test-local subclass FSH323TestEndPoint -- no friend
 * declaration, nothing de-staticised -- PTLib's PProcess::IsInitialised() and
 * PProcess::Current(), FSProcess::GetH323EndPoint(), GetSwitchInterface(), and the
 * module interface the load function returns.
 *
 * BOOTSTRAP TIER
 * --------------
 * FST_CORE_BEGIN + FST_SUITE_BEGIN.  A real core is mandatory because
 * switch_xml_open_cfg() asserts MAIN_XML_ROOT != NULL and ReadConfig() reaches it.
 * The module-loading tier (switch_test.h:346-364) is unusable: it dlopens the module
 * from <confdir>/../.libs/, which would load a second copy of code already linked in,
 * and its end macro unloads without asserting, whereas this suite asserts the
 * shutdown status.  Nothing here ever dlopens or dlcloses.
 *
 * PProcess SINGLETON DISCIPLINE
 * -----------------------------
 * PTLib allows at most one live PProcess-derived object per process, and
 * H323EndPoint's constructor calls PProcess::Current().GetUserName()
 * (h323ep.cxx:713), which _exit(1)s when none exists.  A single FSProcess is
 * therefore shared by the cases that construct an endpoint and released by the last
 * of them; the irreversible PTLib property behind that is stated beside the ownership
 * helpers below.
 *
 * NETWORK SIDE-EFFECT CONTAINMENT
 * -------------------------------
 * Initialise() starts a listener per configured address (mod_h323.cpp:441-448) and,
 * when gk-address is non-empty, a live RAS registration thread (mod_h323.cpp:451-455);
 * an empty listener list makes it fall back to StartListener(""), a wildcard bind on
 * port 1720.  Containment has three independent layers, so no single mistake leaks a
 * socket:
 *
 *   1. Both StartListener() overloads and UseGatekeeper() are retargeted onto
 *      file-static doubles that record the request and perform no I/O.  Every case
 *      that runs Initialise() then asserts the endpoint's listener list is empty.
 *   2. Every injected document pins a listener to 127.0.0.1 on a fixed high port, so
 *      the wildcard fallback is unreachable regardless; the shipped sample's
 *      $${local_ip_v4} and port 1720 (h323.conf.xml:23-28) are reference values only.
 *   3. No configuration with a non-empty gk-address ever reaches Initialise().
 *      StartGkClient()'s early return clears m_stop_gk but leaves m_thread pointing at
 *      a self-deleted thread, and ~FSH323EndPoint -> StopGkClient() then spins forever
 *      waiting on it (mod_h323.cpp:671-674 against :693-701).  The LAN-search case
 *      drives StartGkClient() directly, reaching the same decision with no thread.
 *
 * No case performs third-party network I/O, opens a random port, binds any port, or
 * depends on wall-clock time.
 *
 * RESOURCE RECLAMATION
 * --------------------
 * CI configures the address sanitizer with leak detection live, so a leak is a build
 * failure.  The unstarted H323ListenerTCP objects ReadConfig() produces are owned by
 * the FSListener records in m_listeners and freed from there -- the StartListener()
 * double adopts nothing, so there is one owner and one release path whether a case
 * reached Initialise() or stopped at ReadConfig().  The root pool ReadConfig()
 * allocates and abandons (mod_h323.cpp:469) is captured by a preprocessor seam scoped
 * to the module translation unit and released from the per-case teardown.  The
 * module's retained state -- provider registration, PProcess, module-lifetime pool --
 * is handed forward from case to case and swept by the shutdown case, see
 * fst_h323_suite_state_cleanup().
 */

#include <switch.h>
#include <test/switch_test.h>

/*
 * HERMETIC TOOLKIT INTERPOSITION
 * ------------------------------
 * Three production decisions can only be observed by EXECUTING them, and each
 * reaches the H.323 toolkit when it runs: Initialise() hands every configured
 * listener to StartListener(), which opens a TCP socket and takes ownership on
 * success (h323ep.h:519-545); Initialise() resumes a live FSGkRegThread whenever
 * gk-address is non-empty; and StartGkClient() asks the toolkit to register with, or
 * search the LAN for, a gatekeeper through UseGatekeeper(), which is live RAS
 * signalling.  Asserting those decisions from stored configuration alone would prove
 * only that ReadConfig() copied a string.  Interposing the two toolkit entry points
 * lets the decisions run for real while nothing leaves the process.
 *
 * Neither H323EndPoint::StartListener nor H323EndPoint::UseGatekeeper is virtual
 * (h323ep.h:532, :547 and :354), so a subclass cannot override either; rewriting the
 * call sites is the only seam available.  It is applied here, immediately before the
 * subject include, and nowhere else in this file.
 *
 * The rewrite is confined by construction.  Exactly one header in this translation
 * unit's include set names either token -- the toolkit's own h323ep.h -- and the
 * <ptlib.h> / <h323.h> pre-include below pulls it in behind its include guard BEFORE
 * either macro exists.  mod_h323.h names neither token, so the only text the macros
 * can reach is mod_h323.cpp's own call sites.  The pre-include reproduces
 * mod_h323.h's visibility bracket (mod_h323.h:38-40 and :55-57) so the toolkit's
 * declarations are seen with the visibility the production build sees them with.
 *
 * mod_h323.cpp and mod_h323.h are compiled from byte-identical text with the module
 * target's own flags; only this file's preprocessor state differs.
 */
#if defined(__GNUC__) && defined(HAVE_VISIBILITY)
#pragma GCC visibility push(default)
#endif

#include <ptlib.h>
#include <h323.h>

#if defined(__GNUC__) && defined(HAVE_VISIBILITY)
#pragma GCC visibility pop
#endif

/* One address slot per listener, because listener ORDER is itself a production
 * behaviour: Initialise() walks m_listeners in document order
 * (mod_h323.cpp:444-448).  Eight is four times the largest listener count any
 * document in this file declares. */
#define FST_H323_MAX_OBSERVED_LISTENERS 8
#define FST_H323_ADDRESS_MAX            128

/*
 * Everything the doubles observe.
 *
 * Single-threaded by construction, so no lock is needed: no case starts a
 * thread, and the one production path that would - the FSGkRegThread at
 * mod_h323.cpp:451-455 - is never reached, because no case calls Initialise()
 * with a non-empty gk-address.  The LAN-search case documents why that is a
 * hard requirement rather than a stylistic choice.
 */
typedef struct {
	int gk_calls;
	char gk_address[FST_H323_ADDRESS_MAX];
	char gk_identifier[FST_H323_ADDRESS_MAX];
	char gk_interface[FST_H323_ADDRESS_MAX];

	int listener_calls;
	int listener_nulls;
	char listener_address[FST_H323_MAX_OBSERVED_LISTENERS][FST_H323_ADDRESS_MAX];

	int listener_default_calls;
	char listener_default_address[FST_H323_ADDRESS_MAX];
} fst_h323_toolkit_t;

static fst_h323_toolkit_t fst_h323_toolkit;

static void fst_h323_toolkit_reset(void)
{
	memset(&fst_h323_toolkit, 0, sizeof(fst_h323_toolkit));
}

/* PString's const char * conversion returns its internal array, which is a
 * one-byte "" for an empty string rather than NULL (PString derives from
 * PCharArray).  The NULL branch is nonetheless kept because switch_copy_string
 * would fault on one, and a double must never be the thing that crashes. */
static void fst_h323_record_string(char *dst, switch_size_t size, const PString & value)
{
	const char *text = (const char *) value;

	switch_copy_string(dst, text ? text : "", size);
}

/*
 * UseGatekeeper double.  Records the request and reports FAILURE, which is the only
 * safe answer: once the retry loop exits normally StartGkClient() dereferences
 * GetGatekeeper() unconditionally (mod_h323.cpp:684-685), and this double registers
 * nothing, so GetGatekeeper() is NULL and TRUE would segfault.  FALSE enters the
 * loop, whose first m_stop_gk check returns cleanly (mod_h323.cpp:671-674) -- before
 * any h_timer() sleep, before RemoveGatekeeper(), and before that dereference.
 *
 * The default arguments cover the second and third parameters only, where the real
 * declaration defaults all three (h323ep.h:354-358).  The asymmetry costs nothing,
 * because the sole call site passes all three arguments (mod_h323.cpp:664): every
 * call shape the production code actually uses still compiles, and an address-less
 * request has nothing for this double to record.
 */
static PBoolean fst_h323_use_gatekeeper(const PString & address, const PString & identifier = PString::Empty(),
										const PString & localAddress = PString::Empty())
{
	fst_h323_toolkit.gk_calls++;

	fst_h323_record_string(fst_h323_toolkit.gk_address, sizeof(fst_h323_toolkit.gk_address), address);
	fst_h323_record_string(fst_h323_toolkit.gk_identifier, sizeof(fst_h323_toolkit.gk_identifier), identifier);
	fst_h323_record_string(fst_h323_toolkit.gk_interface, sizeof(fst_h323_toolkit.gk_interface), localAddress);

	return FALSE;
}

/*
 * StartListener(H323Listener *) double - the overload Initialise() uses for every
 * configured listener.  Records the transport address, opens nothing, and reports
 * success so production's success path is the one under test.
 *
 * OWNERSHIP.  The real overload ADOPTS the object when it returns TRUE
 * (h323ep.h:519-531) by putting it in the endpoint's H323ListenerList.  This double
 * does not, because adopting it would hand it to the toolkit this seam exists to keep
 * out.  Ownership therefore stays where ReadConfig() put it -- in the FSListener
 * record inside the public FSH323EndPoint::m_listeners (mod_h323.h:270), which
 * fst_h323_release_listeners() drains.  One owner and one release path, uniform
 * whether a case reaches Initialise() or stops at ReadConfig(), makes a double free
 * structurally impossible.  The endpoint's own listener list staying empty is the
 * assertion that proves no socket was bound: every case running Initialise() checks
 * GetListeners().GetSize() (h323ep.h:2027) is zero.
 */
static PBoolean fst_h323_start_listener(H323Listener * listener)
{
	int slot = fst_h323_toolkit.listener_calls;

	fst_h323_toolkit.listener_calls++;

	if (!listener) {
		fst_h323_toolkit.listener_nulls++;
		return FALSE;
	}

	if (slot < FST_H323_MAX_OBSERVED_LISTENERS) {
		fst_h323_record_string(fst_h323_toolkit.listener_address[slot],
							   sizeof(fst_h323_toolkit.listener_address[slot]), listener->GetTransportAddress());
	}

	return TRUE;
}

/*
 * StartListener(const H323TransportAddress &) double - the default-interface
 * fallback, taken only when the configuration declares no listener at all
 * (mod_h323.cpp:441-443 passes "", which would listen on 0.0.0.0:1720).  Every
 * document this suite injects declares a listener, so this overload must never be
 * reached and every Initialise() case asserts that; it exists because the macro
 * rewrite retargets both call sites, so both overloads must be defined.
 */
static PBoolean fst_h323_start_listener(const H323TransportAddress & iface)
{
	fst_h323_toolkit.listener_default_calls++;
	fst_h323_record_string(fst_h323_toolkit.listener_default_address, sizeof(fst_h323_toolkit.listener_default_address), iface);

	return TRUE;
}

/*
 * ROOT-POOL RECORDING SEAM
 * ------------------------
 * FSH323EndPoint::ReadConfig() opens with an unconditional
 * `switch_core_new_memory_pool(&pool)` (mod_h323.cpp:469) and never uses the result:
 * `pool` is a local, no pointer derived from it is stored, and no exit path destroys
 * it.  Every case that reads configuration therefore strands one parentless root
 * pool, and CI runs the address sanitizer with leak detection live.
 *
 * The allocation is interceptable at the preprocessor because
 * switch_core_new_memory_pool() is a macro (switch_core.h:633), so no edit to the
 * module and no change to its symbol surface is needed.  The interception is bounded
 * to the region that needs it: defined just above the module include and restored
 * immediately after it, so nothing outside mod_h323.cpp is affected -- including this
 * file's own module-lifetime pool below, which must keep the real allocator because a
 * different owner destroys it.
 *
 * Destroying the recorded pools is safe because nothing escapes them: `pool` is
 * written once, tested once and never read again, and no switch_core_alloc(),
 * switch_core_strdup() or subpool is taken from it anywhere in mod_h323.cpp.  The
 * recorded pointer is the only remaining reference in the process, so releasing it
 * after the call returns cannot invalidate a live reference.
 */
#define FST_H323_MAX_RECORDED_POOLS 64

static switch_memory_pool_t *fst_h323_recorded_pools[FST_H323_MAX_RECORDED_POOLS];
static int fst_h323_recorded_pool_count = 0;
static int fst_h323_recorded_pool_total = 0;
static int fst_h323_recorded_pool_overflow = 0;

/*
 * Stands in for switch_core_new_memory_pool() inside the module translation
 * unit only.  Delegates to the real allocator with the caller's own file,
 * function and line, so the core's pool bookkeeping still attributes the
 * allocation to mod_h323.cpp exactly as it would have, then records the pointer
 * so it can be released once the call that abandoned it has returned.  A
 * failed allocation is passed through untouched and is not recorded.
 */
static switch_status_t fst_h323_record_pool(switch_memory_pool_t **pool, const char *file, const char *func, int line)
{
	switch_status_t status = switch_core_perform_new_memory_pool(pool, file, func, line);

	if (status != SWITCH_STATUS_SUCCESS || pool == NULL || *pool == NULL) {
		return status;
	}

	if (fst_h323_recorded_pool_count < FST_H323_MAX_RECORDED_POOLS) {
		fst_h323_recorded_pools[fst_h323_recorded_pool_count++] = *pool;
		fst_h323_recorded_pool_total++;
	} else {
		/* Latched rather than ignored: an overflow would mean a pool escaped the
		   sweep, which is the one outcome this seam exists to prevent.  The last
		   case asserts this is clear. */
		fst_h323_recorded_pool_overflow = 1;
	}

	return status;
}

/*
 * Release every root pool recorded so far and return how many were released.
 * Called from the teardown block after every case and from the suite's own
 * cleanup helper, so nothing accumulates across cases and nothing survives the
 * run.  Idempotent: with nothing recorded it releases nothing and returns 0.
 */
static int fst_h323_release_recorded_pools(void)
{
	switch_memory_pool_t *pool = NULL;
	int released = 0;

	while (fst_h323_recorded_pool_count > 0) {
		pool = fst_h323_recorded_pools[--fst_h323_recorded_pool_count];
		fst_h323_recorded_pools[fst_h323_recorded_pool_count] = NULL;

		if (pool != NULL) {
			switch_core_destroy_memory_pool(&pool);
			released++;
		}
	}

	return released;
}

#undef switch_core_new_memory_pool
#define switch_core_new_memory_pool(p) fst_h323_record_pool(p, __FILE__, __SWITCH_FUNC__, __LINE__)

/* The rewrite itself.  Deliberately the last thing before the subject is
 * included, and never undefined afterwards: below this point the only route to
 * either toolkit entry point is through the doubles above. */
#define UseGatekeeper fst_h323_use_gatekeeper
#define StartListener fst_h323_start_listener

#include "../mod_h323.cpp"

/* Restored immediately, so every allocation this file makes on its own behalf
   uses the real allocator and is owned by whoever destroys it. */
#undef switch_core_new_memory_pool
#define switch_core_new_memory_pool(p) switch_core_perform_new_memory_pool(p, __FILE__, __SWITCH_FUNC__, __LINE__)

/*
 * POSIX process control, for the isolated execution context described at
 * fst_h323_run_readconfig_isolated() below.  All three are already reached
 * transitively - <unistd.h> through switch_platform.h:123 and <sys/wait.h>
 * through PTLib's unix/ptlib/pmachdep.h - and all three carry include guards,
 * so naming them here adds nothing to the preprocessor and only makes the
 * dependency explicit at the point of use.  They are placed AFTER the three
 * includes above so that the documented include order of this suite - core
 * header, test header, subject - is not disturbed.
 *
 * This suite is POSIX-only by construction, not by omission: mod_h323.2017.vcxproj
 * builds only the module and never names this file, and the test target links
 * -lopenh323 -lpt -lrt.  So there is no Windows path here to guard for.
 */
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/*
 * The O_* flags open() takes.  Named for the same reason as the three above -
 * the isolated context below opens /dev/null in the parent so that the child
 * needs nothing but dup2() before it execs, so the open flags are part of this
 * file's direct dependency set.
 */
#include <fcntl.h>

/*
 * fstat() and S_ISFIFO(), and poll().  Both belong to the helper-provenance
 * handshake described at fst_h323_helper_provenance_ok() below: before the helper
 * believes a descriptor number handed to it in the environment, it establishes
 * that the descriptor is actually a pipe (fstat + S_ISFIFO) and then reads the
 * parent's one-time record from it under a bounded wait (poll), so that no
 * descriptor that merely happens to be open can wedge the helper.
 */
#include <sys/stat.h>
#include <poll.h>

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
 * LeakSanitizer suppressions for the H.323 toolkit's own start-up allocations.
 *
 * libasan declares __lsan_default_suppressions() as a weak symbol and calls it
 * once, before main(), to obtain suppressions that travel WITH the binary
 * rather than with an environment variable.  That property is the whole reason
 * this is a function rather than a file plus LSAN_OPTIONS: this suite is run
 * three different ways - automake's driver during `make check', a direct
 * libtool-wrapper invocation, and tests/unit/test.sh - and only two of those
 * three can be given an environment.  A suppression that applies in one lane
 * and not the others is worse than none, because it makes the verdict depend
 * on how the suite was started.
 *
 * WHAT is suppressed, and why it is not this tree's leak to fix.  PTLib and
 * H323Plus install their device and feature plugin factories from _dl_init
 * static initialisers, i.e. while the dynamic loader is still bringing the
 * shared objects up and before any FreeSWITCH code has run.  Those factory
 * singletons are never torn down, so LSan reports them at every exit, and the
 * whole set is reachable through exactly one frame:
 *
 *   PDevicePluginAdapter<H460_Feature>::CreateFactory     libh323 (144 B / 6)
 *   PDevicePluginAdapter<PNatMethod>::CreateFactory       libpt    (72 B / 3)
 *   PDevicePluginAdapter<PVideoInputDevice>::CreateFactory libpt   (72 B / 3)
 *   PDevicePluginAdapter<PSoundChannel>::CreateFactory    libpt    (48 B / 2)
 *   PDevicePluginAdapter<H224_Handler>::CreateFactory     libh323  (24 B / 1)
 *   PDevicePluginAdapter<PVideoOutputDevice>::CreateFactory libpt  (24 B / 1)
 *
 * The total (384 bytes in 16 allocations) is invariant to which test cases run
 * - filtering the suite down to a single pre-existing case reproduces it byte
 * for byte - which is the measurement that establishes it as pre-main() and
 * toolkit-owned.  Not one frame names a FreeSWITCH artefact.
 *
 * WHY the template is this narrow.  It matches one class template's factory
 * constructor, not a library, not a file and not a namespace, so any leak this
 * suite or mod_h323 itself introduces is still reported and still fails the
 * run.  Suppressing by module (`leak:libpt.so') would have silenced genuine
 * leaks in every PTLib call the module makes, which is precisely the coverage
 * this suite exists to provide.
 *
 * The visibility attribute is load-bearing, not decoration: the tree compiles
 * with -fvisibility=hidden (SWITCH_AM_CXXFLAGS), under which this definition
 * would be absent from the executable's .dynsym, libasan would keep its own
 * empty weak default, and the suppression would silently do nothing while
 * looking correct in the source.  Verify with ASAN_OPTIONS=print_suppressions=1,
 * which lists the template and the bytes it accounted for.
 */
extern "C" __attribute__((visibility("default"))) const char *__lsan_default_suppressions(void)
{
	return "leak:PDevicePluginAdapter\n";
}

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

	/* m_stop_gk is the flag StartGkClient() tests to abandon its retry loop
	 * (mod_h323.cpp:671-674 and :677-680).  The LAN-search case ARMS it before
	 * calling StartGkClient() directly and then asserts that production
	 * CONSUMED it - production clears it on the way out - which is the
	 * observable proving the early-return path was the one taken. */
	void TestSetStopGk(bool stop)
	{
		m_stop_gk = stop;
	}

	bool TestGetStopGk() const
	{
		return m_stop_gk;
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
 * Loopback-only, high-unprivileged listener ports.
 *
 * None of these ports is ever bound: ReadConfig() constructs an H323ListenerTCP
 * without opening it (mod_h323.cpp:583), and every StartListener() call site is
 * retargeted onto fst_h323_start_listener(), which records the requested address and
 * opens nothing.  The suite therefore cannot collide with another process, with a
 * parallel copy of itself, or with a service holding one of these numbers.
 *
 * They are fixed and distinct rather than random because a distinct value per
 * document lets each case assert that the address production asked to listen on is
 * the one ITS document configured and never another case's.
 */
#define FST_H323_PORT_CODEC_PREFS   "21721"
#define FST_H323_PORT_MODULE_LOAD   "21722"
#define FST_H323_PORT_GK_DISABLED   "21723"
#define FST_H323_PORT_GK_LAN        "21724"
#define FST_H323_PORT_LISTENER_ONE  "21725"
#define FST_H323_PORT_LISTENER_TWO  "21726"

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
 * PTLib PROCESS OWNERSHIP
 * -----------------------
 * PTLib permits at most one live PProcess-derived object per process, and
 * H323EndPoint's constructor calls PProcess::Current().GetUserName()
 * (h323ep.cxx:713), which prints "Catastrophic failure" and _exit(1)s when none
 * exists.  Every case that constructs an endpoint therefore needs exactly one live
 * PProcess and never two.
 *
 * A per-case FSProcess is ruled out by a second PTLib property: PProcess::~PProcess()
 * runs PostShutdown() (osutils.cxx:1646-1654), which calls DestroySingletons() on
 * every PFactory and irreversibly empties both the OpalMediaFormat registry and the
 * H323CapabilityFactory key list.  They are never repopulated, because the objects
 * that register them are static and their constructors ran at program start.
 * AddAllCapabilities() (mod_h323.cpp:404) enumerates that factory and validates each
 * match against the media-format registry, so once any PProcess has been destroyed no
 * audio capability can be added again -- and whichever case ran second would observe a
 * hollowed-out module through no fault of the module.
 *
 * So exactly one PProcess-derived object exists in this process for the whole run, and
 * it is the one the MODULE creates: the module-load case constructs that FSProcess
 * (mod_h323.cpp:167) while both registries are still populated, every case between it
 * and the shutdown case adopts it through PTLib's public singleton accessor, and the
 * shutdown case shuts the module down, which is the only thing that destroys it.
 *
 * The configuration-absent branch cannot fit inside that arrangement -- it must be
 * declared first and still needs a PProcess to construct an endpoint against -- so it
 * runs in a separately exec'd helper process, whose PProcess is not a second live one
 * here and whose destruction empties no factory here.  See
 * fst_h323_run_readconfig_isolated().
 *
 * The harness therefore normally owns no process at all: fst_h323_process stays NULL
 * for a healthy run and exists only as the fallback for a run in which the module load
 * did not happen, so the direct-object cases still have a process to work against.
 * The release helper frees only a process this file created, never the module's.
 */
static FSProcess *fst_h323_process = NULL;

/*
 * PLUGIN-SEARCH-PATH CONTAINMENT
 * ------------------------------
 * PTLib resolves its plugin directory from the environment before falling back to the
 * P_DEFAULT_PLUGIN_DIR compiled into libpt: PPluginManager honours PTLIBPLUGINDIR and,
 * for backward compatibility, the older PWLIBPLUGINDIR.  Whichever it settles on it
 * enumerates recursively and dlopen()s every matching shared object, running each one's
 * initialisers, as a side effect of bringing a PProcess up -- before any test code gets
 * a say.
 *
 * mod_h323 never touches either variable, so a suite that let a process come up would
 * inherit whatever the invoking environment said and execute code from it.  A test
 * binary is run by `make check' out of an environment nobody audits, which makes an
 * inherited search path an arbitrary-code-execution seam (CWE-427, CWE-829).  Both
 * variables are therefore pinned, unconditionally and before the first PProcess, to a
 * fixed directory that does not exist; "/no/thanks" is the same value the sibling
 * production module uses.
 *
 * setenv() rather than putenv() because setenv() copies both name and value into
 * storage the C library owns, whereas putenv() retains the caller's buffer -- which
 * makes the common `putenv((char *) "NAME=value")` idiom leave a mutable pointer into
 * read-only memory in the environment, and any write through it undefined behaviour.
 * The overwrite flag is 1 because the point is to replace what the environment said,
 * not to default an unset variable.
 *
 * The pin is applied in two places: the suite setup hook, which FCTX runs before every
 * case body, and the acquire helper below, the only place in this file that constructs
 * a PProcess.  Either alone would do today; both means no re-ordering and no new case
 * can reintroduce the exposure, and it is idempotent.
 *
 * Nothing about the pin is cached.  setenv() can fail -- it returns -1 on an allocation
 * failure or an invalid name -- so a helper that assumed success and remembered it
 * would report containment that does not exist.  The pin is re-applied and re-verified
 * by readback on every call, and the verdict is derived from the environment as it is
 * at that instant rather than from a flag.
 */
#define FST_H323_PLUGIN_DIR "/no/thanks"

/*
 * True when both plugin-directory variables read back as the pinned value.  This
 * is the only definition of "pinned" in this file: it interrogates the
 * environment and believes nothing else.
 */
static int fst_h323_plugin_path_is_pinned(void)
{
	const char *ptlib = getenv("PTLIBPLUGINDIR");
	const char *pwlib = getenv("PWLIBPLUGINDIR");

	return ptlib && pwlib && !strcmp(ptlib, FST_H323_PLUGIN_DIR) && !strcmp(pwlib, FST_H323_PLUGIN_DIR);
}

/*
 * Pin both variables and return whether the pin is VERIFIED in place: 1 only
 * when both setenv() calls reported success AND both variables read back as the
 * pinned directory, 0 otherwise.  A caller that ignores the result gets no
 * guarantee, and a caller that honours it fails closed.
 *
 * Both results are checked rather than discarded, so an allocation failure
 * inside the C library becomes a reported "not pinned" instead of a silent one.
 */
static int fst_h323_pin_plugin_path(void)
{
	if (setenv("PTLIBPLUGINDIR", FST_H323_PLUGIN_DIR, 1) != 0) {
		return 0;
	}

	if (setenv("PWLIBPLUGINDIR", FST_H323_PLUGIN_DIR, 1) != 0) {
		return 0;
	}

	return fst_h323_plugin_path_is_pinned();
}

/*
 * Return the one live PProcess, adopting the module's when the module has been
 * loaded and creating a harness-owned fallback only when it has not - or NULL
 * when the plugin-search-path containment cannot be verified.
 *
 * FAILS CLOSED.  This is the only place in this file that constructs a
 * PProcess, and constructing one is what triggers PTLib's plugin enumeration,
 * so the containment is verified at this instant and NULL is returned if it
 * cannot be.  Returning NULL is what makes the failure visible: every caller
 * reaches this helper through fst_requires(), so an unverifiable pin aborts the
 * case instead of letting it proceed against an unaudited search path.  The
 * check also gates ADOPTION, which does not enumerate plugins by itself; that
 * is deliberate, because it keeps the rule "no PProcess is touched without
 * verified containment" true without a caller having to know which branch it
 * took.
 *
 * Adoption uses PProcess::IsInitialised() and PProcess::Current(), both public,
 * with the POINTER form of dynamic_cast because this target is compiled
 * -fno-exceptions and the pointer form yields NULL on a type mismatch rather
 * than throwing.  Under -fno-exceptions `new` aborts rather than returning NULL,
 * so the fallback's NULL check is belt-and-braces in the same style the
 * production module uses (mod_h323.cpp:169-171).
 */
static FSProcess *fst_h323_process_acquire(void)
{
	if (!fst_h323_pin_plugin_path()) {
		return NULL;
	}

	if (PProcess::IsInitialised()) {
		/* Adopt, never own: the live process belongs to whoever created it,
		 * which in a healthy run is the module. */
		FSProcess *live = dynamic_cast < FSProcess * >(&PProcess::Current());

		if (live) {
			return live;
		}
	}

	if (!fst_h323_process) {
		fst_h323_process = new FSProcess();
	}

	return fst_h323_process;
}

/*
 * Release ONLY a process this file created.  A process created by the module is
 * left strictly alone - mod_h323_shutdown() owns that one (mod_h323.cpp:195-196)
 * - so this can never race the module's own teardown.  Safe to call when nothing
 * is held, which is what makes the suite's final sweep idempotent.
 */
static void fst_h323_process_release(void)
{
	if (fst_h323_process) {
		delete fst_h323_process;
		fst_h323_process = NULL;
	}
}

/*
 * PROCESS-ISOLATED EXECUTION, FOR THE CONFIGURATION-ABSENT CASE ONLY
 * -----------------------------------------------------------------
 * Two requirements collide inside one address space.  The configuration-absent failure
 * branch must be the first declared case, so its verdict cannot be an artefact of what
 * ran earlier; and its subject, FSH323EndPoint::ReadConfig(), needs a live PProcess,
 * because H323EndPoint's constructor calls PProcess::Current(), which terminates the
 * binary outright on an uninitialised process.  Yet PProcess::~PProcess() runs
 * PostShutdown(), which calls DestroySingletons() on every PFactory and irreversibly
 * empties the OpalMediaFormat registry and the H323CapabilityFactory key list, so once
 * any PProcess here has been destroyed no later load can add an audio capability --
 * while leaving a harness-owned PProcess alive instead would make the module's own
 * unconditional `h323_process = new FSProcess()' (mod_h323.cpp:167) a second live
 * PProcess, which PTLib does not permit.  Running that one branch in a separate process
 * image dissolves the collision: the helper brings up its own PProcess and exits, and
 * the parent's first and only PProcess is still the module's.
 *
 * fork() IMMEDIATELY FOLLOWED BY exec()
 * ------------------------------------
 * By the time any case body runs the core is up and this process is multi-threaded.
 * fork() duplicates the address space into a child with one thread, and every lock
 * another thread held at that instant is duplicated held with no owner to release it --
 * including the core's log queue, the pool allocator and PTLib's mutexes, all of which
 * the body needs.  POSIX permits only async-signal-safe functions between fork() and
 * exec in a multi-threaded process, and allocating, logging, parsing XML or
 * constructing a C++ object is none of them.
 *
 * The forked image therefore does nothing but exec.  The block holds four call sites
 * and no others, every one async-signal-safe: dup2(), alarm() and execve() on the path
 * that works, and _exit() where execve fails.  The descriptor dup2() redirects onto
 * standard output was opened by the parent before the fork, so the child needs no
 * open().  Everything execve() needs -- path, argv, envp, the handshake pipe and its
 * token -- is built in the parent for the same reason, and the parent releases that
 * plan on every path including immediately after a successful fork, because the child
 * has its own copy.  execve() then replaces the address space wholesale, discarding
 * every inherited lock, thread state and stdio buffer.
 *
 * HELPER SELECTION AND PROVENANCE
 * -------------------------------
 * The image exec'd is this same binary, re-entered from main().  Helper mode is
 * selected by an environment marker rather than an extra argv entry, because FCTX's
 * command-line parser reads a bare positional argument as a test-name filter and exits
 * on an unrecognised option (switch_fct.h fctkern__cl_parse), so argv is not ours to
 * extend.
 *
 * The marker alone cannot dispatch the helper body.  An environment variable is a
 * public channel anything in this process's ancestry can set and that persists into
 * every descendant, and helper mode _exit()s from inside the first declared case -- so
 * a top-level run that believed a stale marker would run one case, exit with that
 * case's status, and be recorded as a clean pass with the later cases never run.  The
 * marker is therefore paired with a handshake the environment cannot supply: the parent
 * writes a one-time token into an anonymous pipe and closes the write end before
 * forking, and the helper must read that exact record back out of the inherited
 * descriptor before it will act.  See fst_h323_helper_provenance_ok().
 *
 * alarm() is armed before the exec and a pending alarm survives an exec -- it is a
 * per-process timer, not a signal handler -- so the helper inherits a watchdog covering
 * its whole life including its own bootstrap.  The parent's deadline-and-kill is the
 * independent backstop.
 *
 * Two cores in one run do not collide: every FST core derives its log and database
 * directories from its own pid (switch_test.h:105 and :110), and FST_CORE_BEGIN passes
 * no SCF_USE_SQL, so neither opens a core database.  The helper's standard output is
 * discarded because it runs the same FCTX driver and its console output would interleave
 * with this run's; its standard error is kept, because that is where FST_CORE_BEGIN
 * writes when a core fails to come up at all (switch_test.h:296-298).
 *
 * FCTX's assertion state lives in the parent, so an fst_check() evaluated in the helper
 * would be recorded in a counter that dies with it.  The helper makes its checks as
 * plain comparisons and reports one distinguishing exit code per outcome, which the
 * parent turns back into assertions with messages; the codes sit above the range a
 * signal or a libc failure produces, so an unexpected value is unambiguous.  It ends
 * with _exit() and leaves its PProcess standing, because the address space is about to
 * be reclaimed wholesale.
 */
#define FST_H323_CHILD_OK                 0	/* every child-side check held      */
#define FST_H323_CHILD_UNPINNED          40	/* plugin path not verifiably pinned */
#define FST_H323_CHILD_NO_PROCESS        41	/* PProcess did not come up          */
#define FST_H323_CHILD_NO_ENDPOINT       42	/* endpoint construction failed      */
#define FST_H323_CHILD_FIRST_NOT_FALSE   43	/* first ReadConfig() did not fail   */
#define FST_H323_CHILD_FIRST_LISTENERS   44	/* first ReadConfig() left a listener */
#define FST_H323_CHILD_SECOND_NOT_FALSE  45	/* repeat ReadConfig() did not fail  */
#define FST_H323_CHILD_SECOND_LISTENERS  46	/* repeat left a listener            */
#define FST_H323_CHILD_EXEC_FAILED       47
#define FST_H323_CHILD_BAD_PROVENANCE    48

/*
 * The environment marker that selects helper mode, and the one mode this suite defines.
 * A marker carrying any other value is treated as NOT helper mode rather than as a
 * request this binary does not understand.
 *
 * The marker is only half of the credential.  An environment variable is a public
 * channel that anything in this process's ancestry can set and that persists into every
 * descendant, so it cannot distinguish "my parent just exec'd me for this purpose" from
 * a stale exported shell variable -- and helper mode _exit()s from inside the first
 * declared case, so a top-level run that entered it by accident would execute one case
 * body, exit with that body's status, and be recorded as a clean pass.  The other half
 * is a one-time secret the parent writes into an anonymous pipe before it forks, which
 * cannot be present in an environment this suite did not construct.  The two names
 * below carry the pipe's descriptor number and the secret;
 * fst_h323_helper_provenance_ok() checks them.
 */
#define FST_H323_HELPER_ENV              "FST_MOD_H323_ISOLATED_HELPER"
#define FST_H323_HELPER_READCONFIG       "readconfig-missing-config"
#define FST_H323_HELPER_FD_ENV           "FST_MOD_H323_HELPER_FD"
#define FST_H323_HELPER_TOKEN_ENV        "FST_MOD_H323_HELPER_TOKEN"

/*
 * The handshake record's leading field, which makes the record self-describing
 * and versioned: a future mode that needed a different record shape would carry a
 * different tag, and a mismatched tag fails the byte-for-byte comparison rather
 * than being silently reinterpreted.
 */
#define FST_H323_HELPER_RECORD_TAG       "fst-mod-h323-helper/1"

/*
 * How many environment names this run owns outright: the mode marker, the
 * handshake descriptor number and the handshake token.  Named rather than
 * open-coded because the count appears in three places - the envp size
 * calculation, the drop list and the append loop - and they must not drift.
 */
#define FST_H323_EXEC_OWNED_NAMES        3

/*
 * Bounds on the helper's read of that record.  The parent writes the whole record
 * and closes the write end BEFORE forking, so in the intended case the data and
 * the end-of-file are already waiting and the first poll returns immediately.
 * These bounds exist for the unintended case: a descriptor number that happens to
 * name a pipe nobody is writing to must make the helper REFUSE, not block, so the
 * read is a bounded poll loop rather than a blocking read.
 */
#define FST_H323_HELPER_HANDSHAKE_MS     2000
#define FST_H323_HELPER_HANDSHAKE_POLL_MS 20

/*
 * Upper bound on a descriptor number this suite will look at.  A number outside
 * it did not come from a pipe() in this process, and refusing early keeps fstat()
 * away from an arbitrary integer.
 */
#define FST_H323_HELPER_MAX_FD           (1 << 20)

/*
 * The image to exec.  /proc/self/exe is preferred over argv[0] because it is
 * absolute, immune to the working directory, and immune to an argv[0] the
 * invoker rewrote - which matters concretely here, since this binary is normally
 * reached through a libtool wrapper script that does rewrite it.  argv[0] is the
 * portable fallback for a platform without /proc.
 */
#define FST_H323_SELF_EXE                "/proc/self/exe"

/*
 * Helper-side watchdog, and the parent's own deadline.  The parent's is the longer of
 * the two: the alarm armed before the exec is the primary escape and the parent's kill
 * is the backstop.  The backstop is not redundant -- an exec preserves an IGNORED
 * signal disposition, so a SIGALRM another part of the process had already set to be
 * ignored would disarm the alarm silently.
 *
 * Both are generous because the helper performs a full core bootstrap of its own before
 * it reaches the assertions.  Sixty seconds is the safety factor rather than the
 * expectation, and it is only ever paid on a failure.
 */
#define FST_H323_CHILD_ALARM_SECONDS     60
#define FST_H323_CHILD_DEADLINE_MS       90000
#define FST_H323_CHILD_POLL_MS           20

/*
 * Non-zero when THIS process was exec'd as the isolated helper.
 *
 * The value is compared, not merely tested for presence: a marker carrying
 * something else was set by something that is not this suite, and running the
 * helper path on the strength of a name alone would let an unrelated environment
 * variable silently truncate the run to one case.
 */
static int fst_h323_in_helper_mode(void)
{
	const char *mode = getenv(FST_H323_HELPER_ENV);

	return mode && !strcmp(mode, FST_H323_HELPER_READCONFIG) ? 1 : 0;
}

/*
 * Presence of the marker whatever its value, which is a different question from the one
 * above: the exact-value test decides whether this process should RUN the helper body,
 * this one decides whether it may SPAWN one.  Keeping them separate makes recursion
 * impossible unless both are wrong at once.
 *
 * If the value test served both, a marker that failed to match -- a mistyped value, a
 * diverged mode name, a regressed comparison -- would leave a process that is a helper
 * but does not know it, and it would spawn a helper of its own, which would do the same.
 * Each generation arms a fresh watchdog, so the recursion is self-sustaining rather than
 * something the deadline can absorb.
 */
static int fst_h323_helper_marker_present(void)
{
	return getenv(FST_H323_HELPER_ENV) != NULL ? 1 : 0;
}

/*
 * THE HELPER-PROVENANCE HANDSHAKE
 * -------------------------------
 * Answers the question the marker cannot: was this process exec'd as the helper by the
 * parent of THIS run, or does it merely happen to have inherited a name?
 *
 * Before it forks, the parent creates an anonymous pipe, mints a fresh UUID, writes the
 * one-line record "<tag> <mode> <token>" into the write end and closes that end, then
 * passes the read end's descriptor number and the token through envp alongside the
 * marker.  A pipe descriptor is not close-on-exec, so the read end survives the execve;
 * the record and the end-of-file are already in the pipe buffer, so the helper never
 * waits on the parent and no ordering between them matters.
 *
 * An environment can be copied, exported or left stale; a live pipe carrying a value
 * generated moments ago cannot.  For a stale environment to pass, the descriptor number
 * it names would have to be open in this process AND be a pipe AND contain exactly the
 * record naming exactly that token -- and the token is a fresh UUID, so a recorded one
 * is worthless next run.
 *
 * Each check earns its place: all three variables must be present and the marker must
 * match the mode exactly, so a half-updated environment cannot half-authenticate; the
 * descriptor is parsed with strtol and the entire string must be consumed, so "3x" is
 * rejected rather than read as 3; it must be above STDERR_FILENO, or the handshake would
 * be reading the suite's own console; fstat() must succeed and S_ISFIFO() must hold,
 * which is what turns a stale number into a refusal, and on that failure the descriptor
 * is deliberately NOT closed because it is not ours; the read is a bounded poll loop, so
 * a pipe nobody writes to makes the helper refuse rather than hang, and EINTR is
 * retried; the buffer is one byte larger than the expected record, so a longer record
 * overshoots and fails the same length test a shorter one fails; and the comparison is a
 * byte-for-byte memcmp of the exact expected length rather than a prefix test.
 *
 * Called exactly once, from the first declared case, because it CONSUMES the pipe.
 */
static int fst_h323_helper_provenance_ok(void)
{
	const char *mode = getenv(FST_H323_HELPER_ENV);
	const char *fd_text = getenv(FST_H323_HELPER_FD_ENV);
	const char *token = getenv(FST_H323_HELPER_TOKEN_ENV);
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

	if (strcmp(mode, FST_H323_HELPER_READCONFIG)) {
		return 0;
	}

	if (strlen(token) > SWITCH_UUID_FORMATTED_LENGTH) {
		return 0;
	}

	parsed = strtol(fd_text, &parse_end, 10);

	if (!parse_end || *parse_end || parsed <= STDERR_FILENO || parsed > FST_H323_HELPER_MAX_FD) {
		return 0;
	}

	fd = (int) parsed;

	memset(&descriptor, 0, sizeof(descriptor));

	/* Not closed on this path on purpose: a descriptor that is not the pipe this
	 * suite created is not this suite's to close. */
	if (fstat(fd, &descriptor) != 0 || !S_ISFIFO(descriptor.st_mode)) {
		return 0;
	}

	expected_len = switch_snprintf(expected, sizeof(expected), "%s %s %s\n", FST_H323_HELPER_RECORD_TAG, mode, token);
	capacity = expected_len + 1;

	if (expected_len <= 0 || capacity > (int) sizeof(actual)) {
		close(fd);
		return 0;
	}

	while (filled < capacity && waited_ms < FST_H323_HELPER_HANDSHAKE_MS) {
		memset(&waiter, 0, sizeof(waiter));
		waiter.fd = fd;
		waiter.events = POLLIN;

		ready = poll(&waiter, 1, FST_H323_HELPER_HANDSHAKE_POLL_MS);

		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		if (ready == 0) {
			waited_ms += FST_H323_HELPER_HANDSHAKE_POLL_MS;
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
 * Everything execve() needs, built in the parent so that the forked child can
 * reach exec without touching the allocator.
 *
 * argv[0] deliberately ALIASES path rather than owning a second copy, so the
 * release helper below frees the argv ARRAY but never its elements.
 */
typedef struct {
	char *path;
	char **argv;
	char **envp;
} fst_h323_exec_plan_t;

/*
 * Release every allocation the plan owns and leave it empty.
 *
 * Safe on a zeroed plan, on a fully built one, and on a partially built one -
 * which is what lets the builder use it as its own single error path instead of
 * unwinding by hand at each failure point.
 */
static void fst_h323_exec_plan_release(fst_h323_exec_plan_t * plan)
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
 * Format one NAME=VALUE environment entry and hand back an owned copy, or NULL if
 * it will not fit or either side is missing.
 *
 * The length is checked BEFORE the format rather than inferred from the return
 * value afterwards, because switch_snprintf truncates silently: a bound that is
 * tested up front is one a static analyser can follow, and a truncated marker
 * would be worse than no marker at all - it would authenticate nothing while
 * looking exactly like a credential.
 */
static char *fst_h323_env_entry(const char *name, const char *value)
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
 * Returns 1 with every member owned by *plan, or 0 with nothing owned and nothing
 * leaked.
 *
 * The environment is copied entry by entry, dropping every inherited entry whose name is
 * one of the three this run owns -- the mode marker, the handshake descriptor number and
 * the handshake token -- whatever their values, and appending a freshly rendered triple
 * in their place.  Dropping all three rather than only the marker makes the credential
 * atomic: a helper that assembled its credential from two different runs is exactly the
 * situation the handshake exists to rule out.
 */
static int fst_h323_exec_plan_build(fst_h323_exec_plan_t * plan, const char *argv0, const char *mode, int handshake_fd, const char *token)
{
	extern char **environ;
	static const char *const owned[FST_H323_EXEC_OWNED_NAMES] = {
		FST_H323_HELPER_ENV,
		FST_H323_HELPER_FD_ENV,
		FST_H323_HELPER_TOKEN_ENV
	};
	const char *chosen = NULL;
	char *added[FST_H323_EXEC_OWNED_NAMES] = { NULL, NULL, NULL };
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

	if (access(FST_H323_SELF_EXE, X_OK) == 0) {
		chosen = FST_H323_SELF_EXE;
	} else if (argv0 && *argv0 && access(argv0, X_OK) == 0) {
		chosen = argv0;
	} else {
		return 0;
	}

	if (!(plan->path = strdup(chosen))) {
		return 0;
	}

	/* Exactly two slots: the program path and the NULL terminator.  Anything
	 * more would be read by FCTX as a test-name filter. */
	if (!(plan->argv = (char **) calloc(2, sizeof(char *)))) {
		fst_h323_exec_plan_release(plan);
		return 0;
	}

	plan->argv[0] = plan->path;
	plan->argv[1] = NULL;

	for (count = 0; environ && environ[count]; count++) {
		;
	}

	/* +FST_H323_EXEC_OWNED_NAMES for the appended triple, +1 for the NULL
	 * terminator.  The drop loop below can only ever shorten the copy, so this is
	 * an upper bound rather than an exact size. */
	if (!(plan->envp = (char **) calloc(count + FST_H323_EXEC_OWNED_NAMES + 1, sizeof(char *)))) {
		fst_h323_exec_plan_release(plan);
		return 0;
	}

	switch_snprintf(fd_text, sizeof(fd_text), "%d", handshake_fd);

	added[0] = fst_h323_env_entry(FST_H323_HELPER_ENV, mode);
	added[1] = fst_h323_env_entry(FST_H323_HELPER_FD_ENV, fd_text);
	added[2] = fst_h323_env_entry(FST_H323_HELPER_TOKEN_ENV, token);

	for (slot = 0; slot < FST_H323_EXEC_OWNED_NAMES; slot++) {
		if (!added[slot]) {
			goto fail;
		}
	}

	for (i = 0; i < count; i++) {
		drop = 0;

		for (slot = 0; slot < FST_H323_EXEC_OWNED_NAMES; slot++) {
			name_len = strlen(owned[slot]);

			/* Name match only: compare up to the name and require the very next
			 * byte to be the '=', so FST_MOD_H323_HELPER_FD cannot be mistaken for
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
	for (slot = 0; slot < FST_H323_EXEC_OWNED_NAMES; slot++) {
		plan->envp[out++] = added[slot];
		added[slot] = NULL;
	}

	plan->envp[out] = NULL;

	return 1;

  fail:

	for (slot = 0; slot < FST_H323_EXEC_OWNED_NAMES; slot++) {
		switch_safe_free(added[slot]);
	}

	fst_h323_exec_plan_release(plan);

	return 0;
}

/*
 * The whole of the configuration-absent assertion set, evaluated in the HELPER.
 *
 * Runs in a freshly exec'd process with a core of its own, returns the exit code the
 * parent decodes, and touches no state the parent can observe.  The helper is a real
 * bootstrap rather than a duplicated image, so there is no inherited-lock hazard here
 * and no restriction on what this may call.  The endpoint is deleted on every path that
 * constructed it, so the body is clean under a static analyser even though _exit() would
 * have reclaimed it.  The helper's PProcess is deliberately not released: the address
 * space is about to be discarded wholesale.
 */
static int fst_h323_readconfig_child_body(void)
{
	FSH323TestEndPoint *endpoint = NULL;
	int code = FST_H323_CHILD_OK;

	/* The helper receives the parent's environment through the exec's envp, so
	 * the pin arrives already in place; it is re-verified here because this is
	 * the process that is about to bring a PProcess up, and the pin is only ever
	 * believed as read back from the environment. */
	if (!fst_h323_pin_plugin_path()) {
		return FST_H323_CHILD_UNPINNED;
	}

	if (fst_h323_process_acquire() == NULL || !PProcess::IsInitialised()) {
		return FST_H323_CHILD_NO_PROCESS;
	}

	endpoint = new FSH323TestEndPoint();

	if (endpoint == NULL) {
		return FST_H323_CHILD_NO_ENDPOINT;
	}

	/* No binding is registered anywhere in this run, and
	 * test/conf_h323/freeswitch.xml deliberately carries no
	 * <configuration name="h323.conf"> child, so both the binding list and the
	 * static root miss, switch_xml_open_cfg() (mod_h323.cpp:482) returns NULL
	 * and ReadConfig() takes its error branch at mod_h323.cpp:484-487. */
	if (endpoint->ReadConfig(0) != SWITCH_STATUS_FALSE) {
		code = FST_H323_CHILD_FIRST_NOT_FALSE;
	} else if (!endpoint->m_listeners.empty()) {
		/* Nothing was parsed, so no listener may have been appended. */
		code = FST_H323_CHILD_FIRST_LISTENERS;
	} else if (endpoint->ReadConfig(0) != SWITCH_STATUS_FALSE) {
		/* The failure branch is deterministic and leaves no residue, so
		 * repeating it must produce the identical observable result. */
		code = FST_H323_CHILD_SECOND_NOT_FALSE;
	} else if (!endpoint->m_listeners.empty()) {
		code = FST_H323_CHILD_SECOND_LISTENERS;
	}

	delete endpoint;

	return code;
}

/*
 * Write the whole buffer or report failure, retrying on EINTR and on a short
 * write.
 *
 * A partial write would produce a record that fails the helper's byte-for-byte
 * comparison, which is the safe outcome but a confusing one to diagnose, so the
 * loop makes "the record was delivered intact" the only success.
 */
static int fst_h323_write_all(int fd, const char *data, switch_size_t len)
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
 * Run fst_h323_readconfig_child_body() in a separately bootstrapped helper process.
 *
 * Returns SWITCH_STATUS_SUCCESS when the helper exited normally, writing its exit code
 * to *code; SWITCH_STATUS_TIMEOUT when it had to be killed for exceeding the parent's
 * deadline; SWITCH_STATUS_FALSE when the plan could not be built, the fork or the wait
 * failed, or the helper died on a signal, in which case *sig carries it (SIGALRM being
 * the watchdog).  *helper_pid receives the pid whenever one was created, so the caller
 * can identify -- and on a clean run remove -- the helper's pid-named log directory.
 *
 * The wait is a bounded poll rather than a blocking waitpid(), so a helper that wedges
 * before the alarm can fire, or that inherited an ignored SIGALRM, is killed at the
 * deadline and reaped instead of hanging the suite.
 */
static switch_status_t fst_h323_run_readconfig_isolated(const char *argv0, int *code, int *sig, pid_t *helper_pid)
{
	fst_h323_exec_plan_t plan;
	switch_uuid_t uuid;
	char token[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
	char record[SWITCH_UUID_FORMATTED_LENGTH + 128] = "";
	int handshake[2];
	int record_len = 0;
	int devnull = -1;
	pid_t pid = -1;
	pid_t reaped = 0;
	int status = 0;
	int waited_ms = 0;
	switch_status_t result = SWITCH_STATUS_FALSE;

	handshake[0] = -1;
	handshake[1] = -1;

	*code = -1;
	*sig = 0;
	*helper_pid = -1;

	/* NEVER NEST.  A helper that somehow reached this point would spawn a helper
	 * of its own, and so on, each generation arming a fresh watchdog - so the
	 * recursion would sustain itself rather than expire at the deadline.  The
	 * refusal is keyed on the marker's PRESENCE, not on its value, so that it
	 * still holds for a marker this build does not recognise; see
	 * fst_h323_helper_marker_present() for why the two questions are separate. */
	if (fst_h323_helper_marker_present()) {
		return SWITCH_STATUS_FALSE;
	}

	/*
	 * THE PROVENANCE THE HELPER WILL CHECK, created before anything else, because
	 * the exec plan has to carry the descriptor number and the token into the new
	 * image's environment.  See fst_h323_helper_provenance_ok() for what the other
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
								 FST_H323_HELPER_RECORD_TAG, FST_H323_HELPER_READCONFIG, token);

	/*
	 * Written, and the write end closed, BEFORE the fork.  Writing first puts the record
	 * in the pipe buffer before the new image starts, so the helper never waits on this
	 * process.  Closing the write end first means the helper sees end-of-file the instant
	 * it has consumed the record, so its read terminates on data rather than on a
	 * timeout.  The record is under a hundred bytes against a pipe buffer of at least
	 * 4 KiB, so this write cannot block.
	 */
	if (record_len <= 0 || record_len >= (int) sizeof(record)
		|| !fst_h323_write_all(handshake[1], record, (switch_size_t) record_len)) {
		close(handshake[0]);
		close(handshake[1]);
		return SWITCH_STATUS_FALSE;
	}

	close(handshake[1]);
	handshake[1] = -1;

	/* Built BEFORE the fork: the child must not need the allocator. */
	if (!fst_h323_exec_plan_build(&plan, argv0, FST_H323_HELPER_READCONFIG, handshake[0], token)) {
		close(handshake[0]);
		return SWITCH_STATUS_FALSE;
	}

	/* Opened in the PARENT, so that redirecting the helper's console output needs
	 * nothing but dup2() in the child.  A failure here is not fatal: the
	 * redirection is output hygiene, not correctness, and the verdict travels in
	 * the exit code either way. */
	devnull = open("/dev/null", O_WRONLY);

	/* Flush before forking so no buffered parent output can be duplicated into
	 * the child image.  The exec discards those buffers anyway, but flushing
	 * here removes the possibility rather than relying on that detail. */
	fflush(NULL);

	pid = fork();

	if (pid < 0) {
		fst_h323_exec_plan_release(&plan);

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
		 * descriptor dup2() needs was opened by the parent, so the child never calls
		 * open() either.
		 *
		 * Only standard output is discarded, because the helper runs the same FCTX driver
		 * and its console output would interleave with this run's.  Standard error is left
		 * alone: that is where FST_CORE_BEGIN writes when a core fails to come up at all
		 * (switch_test.h:296-298).
		 *
		 * The alarm is armed before the exec because a pending alarm survives an exec, so
		 * it covers the helper's own bootstrap as well as its body.  _exit() rather than
		 * exit(), so no inherited atexit handler or stdio buffer runs in this duplicated
		 * image.
		 */
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
		}

		alarm(FST_H323_CHILD_ALARM_SECONDS);
		execve(plan.path, plan.argv, plan.envp);
		_exit(FST_H323_CHILD_EXEC_FAILED);
	}

	*helper_pid = pid;

	/* Safe the instant the fork returned: the child holds its own copy of this
	 * memory, so releasing the parent's cannot affect the exec. */
	fst_h323_exec_plan_release(&plan);

	if (devnull >= 0) {
		close(devnull);
	}

	/* The child inherited the read end across the exec - a pipe descriptor is not
	 * close-on-exec - so this process has no further use for it.  Closing it here
	 * also means the helper is the only reader, and that this function leaks no
	 * descriptor on any path. */
	close(handshake[0]);
	handshake[0] = -1;

	while (waited_ms < FST_H323_CHILD_DEADLINE_MS) {
		reaped = waitpid(pid, &status, WNOHANG);

		if (reaped == pid) {
			break;
		}

		if (reaped < 0 && errno != EINTR) {
			/* Not something the helper did, and it may well still be running:
			 * terminate and reap before reporting, so no failure path of this
			 * function can leave a process behind.  EINTR is excluded on purpose - a
			 * signal delivered to the PARENT says nothing about the helper, so the
			 * poll below simply resumes. */
			kill(pid, SIGKILL);

			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
				;
			}

			result = SWITCH_STATUS_FALSE;
			goto done;
		}

		/* reaped == 0 (still running), or EINTR (a signal interrupted the poll,
		 * which is not information about the helper): keep waiting. */
		switch_yield(FST_H323_CHILD_POLL_MS * 1000);
		waited_ms += FST_H323_CHILD_POLL_MS;
	}

	if (reaped != pid) {
		/* Backstop: kill and reap, so no zombie and no orphan survive the case.  The
		 * blocking reap retries on EINTR for the same reason the poll above tolerates
		 * it: a signal delivered here would otherwise abandon the corpse. */
		kill(pid, SIGKILL);

		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			;
		}

		result = SWITCH_STATUS_TIMEOUT;
		goto done;
	}

	if (WIFEXITED(status)) {
		*code = WEXITSTATUS(status);
		result = SWITCH_STATUS_SUCCESS;
		goto done;
	}

	if (WIFSIGNALED(status)) {
		*sig = WTERMSIG(status);
	}

	result = SWITCH_STATUS_FALSE;

  done:

	return result;
}

/*
 * Remove the working directory the helper's own core created.
 *
 * Every FST core composes its log and database directory from its own pid under the
 * test's base directory (switch_test.h:105 and :110), so a second core in the run leaves
 * a second directory behind; removing it means the run leaves exactly the one directory
 * a single-core suite leaves.
 *
 * The contents are known and small: FST_CORE_BEGIN passes no SCF_USE_SQL - that is
 * FST_CORE_DB_BEGIN, the variant this suite does not use (switch_test.h:315-316) - so no
 * database is opened and the only artefact is the preprocessed configuration the XML
 * reader writes as "<conf-file>.fsxml", with a sibling ".tmp" should a write have been
 * interrupted (src/switch_xml.c:1743-1748).  Both names are removed explicitly and then
 * the directory itself, so the cleanup is bounded by name rather than recursive --
 * anything unexpected keeps the directory alive and visible.
 *
 * It runs only when the helper reported success; on any other outcome those files are the
 * primary evidence for what went wrong, so they are preserved and the caller reports
 * where they are.
 *
 * Every removal is checked and the verdict returned, because a cleanup that silently
 * failed would leave a directory behind every run while the suite still reported a clean
 * pass.  Only absence is tolerated -- ENOENT means the artefact was never written, a
 * legitimate outcome for the ".tmp" sibling -- and anything else is logged with the exact
 * path and errno text.
 */
static int fst_h323_remove_helper_path(const char *path, int is_dir)
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

static switch_status_t fst_h323_remove_helper_dir(pid_t helper_pid)
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
	ok &= fst_h323_remove_helper_path(path, 0);

	switch_snprintf(path, sizeof(path), "%s%s%s", dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml.fsxml.tmp");
	ok &= fst_h323_remove_helper_path(path, 0);

	/* The directory itself last.  rmdir() refuses a non-empty directory, so an
	 * artefact this cleanup did not enumerate keeps the directory alive and
	 * visible and is reported here rather than being deleted unseen - which is the
	 * behaviour a test should have towards a tree it did not enumerate. */
	ok &= fst_h323_remove_helper_path(dir, 1);

	return ok ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*
 * Release the H323ListenerTCP objects the test owns, and empty the record list.
 *
 * FSH323EndPoint::ReadConfig() allocates one listener per <listener> element
 * unconditionally -- `listener.listenAddress = new H323ListenerTCP(*this, ip, port)`
 * (mod_h323.cpp:583) -- and stores it as a raw pointer inside the FSListener record it
 * appends to m_listeners.  FSListener has a defaulted constructor that leaves
 * listenAddress uninitialised and no destructor (mod_h323.h:236-243), and
 * ~FSH323EndPoint only calls StopGkClient() and ClearAllCalls()
 * (mod_h323.cpp:612-617), so `delete endpoint` does not free them.
 *
 * It is called after Initialise() too.  H323EndPoint::StartListener(H323Listener *)
 * documents a conditional transfer -- the endpoint deletes the object only if the call
 * returns TRUE (h323ep.h:532-545) -- and that transfer never happens here, because every
 * StartListener() call site is retargeted onto fst_h323_start_listener(), which adopts
 * nothing.  Ownership therefore stays in m_listeners for every case and this helper is
 * the single release path: one owner, one release, so a double free is structurally
 * impossible.  The Initialise() cases pair the call with an assertion that the
 * endpoint's own H323ListenerList (h323ep.h:2027, :3114) is empty, so "the endpoint owns
 * nothing" is proven before anything is freed.
 *
 * The returned count is the ownership verdict: a case asserts the number released equals
 * the number its document declared, which fails if production dropped a record, if a
 * listener was never constructed, or if something else had already taken it.
 *
 * FSH323EndPoint is taken rather than the test subclass because m_listeners is public on
 * the production class (mod_h323.h:270), which lets the module-load case drain the
 * endpoint the MODULE built -- reached through FSProcess::GetH323EndPoint() -- with the
 * same helper the direct-object cases use.
 */
static int fst_h323_release_listeners(FSH323EndPoint * endpoint)
{
	int released = 0;

	if (!endpoint) {
		return 0;
	}

	for (std::list < FSListener >::iterator it = endpoint->m_listeners.begin(); it != endpoint->m_listeners.end(); ++it) {
		if (it->listenAddress) {
			delete it->listenAddress;
			it->listenAddress = NULL;
			released++;
		}
	}

	endpoint->m_listeners.clear();

	return released;
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
 * its lifetime is exactly ONE test case.  The module's lifetime here spans the
 * WHOLE suite: the module-load case loads it and the last asserts that
 * shutting it down works, with every case in between running against a module
 * that is still loaded.  Had that case loaded with fst_pool, that pool
 * would already have been destroyed by the time the second case ran, and
 * mod_h323_shutdown() would eventually have been asked to tear down a module
 * whose interface memory was freed underneath it - a use-after-free across a
 * case boundary, and a hard build failure under the address sanitizer the CI
 * configure line always enables (ci.sh:76-79).
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
 * Total, idempotent reclamation of everything this suite can retain between cases: the
 * registered h323.conf provider, the loaded module's own state, any harness-owned
 * fallback PProcess, and the module-lifetime pool.
 *
 * Every step is idempotent.  switch_xml_unbind_search_function_ptr() reports
 * SWITCH_STATUS_FALSE when no binding carries the pointer and changes nothing.
 * mod_h323_shutdown() frees its four globals through switch_safe_free(), which NULLs
 * each pointer after freeing it, then deletes the process and NULLs that too
 * (mod_h323.cpp:188-199), so it is safe whether the module was loaded, never loaded or
 * already shut down.  The process release and the pool destroy are guarded on their own
 * handles.
 *
 * It is not invoked from FST_TEARDOWN, which runs after every case, because this suite
 * hands state forward on purpose: the module-load case leaves the module loaded, its
 * PProcess alive and its pool alive, because the cases that follow observe a loaded
 * module and the shutdown case asserts that shutting it down works.  Reclamation is
 * anchored instead in a cleanup tail in every case that retains anything, plus this
 * total sweep in the shutdown case.
 *
 * Two framework facts make that sound: a fatal check merely breaks out of the enclosing
 * test body (fct_req expands to `if (!ok) { break; }`, switch_fct.h:3668-3669), and FCTX
 * re-enters the whole fixture-suite body once per declared case, running only the case
 * whose number matches (switch_fct.h:3507-3516).  So a fatal check in one case can skip
 * that case's own tail but never a later case, which is why the sweep case is a dependable
 * safety net for everything declared ahead of it.  The two OOS-9 cases declared AFTER it
 * need no safety net of their own: neither makes a fatal check, so neither can break out
 * early, and each releases in its own body the one thing it plants - a registered
 * stand-in module, or a planted reservation.
 *
 * Two properties of the cases as written bound what such a skipped tail can leave behind,
 * and both hold by construction rather than by convention.  First, no fatal check stands
 * between a SUCCESSFUL bind and its matching unbind: where a bind is itself the subject of
 * a fatal check, that check can only break when the bind failed, and a failed bind
 * registers nothing, so no case can strand a provider for a later one to trip over.
 * Second, the state a fatal check CAN strand is the shared PProcess - the gk-address-empty
 * and codec-prefs cases each acquire it and only then check the module interface fatally -
 * and releasing exactly that is what this sweep is for.  The sweep is always reached,
 * because the case that runs it makes no fatal check at all.
 *
 * Order is load-bearing: unbind the provider first, so nothing that follows can trigger
 * a configuration lookup that re-enters it; shut the module down next, since that is the
 * last thing to read module state carved out of the pool and the only thing that
 * destroys the PProcess the module created; release a harness-owned fallback process
 * AFTER shutdown, so the one-live-PProcess invariant is never breached; destroy the
 * module-lifetime pool once nothing points into it; and release any abandoned root pool
 * last, so it also covers anything the earlier steps caused to be recorded.
 */
static void fst_h323_suite_state_cleanup(void)
{
	/* FALSE simply means nothing was registered, which is the healthy state
	 * here, so the status is deliberately not treated as an error. */
	(void) fst_h323_unbind_config();

	(void) mod_h323_shutdown();

	fst_h323_process_release();

	fst_h323_module_pool_destroy();

	(void) fst_h323_release_recorded_pools();
}


/*
 * OOS-9 CO-LOAD GUARD: THE SIBLING STAND-IN
 * -----------------------------------------
 * mod_h323_load() refuses to load when mod_opal is already in the process, because the
 * two modules link different PTLib runtimes and the second FSProcess constructed
 * SIGSEGVs on the conflicting PProcess singleton (mod_h323.cpp, top of mod_h323_load;
 * evidence in blitzy/documentation/oos9-coload-determination.md).
 *
 * The refusal is asserted through the module-load API against a sibling that is
 * genuinely present, which is what the guard's predicate actually reads:
 * switch_loadable_module_exists() answers from loadable_modules.module_hash
 * (switch_loadable_module.c:1846-1863), and switch_loadable_module_build_dynamic()
 * (switch_loadable_module.h:169) registers a module in that hash under the filename it
 * is handed - the same key switch_loadable_module_process() uses for a dlopen'd module
 * (switch_loadable_module.c:1830).  Registering this two-function stand-in under
 * "mod_opal" therefore makes the sibling present to the guard EXACTLY as the real
 * module would be, while linking no second PTLib into this binary.
 *
 * That distinction is the whole reason the stand-in exists.  Loading the real mod_opal
 * here is precisely the crash the guard prevents: it would map libpt.so.2.12-beta10
 * beside this binary's libpt.so.2.10.9 and kill the test process before it could assert
 * anything.  So the negative branch is asserted hermetically against the predicate, and
 * the end-to-end refusal of the real module - the ERROR line, the surviving process and
 * a still-UP instance - is proven in the runtime evidence archived with the
 * determination (blitzy/documentation/oos9-coload-evidence/), not here.
 *
 * The stand-in is deliberately inert: it creates a module interface, because
 * switch_loadable_module_build_dynamic() dereferences one, and does nothing else.  Its
 * shutdown routine succeeds so that switch_loadable_module_unload_module() can remove
 * it again and hand its pool back.  Both carry C language linkage so their types match
 * switch_module_load_t and switch_module_shutdown_t (switch_types.h:2608-2610) exactly.
 */
#define FST_H323_SIBLING_MODULE "mod_opal"

/*
 * The process-global reservation name the OOS-9 atomic exclusion contends for.
 *
 * ALIASED to the module's own macro rather than re-spelled as a literal, and that is
 * available here only because this suite compiles the production translation unit into
 * itself (#include "../mod_h323.cpp" above).  An alias cannot drift; a second copy of
 * the literal could, and a reservation taken under a name the module does not use would
 * make case 10 below pass while proving nothing.  mod_opal.cpp declares the same name
 * independently, and its own suite - which links rather than includes - has to spell it
 * out; the two module-side definitions must stay identical to each other.
 */
#define FST_H323_PTLIB_RESERVATION H323_PTLIB_RESERVATION

SWITCH_BEGIN_EXTERN_C
static switch_status_t fst_h323_sibling_stub_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, FST_H323_SIBLING_MODULE);

	return *module_interface != NULL ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_MEMERR;
}

static switch_status_t fst_h323_sibling_stub_shutdown(void)
{
	return SWITCH_STATUS_SUCCESS;
}
SWITCH_END_EXTERN_C


/*
 * THE SUITE
 * ---------
 * "conf_h323" names this module's own fixture root.  The core bootstrap builds the
 * configuration directory as SWITCH_TEST_BASE_DIR_FOR_CONF, a path separator, then that
 * name (switch_test.h:92-93), so the target must define -DSWITCH_TEST_BASE_DIR_FOR_CONF
 * and -DSWITCH_TEST_BASE_DIR_OVERRIDE to ${abs_builddir}/test.  Without those defines the
 * bootstrap falls back to "./conf_h323" (switch_test.h:94-99), which resolves against the
 * working directory, so the suite would only find its fixtures when run from inside test/.
 *
 * Case order is load-bearing, because FCTX runs cases in declaration order:
 *
 *   1. readconfig_without_configuration_fails -- first, so the configuration-absent
 *      verdict cannot be an artefact of anything earlier; run in an exec'd helper.
 *   2. module_load_registers_endpoint_interface -- the full load, and the first case to
 *      bring a PProcess up here; leaves the module loaded for everything that follows.
 *   3. gatekeeper_registration_disabled_when_gk_address_empty -- runs Initialise().
 *   4. gatekeeper_lan_search_address_preserved_verbatim -- drives StartGkClient().
 *   5. listeners_parsed_from_configuration -- ReadConfig() only; parsing a listener does
 *      not start one.
 *   6. codec_prefs_negotiation_order -- the last case to run Initialise().
 *   7. module_shutdown_releases_resources -- reclaims what case 2 allocated, and the last
 *      case that touches module lifetime state.
 *   8. coload_reservation_outlives_the_unloaded_module -- immediately after the shutdown
 *      it observes: the OOS-9 claim must survive it, because the PTLib runtime stays
 *      mapped.  This is the property whose absence let `load; unload; load the sibling'
 *      reach the crash.
 *   9. coload_guard_refuses_when_sibling_is_loaded -- the OOS-9 refusal reached through the
 *      module hash, which needs a shut-down module and adds no state for case 7's sweep
 *      to release.
 *  10. coload_reservation_refuses_a_reserved_ptlib_runtime -- last; the OOS-9 refusal
 *      reached through the ATOMIC reservation instead, which is the branch a concurrent
 *      load takes and which the module hash cannot express.
 *
 * Case 1 needs a process of its own rather than merely being declared first, because
 * PProcess::~PProcess() irreversibly empties both PTLib factories.  It cannot avoid
 * bringing a PProcess up -- H323EndPoint's constructor calls PProcess::Current() and
 * terminates the binary when none exists -- and it cannot leave one alive either, because
 * the module's own load unconditionally constructs a second, which PTLib does not permit.
 * That image is a freshly exec'd one rather than a fork, for the async-signal-safety
 * reason set out at fst_h323_run_readconfig_isolated(), and case 1 asserts the property
 * by checking the parent still owns no PProcess afterwards.
 *
 * Cases 3-6 construct endpoints of their own but never a second process: they adopt the
 * live one through fst_h323_process_acquire().  Two H323EndPoint objects may coexist --
 * the class is not a singleton and its constructor binds nothing -- and no port is
 * contended for because none is ever bound.  The distinct port per document lets each
 * case assert the address its own document configured.
 */
FST_CORE_BEGIN("conf_h323")
{
	FST_SUITE_BEGIN(mod_h323)
	{
		/*
		 * This hook must be PRESENT.  FST_CORE_BEGIN sets fst_core == 2, and every
		 * FST_TEST_BEGIN then fatally requires both fst_pool and a started fst_timer
		 * (switch_test.h:451-453), which only FST_SETUP_BEGIN creates
		 * (switch_test.h:407-412).
		 *
		 * Its body pins PTLib's plugin search path, and this is the earliest point
		 * guaranteed to run before any case body: PTLib enumerates and dlopen()s that
		 * directory while a PProcess comes up, so the containment must be installed
		 * before the first construction, whichever case causes it.
		 *
		 * The result is discarded here and only here, because a setup hook runs outside
		 * any test body and a failed check would have nothing to attribute itself to.
		 * The guarantee is enforced where it is attributable:
		 * fst_h323_process_acquire() re-verifies and returns NULL if it cannot, and
		 * every case reaches it through fst_requires().
		 *
		 * It also zeroes the toolkit observation record, so every case sees only the
		 * calls it caused, and doing it here means a case cannot forget or inherit a
		 * count from one that broke out early.
		 */
		FST_SETUP_BEGIN()
		{
			(void) fst_h323_pin_plugin_path();
			fst_h323_toolkit_reset();
		}
		FST_SETUP_END()

		/*
		 * Reclaims exactly one thing: every root pool the module abandoned IN THIS
		 * PROCESS during the case that just ran.  FSH323EndPoint::ReadConfig() allocates
		 * one on entry and never destroys or uses it (mod_h323.cpp:469).
		 *
		 * Six of the ten cases call it, but only five of those calls happen in this
		 * process and reach this hook: cases 2 through 6 read configuration here, while
		 * case 1 reads it twice inside the exec'd helper image, whose pools live and die
		 * in an address space this hook cannot see and that _exit() discards wholesale.
		 * That is why the accounting line logged at the end of a run reports five.
		 *
		 * A recorded pool is the one resource here that no case owns and no later case
		 * reads -- nothing in mod_h323.cpp or in this file holds a pointer derived from
		 * it -- so releasing it after every case is unconditional and cannot demolish
		 * anything.  Being in this hook rather than in the case bodies means it also
		 * happens when a fatal precondition breaks a case out early, which is exactly
		 * when a case's own tail would be skipped.
		 *
		 * Nothing else is reclaimed here.  There is nothing to unload -- the harness
		 * calls the module's entry points by name and never dlopens it -- and no module
		 * state may be released, because this hook runs after every case while the suite
		 * hands the loaded module from the second case to every case after it.
		 * FST_TEARDOWN_BEGIN also destroys fst_pool before this body is entered
		 * (switch_test.h:425-432).
		 *
		 * What makes that safe is a rule the cases keep: no case makes a fatal check
		 * after registering the configuration provider or after creating the
		 * module-lifetime pool, so control always reaches that case's cleanup tail
		 * instead of breaking out to this hook.  A fatal check can still strand a
		 * harness-owned fallback PProcess, which only exists when the module load did
		 * not happen and which the shutdown case's sweep releases, and anything allocated
		 * from fst_pool, which FST_TEARDOWN_BEGIN destroys on the way in.
		 *
		 * The release count is discarded here and only here, because a teardown body
		 * runs outside any test's assertion scope.  The property is asserted where it is
		 * attributable: the case that reads configuration checks the seam recorded a
		 * pool, and the shutdown case checks that none survives and that the
		 * recorder never overflowed.
		 */
		FST_TEARDOWN_BEGIN()
		{
			(void) fst_h323_release_recorded_pools();
		}
		FST_TEARDOWN_END()

		/*
		 * CASE 1 - the configuration-absent failure branch, declared first and run in a
		 * process of its own.
		 *
		 * Asserted on ReadConfig() directly and not on mod_h323_load():
		 * FSH323EndPoint::Initialise() discards ReadConfig()'s status
		 * (mod_h323.cpp:381) and returns TRUE unconditionally (mod_h323.cpp:457), so
		 * mod_h323_load() can never report a configuration failure.
		 *
		 * Declared first so the verdict cannot be an artefact of anything else: no
		 * module is loaded, no configuration provider has been registered, and the
		 * parent has never brought a PProcess up.  Being first and bringing a PProcess
		 * up are mutually exclusive inside one address space, because ~PProcess() would
		 * irreversibly empty the two PTLib factories the module-load case depends on -
		 * see fst_h323_run_readconfig_isolated().  The helper makes the assertions; this
		 * body asserts on the helper's outcome and on the parent state the isolation
		 * protects.
		 *
		 * The subject is an endpoint the helper constructs itself, no provider is
		 * registered, and mod_h323 registers no XML search function, so the lookup
		 * misses in both the binding list and the static root whether or not a module
		 * is loaded - which is what makes the failure branch deterministic.
		 *
		 * Socket-free: ReadConfig() only constructs H323ListenerTCP objects
		 * (mod_h323.cpp:583) and OpenH323 binds in H323ListenerTCP::Open(), reached
		 * from H323EndPoint::StartListener(), which ReadConfig() never calls.  On this
		 * path nothing is even constructed.
		 *
		 * m_pi, m_ai and m_endpointname are not asserted here: their defaults are
		 * applied at mod_h323.cpp:490-493, after the failure return, and the
		 * constructor (mod_h323.cpp:599-610) leaves the two ints uninitialised.  The
		 * context and dialplan defaults at mod_h323.cpp:474-475 are not asserted
		 * either: they live in mod_h323_globals, a .cpp-file static (mod_h323.cpp:42)
		 * whose only setters are file-static (mod_h323.cpp:44-47), so no public seam
		 * exposes them and an assertion would rest on internal linkage alone.
		 */
		FST_TEST_BEGIN(readconfig_without_configuration_fails)
		{
			switch_status_t isolated = SWITCH_STATUS_FALSE;
			switch_status_t cleaned = SWITCH_STATUS_SUCCESS;
			int child_code = -1;
			int child_signal = 0;
			pid_t helper_pid = -1;

			/*
			 * The helper's own entry point.  When this process is the exec'd helper, its
			 * whole work is the body below and its whole result is an exit code.  It sits
			 * in the first declared case because FCTX runs cases in declaration order, so
			 * _exit()ing here guarantees no later case runs in the helper: it cannot load
			 * the module, construct a second PProcess, or report a verdict into the
			 * parent's tally.
			 *
			 * _exit() rather than return, so the helper neither tears a core down through
			 * FST_CORE_END nor prints a summary the parent prints a moment later, and no
			 * atexit or leak-sanitizer at-exit check fires in a process that is mid-suite
			 * by construction.  No alarm is armed here because the parent armed one
			 * immediately before the exec and a pending alarm survives an exec, which
			 * covers this image through its own core bootstrap too.
			 *
			 * Two credentials are required, not one: the marker names the mode and the
			 * provenance handshake proves it came from the parent of this run.  Helper
			 * mode _exit()s from inside this first case, so a top-level run that entered
			 * it on an inherited or stale marker alone would run one case, exit with that
			 * case's status, and be recorded as a clean pass with the nine later cases
			 * never run.
			 *
			 * A marker without valid provenance is therefore a hard refusal rather than a
			 * fallback to an ordinary run: the marker's presence also disarms the spawn
			 * below, since fst_h323_run_readconfig_isolated() refuses to nest on presence
			 * alone, so this case could not do its work either way.  A distinct non-zero
			 * code makes the misconfiguration unmistakable; 48 is outside the range a
			 * signal or libc failure produces and is neither of automake's reserved 77
			 * (skip) or 99 (framework error).
			 *
			 * The diagnostic goes to standard error rather than the core logger, for the
			 * same reason FST_CORE_BEGIN reports a failed core there
			 * (switch_test.h:296-298): the process is about to _exit(), so a message
			 * queued for the logging thread might never be written, whereas stderr is
			 * flushed here and is the one stream the helper path never redirects.
			 */
			if (fst_h323_helper_marker_present()) {
				if (fst_h323_in_helper_mode() && fst_h323_helper_provenance_ok()) {
					fflush(NULL);
					_exit(fst_h323_readconfig_child_body());
				}

				fprintf(stderr,
						"%s is set in this environment but carries no valid parent provenance, so this process "
						"refuses to run as the isolated helper and refuses to continue as an ordinary run. "
						"Unset %s, %s and %s and run the suite again.\n",
						FST_H323_HELPER_ENV, FST_H323_HELPER_ENV, FST_H323_HELPER_FD_ENV, FST_H323_HELPER_TOKEN_ENV);
				fflush(NULL);
				_exit(FST_H323_CHILD_BAD_PROVENANCE);
			}

			/* Preconditions, fatal because nothing after them means anything if they
			 * do not hold.  Nothing has been allocated at this point, so breaking out
			 * here strands nothing.
			 *
			 * The pin is asserted rather than assumed: the helper receives a copy of
			 * this environment and is the process that brings a PProcess up, so an
			 * unpinned parent would hand PTLib an unaudited plugin search path. */
			fst_requires(fst_h323_plugin_path_is_pinned());

			/* THE PARENT MUST OWN NO PProcess YET.  This is what makes the case
			 * genuinely first: it holds only while no earlier case has loaded the
			 * module or constructed a fallback process. */
			fst_requires(!PProcess::IsInitialised());
			fst_requires(fst_h323_process == NULL);

			/* argv[0] is FCTX's own main() parameter (switch_fct.h:3316-3319) and
			 * is the fallback image path; /proc/self/exe is preferred where it
			 * exists.  Passing it in rather than reaching for a global keeps the
			 * spawn helper free of hidden inputs. */
			isolated = fst_h323_run_readconfig_isolated(argv[0], &child_code, &child_signal, &helper_pid);

			if (isolated == SWITCH_STATUS_SUCCESS && child_code == FST_H323_CHILD_OK) {
				/* Clean run: the helper's own pid-named log directory is residue and
				 * nothing more, so it goes - and the verdict is kept, because a
				 * cleanup that silently failed would let residue accumulate one
				 * directory per run while the suite still reported a pass.  On the
				 * failure branch below no cleanup is attempted at all, and `cleaned'
				 * keeps its initial SWITCH_STATUS_SUCCESS so the assertion further
				 * down says nothing about a directory that is being preserved on
				 * purpose. */
				cleaned = fst_h323_remove_helper_dir(helper_pid);
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
			 * deadline; SWITCH_STATUS_FALSE means the spawn or the wait failed, or the
			 * helper died on a signal - SIGALRM being its own watchdog firing. */
			fst_xcheck(isolated == SWITCH_STATUS_SUCCESS,
					   "the isolated configuration-absent check must run to completion in its own process");
			fst_xcheck(child_signal == 0, "the isolated configuration-absent check must not be terminated by a signal");

			/* The substantive assertion: ReadConfig() reported SWITCH_STATUS_FALSE
			 * and appended no listener, twice over.  The distinct exit codes make an
			 * unexpected value name its own failure mode in the log line above. */
			fst_xcheck(child_code == FST_H323_CHILD_OK,
					   "ReadConfig() must report SWITCH_STATUS_FALSE and leave m_listeners empty when no h323.conf can be located");

			/* A clean helper run must leave this suite with exactly the one pid-named
			 * working directory a single-core suite leaves; fst_h323_remove_helper_dir()
			 * has already logged the path and errno for anything it could not remove. */
			fst_xcheck(cleaned == SWITCH_STATUS_SUCCESS,
					   "the helper's pid-named working directory must be removed in full after a clean isolated run");

			/* THE ISOLATION PROPERTY ITSELF.  The parent's PTLib factories are
			 * untouched, so the module load declared next still observes a fully
			 * populated H323CapabilityFactory and OpalMediaFormat registry.  This is
			 * the assertion that makes the isolated design self-checking rather than
			 * merely intended. */
			fst_check(!PProcess::IsInitialised());
			fst_check(fst_h323_process == NULL);
		}
		FST_TEST_END()

		/*
		 * CASE 2 - a configuration-present module load registers the endpoint interface.
		 *
		 * mod_h323_load() is reached by name rather than through a dlopen:
		 * SWITCH_MODULE_LOAD_FUNCTION expands to a plain definition with no storage
		 * class and mod_h323.cpp declares it inside SWITCH_BEGIN_EXTERN_C, so it has C
		 * linkage and external visibility.
		 *
		 * This is the first case to bring a PProcess up in this address space, which is
		 * why the module is observed fully initialised: no PProcess has been constructed
		 * or destroyed here, so the H323CapabilityFactory and the OpalMediaFormat
		 * registry are still fully populated and the Initialise() inside the load builds
		 * a real capability table.  Had one been created and destroyed beforehand,
		 * PostShutdown() would have emptied both factories for good and the load could
		 * have added no audio capability at all - a degraded subject produced purely by
		 * harness ordering.  Case 1 runs in a separately exec'd helper so that this
		 * stays true.
		 *
		 * The binding must be registered before the call, because the load path reads
		 * the configuration itself.  The module is left loaded on purpose: every later
		 * case observes a loaded module, and the last asserts that shutting it down
		 * works.
		 */
		FST_TEST_BEGIN(module_load_registers_endpoint_interface)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			switch_status_t bound = SWITCH_STATUS_FALSE;
			switch_status_t pooled = SWITCH_STATUS_FALSE;
			FSProcess *process = NULL;
			char capabilities[2048];

			fst_h323_process_release();

			/* This is where the run's first PProcess in this address space comes
			 * up - and therefore the point at which PTLib's plugin enumeration is
			 * either contained or not.  Asserted before the load below, and
			 * fatally: a suite that went on to dlopen an inherited plugin
			 * directory must not run at all. */
			fst_requires(fst_h323_plugin_path_is_pinned());

			/* Fatal, and the last fatal check in this case: a second live PProcess would
			 * make PTLib abort inside the load below.  Nothing has been registered or
			 * allocated yet, so breaking out here strands nothing. */
			fst_requires(!PProcess::IsInitialised());

			/* From here on every check is NON-FATAL, so the cleanup tail at the
			 * bottom of this case is reached on every path.
			 *
			 * Registration and pool creation are therefore performed as
			 * statements whose status is checked afterwards, never as the
			 * expression of a fatal assertion.  A fatal check placed after the
			 * provider is registered would break out of the case body and leave
			 * that provider bound for every later case to trip over, because
			 * FST_TEARDOWN is deliberately empty. */
			bound = fst_h323_bind_config(fst_h323_conf_module_load);
			fst_xcheck(bound == SWITCH_STATUS_SUCCESS, "the h323.conf provider must be registered before the module reads its configuration");

			/* The module is loaded against the module-lifetime pool, NEVER
			 * against fst_pool: this case deliberately leaves the module loaded
			 * for every case after it, and fst_pool does not survive this case's
			 * teardown.
			 * See fst_h323_module_pool above. */
			pooled = fst_h323_module_pool_create();
			fst_xcheck(pooled == SWITCH_STATUS_SUCCESS, "the module-lifetime pool must be available before the module is loaded");
			fst_check(fst_h323_module_pool != NULL);

			/* The load is attempted only when both of its preconditions hold.
			 * Without the pool, mod_h323_load() would hand NULL straight to
			 * switch_loadable_module_create_module_interface() (mod_h323.cpp:159);
			 * without the provider it would read no configuration at all and
			 * Initialise() would take the empty-listener fallback that
			 * wildcard-binds port 1720 (mod_h323.cpp:441-442).  Attempting it
			 * regardless would trade a reported failure for an unreportable
			 * one. */
			if (bound == SWITCH_STATUS_SUCCESS && fst_h323_module_pool != NULL) {
				/* mod_h323_load() creates the module interface itself
				 * (mod_h323.cpp:159) and hands it back through the
				 * out-parameter */
				status = mod_h323_load(&module_interface, fst_h323_module_pool);
			} else {
				fst_fail("the module load was not attempted because a precondition failed");
			}

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
				int released = 0;

				fst_check(registered != NULL);

				if (registered != NULL) {
					fst_check_string_equals(registered->interface_name, "h323");
				}

				fst_check_int_equals((int) endpoint.m_listeners.size(), 1);
				fst_check_int_equals(fst_h323_toolkit.listener_calls, 1);
				fst_check_int_equals(fst_h323_toolkit.listener_nulls, 0);
				fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
				fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_LOOPBACK);
				fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_PORT_MODULE_LOAD);
				fst_check_int_equals((int) endpoint.GetListeners().GetSize(), 0);

				fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

				/* THE MODULE WAS LOADED AGAINST POPULATED REGISTRIES, and that
				 * is asserted rather than assumed.  AddAllCapabilities()
				 * (mod_h323.cpp:404) can only add an audio capability while the
				 * H323CapabilityFactory and the OpalMediaFormat registry are
				 * both populated, which is true here precisely because no
				 * PProcess has ever been constructed or destroyed in this address
				 * space before this case.  A
				 * non-empty capability table carrying this document's
				 * codec-prefs entries is therefore the observable proof that the
				 * subject is a fully initialised module rather than a degraded
				 * one.  No absolute count is asserted: factory registration
				 * varies by toolkit and build, so only growth and presence are
				 * stable properties. */
				fst_check(endpoint.GetCapabilities().GetSize() > 0);

				fst_h323_capability_table(endpoint.GetCapabilities(), capabilities, sizeof(capabilities));

				fst_check(!zstr(capabilities));

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "mod_h323 capability table after module load with codec-prefs \"PCMA,PCMU\": %s\n", capabilities);

				/* matched on the media-format names the capability table holds,
				 * which are h323_formats' left-hand column, for the two entries
				 * fst_h323_conf_module_load declares */
				fst_check_string_has(capabilities, "G.711-ALaw-64k");
				fst_check_string_has(capabilities, "G.711-uLaw-64k");

				/* and in the configured preference order */
				fst_check(fst_h323_name_position(capabilities, "G.711-uLaw-64k")
						  > fst_h323_name_position(capabilities, "G.711-ALaw-64k"));

				/*
				 * The module's listener is released here, by the case that caused it to be
				 * constructed, because nothing downstream would ever free it:
				 * mod_h323_shutdown() deletes the FSProcess (mod_h323.cpp:195), whose
				 * destructor deletes the endpoint (mod_h323.cpp:366-368), and
				 * ~FSH323EndPoint touches only StopGkClient() and ClearAllCalls()
				 * (mod_h323.cpp:612-617) without ever walking m_listeners.  With the
				 * hand-off intercepted the base endpoint's list is empty too, so draining
				 * now is safe for every later case: they read the module's interfaces,
				 * capabilities and process, never its listener records.
				 *
				 * The count is hoisted into a local before it is asserted because
				 * fst_check_int_equals expands its first argument twice
				 * (switch_fct.h:3845-3851); a releasing call inside it would release once,
				 * report zero on the second evaluation and fail an assertion that holds.
				 * No side-effecting expression appears as an argument to a check macro in
				 * this file.
				 */
				released = fst_h323_release_listeners(&endpoint);
				fst_check_int_equals(released, 1);
				fst_check(endpoint.m_listeners.empty());
			}

			/*
			 * SINGLE UNCONDITIONAL CLEANUP TAIL.  Every check above is
			 * non-fatal, so control always arrives here.
			 */
			if (bound == SWITCH_STATUS_SUCCESS) {
				fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);
			}

			if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_NOUNLOAD) {
				/* The load did not succeed, so nothing is handed on: reclaim
				 * everything here rather than leaving a pool and a half-built
				 * load behind for cases that would then have nothing to observe.
				 * The direct-object cases that follow stay meaningful even so -
				 * fst_h323_process_acquire() falls back to a harness-owned
				 * process when no live one exists - so they still report their
				 * own verdicts instead of collapsing. */
				fst_h323_suite_state_cleanup();
			}

			/* On the success path the module interface, h323_process and the
			 * pool that backs them are deliberately left alive for every case
			 * that follows: they adopt this process rather than creating one,
			 * and the shutdown case among them asserts that shutting the module
			 * down works. */
		}
		FST_TEST_END()

		/*
		 * CASE 3 - an empty gk-address leaves gatekeeper registration disabled.
		 *
		 * Runs the real Initialise(), so the production guard at mod_h323.cpp:451 is
		 * executed rather than reasoned about, and both observable consequences - no
		 * registration thread and no toolkit gatekeeper call - are asserted.  Socket-free
		 * all the same, because the listener hand-off is intercepted by the double.
		 */
		FST_TEST_BEGIN(gatekeeper_registration_disabled_when_gk_address_empty)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_loadable_module_interface_t *module_interface = NULL;
			bool initialised = false;
			int released = 0;

			fst_requires(fst_h323_process_acquire() != NULL);

			module_interface = switch_loadable_module_create_module_interface(fst_pool, "mod_h323_test_gk_disabled");
			fst_requires(module_interface != NULL);

			fst_requires(fst_h323_bind_config(fst_h323_conf_gk_disabled) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			/*
			 * Initialise() reads the configuration itself (mod_h323.cpp:381) and then
			 * evaluates the gatekeeper guard at mod_h323.cpp:451.  Stopping at
			 * ReadConfig() would leave m_thread NULL whether or not that guard works, so
			 * running Initialise() is what makes the NULL below evidence.
			 */
			initialised = endpoint->Initialise(module_interface);

			/* Initialise() returns TRUE unconditionally (mod_h323.cpp:457) */
			fst_check(initialised == true);

			/* An explicitly empty gk-address is stored empty ... */
			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check_string_equals((const char *) endpoint->TestGetGkAddress(), "");
			fst_check(endpoint->TestGetGkIdentifer().IsEmpty());
			fst_check(endpoint->TestGetGkInterface().IsEmpty());

			/* The !m_gkAddress.IsEmpty() guard at mod_h323.cpp:451 short-circuited during
			 * the call above, so no FSGkRegThread was constructed or resumed - which is
			 * what keeps this case free of network side effects. */
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);
			fst_check_string_equals(fst_h323_toolkit.gk_address, "");

			/* endpoint-name was omitted from this document, so the hard-coded
			 * default at mod_h323.cpp:492 stands */
			fst_check_string_equals((const char *) endpoint->TestGetEndpointName(), "FreeSwitch");

			fst_check(endpoint->GetSwitchInterface() != NULL);

			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);
			fst_check_int_equals(fst_h323_toolkit.listener_calls, 1);
			fst_check_int_equals(fst_h323_toolkit.listener_nulls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_LOOPBACK);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_PORT_GK_DISABLED);
			fst_check_int_equals((int) endpoint->GetListeners().GetSize(), 0);

			/* Ownership never transferred, so this case releases it. */
			released = fst_h323_release_listeners(endpoint);
			fst_check_int_equals(released, 1);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* The one live PProcess belongs to the module and is deliberately
			 * left untouched: this case adopted it, it did not create it. */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 4 - gk-address "*" requests a LAN gatekeeper search, the sentinel
		 * documented at h323.conf.xml:10 ("empty to disable, \"*\" to search LAN").
		 *
		 * The stored value is asserted, and so is the decision it drives: StartGkClient()
		 * is executed and the toolkit request it makes is observed through the double,
		 * because storage alone cannot distinguish a working sentinel from a string that
		 * is copied and then ignored.
		 *
		 * StartGkClient() is called directly rather than through Initialise() because
		 * Initialise() is unusable here for two production reasons.  A non-empty
		 * gk-address makes it construct an FSGkRegThread, SetAutoDelete() it and Resume()
		 * it (mod_h323.cpp:451-455), so the subject would be a self-deleting thread that
		 * cannot be joined and every assertion about it would be a race.  Decisively,
		 * StartGkClient()'s early return clears m_stop_gk but not m_thread
		 * (mod_h323.cpp:671-674; only the normal exit at :686 nulls it), so once that
		 * thread has deleted itself m_thread points at freed memory and
		 * ~FSH323EndPoint -> StopGkClient() sees it non-NULL, sets m_stop_gk and spins in
		 * `while (m_stop_gk) { h_timer(2); }` (mod_h323.cpp:693-701) waiting for a thread
		 * that no longer exists - an unbounded hang.  No case in this file calls
		 * Initialise() with a non-empty gk-address, so m_thread is NULL at every
		 * destruction here.
		 *
		 * Calling StartGkClient() directly reaches the same UseGatekeeper() invocation
		 * with one call, no thread, no sleep and no RAS I/O.
		 */
		FST_TEST_BEGIN(gatekeeper_lan_search_address_preserved_verbatim)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			int released = 0;

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

			/* Reading the configuration does not by itself start registration:
			 * only Initialise() constructs the thread, and this case does not
			 * call it - see the rationale above. */
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			fst_check_int_equals(fst_h323_toolkit.listener_calls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);

			/* Asserted before the call, so the invocation counted afterwards can only have
			 * come from the StartGkClient() below. */
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			/*
			 * The double reports failure - it must, because the normal loop exit
			 * dereferences a NULL GetGatekeeper() (mod_h323.cpp:684-685) - so production
			 * enters its retry loop.  m_stop_gk set beforehand means the first check
			 * inside that loop (mod_h323.cpp:671-674) returns, before h_timer() sleeps for
			 * gk-retry seconds and before RemoveGatekeeper() is reached: one invocation,
			 * no sleep, no I/O.
			 *
			 * The retry argument must be > 0 or the loop is never entered, so the
			 * configured gk-retry is passed - the value Initialise() would have passed
			 * (mod_h323.cpp:452).  The three PString pointers are the ones production
			 * passes; StartGkClient() ignores them and reads the members directly
			 * (mod_h323.cpp:664).
			 */
			endpoint->TestSetStopGk(true);
			fst_check(endpoint->TestGetStopGk() == true);

			endpoint->StartGkClient(endpoint->TestGetGkRetry(), NULL, NULL, NULL);

			fst_check_int_equals(fst_h323_toolkit.gk_calls, 1);
			fst_check_string_equals(fst_h323_toolkit.gk_address, "*");
			fst_check_int_equals((int) strlen(fst_h323_toolkit.gk_address), 1);

			fst_check_string_equals(fst_h323_toolkit.gk_identifier, "fst-gatekeeper");
			fst_check_string_equals(fst_h323_toolkit.gk_interface, "");

			/* Production consumed the abort flag on its way out, which is the observable
			 * proving the early-return branch at mod_h323.cpp:671-674 is the one that ran;
			 * the normal exit at :686 would have left m_stop_gk set and nulled m_thread. */
			fst_check(endpoint->TestGetStopGk() == false);
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* Initialise() never ran, so the listeners are still this case's.
			 * Hoisted before the assertion because fst_check_int_equals expands
			 * its argument twice (switch_fct.h:3845-3851). */
			released = fst_h323_release_listeners(endpoint);
			fst_check_int_equals(released, 1);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* The one live PProcess belongs to the module and is deliberately
			 * left untouched: this case adopted it, it did not create it. */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 5 - listener address and port are taken from configuration.
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
		 * (mod_h323.cpp:492) asserted by the gatekeeper-disabled case, and a
		 * nameless <listener> falling back to "unnamed" (mod_h323.cpp:567-568).
		 *
		 * Socket-free: ReadConfig() only.
		 */
		FST_TEST_BEGIN(listeners_parsed_from_configuration)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			int pools_before = 0;
			int pools_after = 0;
			int released = 0;

			fst_requires(fst_h323_process_acquire() != NULL);
			fst_requires(fst_h323_bind_config(fst_h323_conf_listeners) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			/* Counted across the call, so that the recording seam is proved live
			 * rather than assumed.  Reading a counter into a local first is not
			 * decoration: fst_check_int_equals expands to fct_xchk() with each
			 * operand passed as a separate argument (switch_fct.h:1183-1184), so
			 * each is evaluated twice - harmless for a plain int, but the habit is
			 * what keeps a side-effecting expression from ever being placed there.
			 *
			 * If a future edit ever moved, renamed or lost the interception, this
			 * delta would fall to zero and say so here, instead of the abandoned
			 * pool quietly reappearing as a sanitizer failure with no attribution. */
			pools_before = fst_h323_recorded_pool_total;

			status = endpoint->ReadConfig(0);

			pools_after = fst_h323_recorded_pool_total;

			fst_check(status == SWITCH_STATUS_SUCCESS);

			fst_check_int_equals(pools_after - pools_before, 1);
			fst_check(fst_h323_recorded_pool_count > 0);
			fst_check_int_equals(fst_h323_recorded_pool_overflow, 0);

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

			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			/* ReadConfig() alone hands nothing to StartListener(), so nothing is opened. */
			fst_check_int_equals(fst_h323_toolkit.listener_calls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
			fst_check_int_equals((int) endpoint->GetListeners().GetSize(), 0);

			released = fst_h323_release_listeners(endpoint);
			fst_check_int_equals(released, 2);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* The one live PProcess belongs to the module and is deliberately
			 * left untouched: this case adopted it, it did not create it. */
			fst_check(PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 6 - the codec preference string "PCMA,PCMU,GSM,G729" is honoured in order.
		 *
		 * Initialise() is called on an endpoint of this case's own, because the capability
		 * table is built there (mod_h323.cpp:393-421) while ReadConfig() only stores the
		 * preference string.  The document pins the listener to loopback on
		 * FST_H323_PORT_CODEC_PREFS and leaves gk-address empty, so the address the
		 * hand-off is asserted against is this case's own; the hand-off is intercepted by
		 * the double, so no socket is opened.  It needs the one PProcess the module-load
		 * case created to still be alive, because once any PProcess has been destroyed
		 * both PTLib factories are empty for good and AddAllCapabilities() has nothing to
		 * add - which is what the declaration order protects.
		 *
		 * The mechanism, so that the case is not mis-modelled: the loop at
		 * mod_h323.cpp:397-421 walks the module's own h323_formats table
		 * (mod_h323.cpp:58-72) in table order and asks, for each entry, whether the
		 * entry's short token occurs as a substring of the configured string
		 * (mod_h323.cpp:400).  Capabilities are appended in table order filtered by
		 * presence in the configuration, so the resulting order only coincidentally equals
		 * the order the operator wrote.
		 *
		 * Presence, relative order and monotonic growth are asserted, never an absolute
		 * count: each AddAllCapabilities() call (mod_h323.cpp:404) adds every factory
		 * entry matching a "<name>*{sw}" wildcard, so "G.729*{sw}" alone can match the
		 * G.729, G.729A, G.729B and G.729A/B registrars mod_h323.h installs
		 * (mod_h323.h:620-628).
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
			int released = 0;
			bool initialised = false;

			/* All fatal checks precede the binding registration, so a fatal
			 * exit can never orphan a binding into a later case.
			 *
			 * This case MUST run while the first PProcess of the process
			 * lifetime is still alive, and it does: acquire ADOPTS the module's
			 * process rather than creating a second, and nothing destroys a
			 * PProcess before the shutdown case, which is declared after this
			 * one.  Once any PProcess has been destroyed the capability factory
			 * and the media-format registry are empty for good and
			 * AddAllCapabilities() can add nothing. */
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

			fst_check(initialised == true);

			/* The preference string must have GROWN the capability table.  Only
			 * growth is asserted, never an absolute count on either side:
			 * H323CapabilityFactory registration varies by toolkit and build, so
			 * a fixed number would be an assertion about H323Plus rather than
			 * about mod_h323 and would break on a differently configured
			 * toolkit.  Growth, presence and relative order are the stable
			 * properties, and all three are asserted here. */
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

			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			fst_check_int_equals(fst_h323_toolkit.listener_calls, 1);
			fst_check_int_equals(fst_h323_toolkit.listener_nulls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_LOOPBACK);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_PORT_CODEC_PREFS);
			fst_check_int_equals((int) endpoint->GetListeners().GetSize(), 0);

			/* The listener is released here like everywhere else in this suite.
			 * The conditional ownership transfer at h323ep.h:519-531 never
			 * happened, because the hand-off was intercepted, and the empty
			 * endpoint listener list asserted immediately above is the proof:
			 * with nothing in it, ~H323EndPoint() has nothing to destroy
			 * (h323ep.cxx:978) and this release is the only one.  Hoisted
			 * because fst_check_int_equals expands its argument twice
			 * (switch_fct.h:3845-3851). */
			released = fst_h323_release_listeners(endpoint);
			fst_check_int_equals(released, 1);
			fst_check(endpoint->m_listeners.empty());
			delete endpoint;

			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_SUCCESS);

			/* The PROCESS IS DELIBERATELY NOT RELEASED HERE.  It belongs to the
			 * module, which the module-load case loaded and the shutdown case shuts down;
			 * releasing it now would destroy the module's own FSProcess behind
			 * its back and empty both PTLib factories before the shutdown case
			 * had observed anything.  This case owns the endpoint it constructed
			 * and nothing else, and it has just deleted that. */
			fst_check(PProcess::IsInitialised());
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
		 * opened, so they are allocated even by the configuration-absent case's
		 * failing call and may
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

			/* The preceding case must have left NOTHING registered.  That is the
			 * invariant every case's cleanup tail exists to keep, and this is
			 * where a breach first becomes observable, so it is asserted rather
			 * than assumed.  The unbind is the only public way to ask the
			 * question and it is harmless when the answer is "nothing": FALSE
			 * means no binding carried the pointer and nothing was changed
			 * (src/switch_xml.c:315-338).  Should a future edit ever reintroduce
			 * a fatal check after the provider is bound, this reports it here
			 * instead of letting a stray provider silently answer a later
			 * configuration lookup. */
			fst_xcheck(fst_h323_unbind_config() == SWITCH_STATUS_FALSE,
					   "the preceding case must leave no h323.conf provider registered");

			/* The module-load case left the module loaded, and every case between
			 * then and now adopted its process rather than replacing it.  Asserted
			 * through PTLib's
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

			/* ONLY NOW is retained state released.  Shutdown is the last thing
			 * that touches pool-backed module state, so this is the earliest
			 * point at which destroying the pool is safe - and doing it here,
			 * inside this case, keeps it out of the per-case teardown that owns
			 * fst_pool.
			 *
			 * The sweep is total rather than a bare pool destroy so that it also
			 * reclaims anything an earlier case could have orphaned by breaking
			 * out on a fatal precondition: a still-registered provider, a stray
			 * harness-owned fallback PProcess, and the pool.  Every step is idempotent, so
			 * repeating the shutdown this case has already asserted twice costs
			 * nothing.  No check in this case is fatal, so this tail is reached
			 * unconditionally. */
			fst_h323_suite_state_cleanup();

			fst_check(fst_h323_module_pool == NULL);
			fst_check(!PProcess::IsInitialised());

			/* Three properties of the root-pool recording seam, asserted from the last
			 * declared case because only here can the whole run be characterised: the
			 * seam recorded something, so it was in force for the five cases that read
			 * configuration rather than silently bypassed; nothing it recorded survives,
			 * so every pool the module abandoned was released and the image the sanitizer
			 * examines at exit is balanced; and the recorder never overflowed its fixed
			 * table, which is the one way a pool could escape the sweep unnoticed.  The
			 * total is logged as well, so the run's own accounting is readable in CI
			 * output without a debugger. */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "mod_h323 abandoned %d root memory pool(s) during this run; %d still held, %d recorder overflow(s)\n",
							  fst_h323_recorded_pool_total, fst_h323_recorded_pool_count, fst_h323_recorded_pool_overflow);

			fst_check(fst_h323_recorded_pool_total > 0);
			fst_check_int_equals(fst_h323_recorded_pool_count, 0);
			fst_check_int_equals(fst_h323_recorded_pool_overflow, 0);

			/* The positive closing statement that the suite exits with an empty
			 * binding list.  FALSE here is the expected answer: the entry check
			 * established that the preceding case unbound its provider, no case
			 * binds
			 * anything afterwards, and the sweep's own unbind was therefore a
			 * no-op (src/switch_xml.c:315-338). */
			fst_check(fst_h323_unbind_config() == SWITCH_STATUS_FALSE);
		}
		FST_TEST_END()

		/*
		 * CASE 8 - the OOS-9 reservation OUTLIVES the module it protected.  Declared
		 * immediately after the shutdown case, because that shutdown is what it observes.
		 *
		 * This is the case whose absence let the QA6 defect through.  While the claim was
		 * released by mod_h323_shutdown(), the sequence `load mod_h323; unload mod_h323;
		 * load mod_opal' left BOTH guard arms clear - the module had left
		 * loadable_modules.module_hash and the reservation was free - while
		 * libpt.so.2.10.9 was still mapped, because this build defines HAVE_FAKE_DLCLOSE
		 * and src/switch_dso.c:91-98 then skips dlclose().  The sibling's load walked
		 * straight into the PProcess::Construct() SIGSEGV the guard exists to prevent.
		 * Residency, not registration, is the invariant, so the claim now outlives the
		 * module and only its own holder may take it again.
		 *
		 * Asserted, in the state case 7 leaves behind - shut down, no interface, no
		 * PProcess: the claim is still held and still names THIS module; a sibling can
		 * take it neither as a free claim nor as a re-claim, which is exactly the
		 * compare-and-swap pair mod_opal_load() would execute against it; and this
		 * module's own re-claim still succeeds, which is why `load mod_h323;
		 * unload mod_h323; load mod_h323' remains a supported operator flow.
		 *
		 * The refusal those predicates produce inside the REAL sibling is not reachable
		 * from this binary, for the same reason the stand-in below exists - loading the
		 * real mod_opal is the crash under discussion - so the end-to-end proof lives in
		 * the runtime evidence archived with the determination.  Nothing here is left
		 * changed: the claim is observed and handed back to itself, never consumed.
		 */
		FST_TEST_BEGIN(coload_reservation_outlives_the_unloaded_module)
		{
			char *holder = NULL;

			/* The claim case 2 took is still there, and it is ours.  A NULL holder at
			 * this point IS the defect: it means a shutdown released the claim while
			 * the PTLib runtime it stood for remained mapped. */
			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_xcheck(holder != NULL,
					   "the PTLib reservation must survive mod_h323_shutdown(), because the runtime stays mapped (OOS-9)");
			fst_check(holder != NULL && !strcmp(holder, modname));
			switch_safe_free(holder);

			/* Neither arm of the sibling's claim can succeed against it: not the
			 * free-claim arm, whose val2 "" matches only an unset or empty variable,
			 * and not the re-claim arm, whose val2 matches only the sibling's own
			 * name.  Both failing is precisely what makes mod_opal_load() refuse. */
			fst_check(switch_core_set_var_conditional(FST_H323_PTLIB_RESERVATION,
													 FST_H323_SIBLING_MODULE, "") != SWITCH_TRUE);
			fst_check(switch_core_set_var_conditional(FST_H323_PTLIB_RESERVATION,
													 FST_H323_SIBLING_MODULE,
													 FST_H323_SIBLING_MODULE) != SWITCH_TRUE);

			/* Neither refused attempt may have moved the claim. */
			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_check(holder != NULL && !strcmp(holder, modname));
			switch_safe_free(holder);

			/* THIS module may still re-claim what it already holds.  That is the arm
			 * that keeps reloading mod_h323 alone working after an unload. */
			fst_check(switch_core_set_var_conditional(FST_H323_PTLIB_RESERVATION,
													 modname, modname) == SWITCH_TRUE);

			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_check(holder != NULL && !strcmp(holder, modname));
			switch_safe_free(holder);

			/* None of this constructed anything: the claim is guard state, not a
			 * constructor, and case 7's teardown must still hold. */
			fst_check(!PProcess::IsInitialised());
		}
		FST_TEST_END()

		/*
		 * CASE 9 - the OOS-9 co-load refusal reached through the MODULE HASH.
		 *
		 * Declared here for two reasons.  It needs a module that is NOT loaded, which is
		 * exactly what case 7 leaves behind, and it must not perturb the eight cases
		 * before it:
		 * the sibling stand-in it registers would make every one of their loads refuse
		 * if it ever outlived this case.  Its cleanup tail removes the stand-in and
		 * confirms the removal, so nothing it registers reaches the case after it, and
		 * no check here is fatal.
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
		 * clear of the module-lifetime pool that case 7 destroyed.
		 *
		 * See the sibling stand-in above for why the real mod_opal is not loaded here
		 * and where the end-to-end proof lives instead.
		 */
		FST_TEST_BEGIN(coload_guard_refuses_when_sibling_is_loaded)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			const char *err = NULL;

			/* The positive control.  Nothing in this binary has ever registered the
			 * sibling, so the guard's predicate must report absence - which is why
			 * every earlier case's load was allowed to proceed. */
			fst_check(switch_loadable_module_exists(FST_H323_SIBLING_MODULE) == SWITCH_STATUS_FALSE);

			/* Registration is a statement whose status is checked afterwards, never the
			 * expression of a fatal check: a fatal check here would break out of the
			 * body and leave the stand-in registered. */
			status = switch_loadable_module_build_dynamic((char *) FST_H323_SIBLING_MODULE,
														 fst_h323_sibling_stub_load, NULL,
														 fst_h323_sibling_stub_shutdown, SWITCH_FALSE);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "the mod_opal stand-in must register before the co-load guard is exercised");

			if (status == SWITCH_STATUS_SUCCESS) {
				fst_check(switch_loadable_module_exists(FST_H323_SIBLING_MODULE) == SWITCH_STATUS_SUCCESS);

				status = mod_h323_load(&module_interface, fst_pool);

				fst_xcheck(status == SWITCH_STATUS_FALSE, "mod_h323 must refuse to load while mod_opal is present (OOS-9)");
				fst_check(module_interface == NULL);
				fst_check(!PProcess::IsInitialised());

				/* Cleanup tail: remove the stand-in and confirm the predicate answers
				 * absence again, so the refusal is provably a function of the sibling's
				 * presence rather than of anything permanent this case did. */
				fst_check(switch_loadable_module_unload_module("", FST_H323_SIBLING_MODULE, SWITCH_FALSE, &err) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_loadable_module_exists(FST_H323_SIBLING_MODULE) == SWITCH_STATUS_FALSE);
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
		 * reached the way a losing concurrent claimant reaches it: the reservation is handed
		 * to the SIBLING's name while the sibling is NOT in the module hash - exactly the
		 * state the window produces - and mod_h323_load() is then asked to load into it.
		 *
		 * The hand-over is a compare-and-swap out of THIS module's name rather than a bare
		 * set, because case 8 established that the claim is still held by this module: the
		 * reservation now outlives the shutdown case 7 performed, so there is a holder to
		 * displace and the transfer must fail loudly if there is not.
		 *
		 * Asserted: the sibling really is absent from the hash, so case 9's guard cannot be
		 * what refuses; the load returns SWITCH_STATUS_FALSE; no module interface is created;
		 * and no FSProcess is constructed, which is the property that matters because
		 * constructing a second one is what crashes the process.  The claim is then handed
		 * back to the module that really did construct a PProcess here, so the suite exits
		 * with the reservation describing the truth about this process.
		 */
		FST_TEST_BEGIN(coload_reservation_refuses_a_reserved_ptlib_runtime)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			char *holder = NULL;

			/* The reservation starts out held by THIS module: case 7 shut the module down
			 * and the claim deliberately survived it, as case 8 asserts.  If it were
			 * unheld, this case would be measuring the defect rather than the guard. */
			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_xcheck(holder != NULL && !strcmp(holder, modname),
					   "the PTLib reservation must still be held by mod_h323 when this case begins");
			switch_safe_free(holder);

			/* The distinguishing control: the sibling is NOT registered, so the module-hash
			 * guard asserted in case 9 cannot be the thing that refuses below. */
			fst_check(switch_loadable_module_exists(FST_H323_SIBLING_MODULE) == SWITCH_STATUS_FALSE);

			/* Hand the reservation over to the sibling's name, the way the sibling's own
			 * load would have taken it a moment before publishing itself. */
			fst_xcheck(switch_core_set_var_conditional(FST_H323_PTLIB_RESERVATION,
													  FST_H323_SIBLING_MODULE, modname) == SWITCH_TRUE,
					   "the sibling stand-in must be able to take the PTLib reservation over");

			status = mod_h323_load(&module_interface, fst_pool);

			fst_xcheck(status == SWITCH_STATUS_FALSE,
					   "mod_h323 must refuse to load while another endpoint holds the PTLib reservation (OOS-9)");
			fst_check(module_interface == NULL);
			fst_check(!PProcess::IsInitialised());

			/* The refused load must not have taken the reservation from its holder either. */
			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_check(holder != NULL && !strcmp(holder, FST_H323_SIBLING_MODULE));
			switch_safe_free(holder);

			/* Cleanup tail: hand the claim back to the module that really did construct a
			 * PProcess in this process, and confirm the hand-back, so the refusal is
			 * provably a function of who held the reservation rather than of anything
			 * permanent this case did. */
			fst_check(switch_core_set_var_conditional(FST_H323_PTLIB_RESERVATION, modname,
													 FST_H323_SIBLING_MODULE) == SWITCH_TRUE);

			holder = switch_core_get_variable_dup(FST_H323_PTLIB_RESERVATION);
			fst_check(holder != NULL && !strcmp(holder, modname));
			switch_safe_free(holder);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
