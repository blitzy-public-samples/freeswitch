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
 * switch_module_runtime entry point, and no case here can start one because
 * there is none to start.  Alongside those, the suite drives
 * FSH323EndPoint::ReadConfig() and FSH323EndPoint::Initialise() directly.
 *
 * That is a statement about FreeSWITCH's runtime entry point and nothing wider:
 * the H.323 TOOLKIT does start threads of its own, and two cases here reach the
 * code that starts them.  FSH323EndPoint::Initialise() hands every configured
 * listener to H323EndPoint::StartListener(), and an H323Listener IS a PThread
 * (h323plus transports.h), so a listener that binds successfully leaves a live
 * toolkit thread behind for as long as the endpoint owning it lives.  Two cases
 * take that path - the module-load case, whose listener then lives until the
 * shutdown case deletes the process, and the codec-preference case, which owns
 * and deletes its own endpoint within the case.  Both are confined to a
 * loopback address on a fixed high port, and no case starts a gatekeeper RAS
 * thread at all; see NETWORK SIDE-EFFECT CONTAINMENT below for the rules that
 * make that true and for how each thread is reclaimed.
 *
 * WHY THE PRODUCTION TRANSLATION UNIT IS INCLUDED (and not just the header)
 * ------------------------------------------------------------------------
 * The obvious shape for a C++ module test, and the one the tree's only
 * precedent uses (src/mod/codecs/mod_openh264/Makefile.am:12-21), is a
 * convenience library - noinst_LTLIBRARIES = libmodh323.la - linked in through
 * test_test_mod_h323_LDADD, with the suite including only "../mod_h323.h".
 * That arrangement was built for this module and then measured, and it DOES NOT
 * LINK.  Reproduced verbatim under g++ 12.5.0 with GNU ld 2.45:
 *
 *     /usr/bin/ld: ./.libs/libmodh323.a(libmodh323_la-mod_h323.o): in function
 *         `BaseG7231Capab::OnReceivedPDU(H245_AudioCapability const&,
 *          unsigned int&)':
 *     mod_h323.h:593: multiple definition of
 *         `FSH323_T38Capability::CreateChannel(H323Connection&,
 *          H323Channel::Directions, unsigned int,
 *          const H245_H2250LogicalChannelParameters*) const';
 *     test/test_mod_h323-test_mod_h323.o:mod_h323.h:593: first defined here
 *     collect2: error: ld returned 1 exit status
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
 *     situation, and the trigger is named explicitly: white-box inclusion is
 *     "held as the documented contingency for a duplicate-symbol failure"
 *     (AAP 0.8.4 Correction 5), on which the suite "switches to white-box
 *     source inclusion ... and its convenience library declaration is dropped"
 *     (AAP 0.3.1), described there as "a local change to one Makefile.am and
 *     one #include line", with "the compile-twice convenience-library shape ...
 *     the documented fallback in either direction" (AAP 0.8.8 R2).  The link
 *     error quoted above IS that duplicate-symbol failure, so the contingency
 *     is live rather than hypothetical.
 *
 *     Every alternative is closed off by a rule this work may not break.
 *     Adding `inline` at mod_h323.h:588 would fix the collision at its root but
 *     is a production edit to a file listed under "Source that must not change"
 *     (AAP 0.2.2), barred by R2-1, whose minimum required production change is
 *     zero (AAP 0.8.1).  -Wl,--allow-multiple-definition only hides an ODR
 *     violation and is not portable.  Renaming the member is barred by exactly
 *     the same rule - it is another edit to the same frozen production header,
 *     and it would additionally change a public class's API for the convenience
 *     of a test - and it is unnecessary anyway, because compiling the module
 *     translation unit in removes the collision without touching the module.
 *
 *     The #include line is the `#include "../mod_h323.cpp"` below.  The
 *     Makefile.am change is a separate work boundary that this file must not
 *     edit, so the contract it has to satisfy is stated here as a REQUIREMENT
 *     rather than being left implicit:
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
 *     -D_REENTRANT, -fno-exceptions, -DP_64BIT under the 64-bit Linux
 *     conditional, the openh323/PTLib link flags
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
 * port 1720.
 *
 * Containment is structural and has three independent layers, so that no single
 * mistake can leak a socket:
 *
 *   1. THE TOOLKIT ENTRY POINTS ARE INTERPOSED.  Both StartListener() overloads
 *      and UseGatekeeper() are retargeted onto file-static doubles that record
 *      the request and perform no I/O - see the interposition block above the
 *      subject include.  NO PORT IS EVER BOUND by this suite, on any path, and
 *      every case that runs Initialise() asserts the endpoint's own listener
 *      list is empty afterwards as the positive proof of it.
 *   2. EVERY INJECTED DOCUMENT DECLARES A LISTENER, pinned to 127.0.0.1 on a
 *      fixed high port, so the wildcard fallback is unreachable regardless.  The
 *      shipped sample's $${local_ip_v4} and port 1720 (h323.conf.xml:23-28) are
 *      reference values only and are never used as-is.  Each case additionally
 *      asserts that the default-interface overload was not the one invoked.
 *   3. NO CONFIGURATION WITH A NON-EMPTY gk-address EVER REACHES Initialise().
 *      That is not merely about RAS traffic: StartGkClient()'s early return
 *      clears m_stop_gk but leaves m_thread pointing at a self-deleted thread,
 *      and ~FSH323EndPoint -> StopGkClient() then spins forever waiting on it
 *      (mod_h323.cpp:670-673 against :693-701).  The LAN-search case therefore
 *      drives StartGkClient() directly instead, which reaches the same decision
 *      with no thread at all; its case comment carries the full derivation.
 *
 * No case performs third-party network I/O, opens a random port, binds any port,
 * or depends on wall-clock time.
 *
 * RESOURCE RECLAMATION
 * --------------------
 * The run is balanced, because CI configures the address sanitizer with leak
 * detection live and a leak there is a build failure rather than a test failure.
 * Three things need reclaiming and each is anchored where it is unconditionally
 * correct.  The unstarted H323ListenerTCP objects ReadConfig() produced are owned
 * by the FSListener records in the public FSH323EndPoint::m_listeners list and are
 * freed from there - the StartListener() double deliberately adopts nothing, so
 * there is one owner and one release path whether a case reached Initialise() or
 * stopped at ReadConfig().  The root pool FSH323EndPoint::ReadConfig() allocates
 * and abandons (mod_h323.cpp:469) is captured by a preprocessor seam scoped to the
 * module translation unit and released from the per-case teardown - see the
 * ROOT-POOL RECORDING SEAM below, including why that is a seam and not a
 * production edit.  And the module's own retained state - provider registration,
 * PProcess, module-lifetime pool - is handed forward deliberately from case to
 * case and swept by the last declared case, see fst_h323_suite_state_cleanup().
 */

#include <switch.h>
#include <test/switch_test.h>

/*
 * ---------------------------------------------------------------------------
 * HERMETIC TOOLKIT INTERPOSITION
 * ---------------------------------------------------------------------------
 *
 * WHY THIS EXISTS
 * ---------------
 * Three production decisions can only be observed by EXECUTING them, and all
 * three reach out to the H.323 toolkit when they run:
 *
 *   - Initialise() hands every configured listener to StartListener()
 *     (mod_h323.cpp:441-448), which OPENS A TCP SOCKET on the configured
 *     address and port and takes ownership of the object on success
 *     (h323ep.h:519-545);
 *   - Initialise() constructs and resumes a live FSGkRegThread whenever
 *     gk-address is non-empty (mod_h323.cpp:451-455);
 *   - StartGkClient() asks the toolkit to register with, or search the LAN
 *     for, a gatekeeper through UseGatekeeper() (mod_h323.cpp:663), which is
 *     live RAS signalling.
 *
 * Asserting those decisions from stored configuration alone proves only that
 * ReadConfig() copied a string.  Executing them against the real toolkit binds
 * ports this process does not own and performs network I/O, which makes the
 * suite non-hermetic and port-collision-prone.  Interposing the two toolkit
 * entry points removes both problems at once: the decisions run for real, and
 * nothing leaves the process.
 *
 * WHY MACRO REWRITING IS THE ONLY AVAILABLE SEAM
 * ----------------------------------------------
 * Neither H323EndPoint::StartListener nor H323EndPoint::UseGatekeeper is
 * virtual (h323ep.h:532, :547 and :354), so a subclass cannot override either
 * and the existing FSH323TestEndPoint cannot help.  Rewriting the CALL SITES
 * is therefore the only seam available, and it is applied here - immediately
 * before the subject is included, and nowhere else in this file.
 *
 * WHY THIS IS SAFE
 * ----------------
 * The rewrite is confined by construction.  Exactly ONE header in this
 * translation unit's include set names either token - the toolkit's own
 * /usr/include/openh323/h323ep.h - and it is pulled in, in full and behind its
 * include guard, by the <ptlib.h> and <h323.h> pre-include below, BEFORE either
 * macro exists.  mod_h323.h names neither token.  The only text the macros can
 * therefore reach is mod_h323.cpp's own call sites, which is exactly the intent.
 *
 * The pre-include reproduces mod_h323.h's visibility bracket verbatim
 * (mod_h323.h:38-40 and :55-57), so the toolkit's declarations are seen with
 * the same visibility the production build sees them with.  The result was
 * verified at the symbol level: the three undefined toolkit references carried
 * by the module's own object file - H323EndPoint::StartListener(H323Listener*),
 * H323EndPoint::StartListener(H323TransportAddress const&) and
 * H323EndPoint::UseGatekeeper(PString const&, PString const&, PString const&) -
 * are absent from this suite's object, and nothing else changed.
 *
 * NO PRODUCTION LINE IS MODIFIED.  mod_h323.cpp and mod_h323.h are compiled
 * from byte-identical text with the module target's own flags; only this file's
 * preprocessor state differs.
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
	/* UseGatekeeper (mod_h323.cpp:663) */
	int gk_calls;
	char gk_address[FST_H323_ADDRESS_MAX];
	char gk_identifier[FST_H323_ADDRESS_MAX];
	char gk_interface[FST_H323_ADDRESS_MAX];

	/* StartListener(H323Listener *) - the per-record overload, mod_h323.cpp:446 */
	int listener_calls;
	int listener_nulls;
	char listener_address[FST_H323_MAX_OBSERVED_LISTENERS][FST_H323_ADDRESS_MAX];

	/* StartListener(const H323TransportAddress &) - the empty-list fallback,
	 * mod_h323.cpp:442 */
	int listener_default_calls;
	char listener_default_address[FST_H323_ADDRESS_MAX];
} fst_h323_toolkit_t;

static fst_h323_toolkit_t fst_h323_toolkit;

/* Called from per-case setup, so every case observes only its own calls */
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
 * UseGatekeeper double.  Records the request and reports FAILURE.
 *
 * FALSE is the only safe answer, and the reason is production code rather than
 * preference: once the retry loop exits normally StartGkClient() dereferences
 * GetGatekeeper() unconditionally (mod_h323.cpp:684-685), and this double
 * registers nothing, so GetGatekeeper() is NULL and TRUE would segfault.  FALSE
 * enters the loop, whose FIRST m_stop_gk check returns cleanly
 * (mod_h323.cpp:670-673) - before any h_timer() sleep, before
 * RemoveGatekeeper(), and before that dereference.  The LAN-search case arms
 * m_stop_gk for precisely that reason.
 *
 * The default arguments reproduce the declaration at h323ep.h:354-358 so that
 * every call shape the production code is allowed to use still compiles.
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
 * StartListener(H323Listener *) double - the overload Initialise() uses for
 * every configured listener (mod_h323.cpp:446).
 *
 * Records the transport address the production code asked to listen on, opens
 * NOTHING, and reports success so that production's success path is the one
 * under test.
 *
 * OWNERSHIP, which is the whole reason this double is worth its comment.  The
 * real overload ADOPTS the object when it returns TRUE (h323ep.h:519-531), by
 * putting it in the endpoint's H323ListenerList.  This double deliberately does
 * not, because adopting it would mean handing it to the very toolkit this seam
 * exists to keep out of the test.  Ownership therefore stays exactly where
 * ReadConfig() put it - in the FSListener record inside
 * FSH323EndPoint::m_listeners (mod_h323.h:270) - which is public, is the single
 * canonical record of every listener the configuration produced, and is where
 * fst_h323_release_listeners() drains it from.  That keeps the ownership rule
 * UNIFORM for every case in this suite, whether it reaches Initialise() or
 * stops at ReadConfig(), and makes a double free structurally impossible:
 * there is one owner and one release path.
 *
 * The endpoint's own listener list staying empty is not a footnote, it is the
 * assertion that proves no socket was ever bound - every case that runs
 * Initialise() checks GetListeners().GetSize() (h323ep.h:2027) is zero.
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
 * StartListener(const H323TransportAddress &) double - the DEFAULT-INTERFACE
 * fallback, taken only when the configuration declares no listener at all
 * (mod_h323.cpp:441-443, which passes "" and so would listen on 0.0.0.0:1720).
 *
 * Every document this suite injects declares at least one listener, so this
 * overload must never be reached, and every Initialise() case asserts exactly
 * that.  It is nonetheless required for the suite to compile: the macro rewrite
 * below retargets BOTH call sites, so both overloads must exist.
 */
static PBoolean fst_h323_start_listener(const H323TransportAddress & iface)
{
	fst_h323_toolkit.listener_default_calls++;
	fst_h323_record_string(fst_h323_toolkit.listener_default_address, sizeof(fst_h323_toolkit.listener_default_address), iface);

	return TRUE;
}

/*
 * ---------------------------------------------------------------------------
 * ROOT-POOL RECORDING SEAM
 * ---------------------------------------------------------------------------
 * FSH323EndPoint::ReadConfig() opens with an unconditional
 * `switch_core_new_memory_pool(&pool)` (mod_h323.cpp:469) and then never uses
 * the result: `pool` is a local, no pointer derived from it is stored anywhere,
 * and no destroy call exists on any of the function's exit paths.  It is the
 * only pool allocation in the whole translation unit.  Every case that reads
 * configuration - directly or through the module load - therefore strands one
 * independent, parentless root pool, and CI configures the address sanitizer
 * with leak detection live, so a suite that exercised those paths repeatedly
 * would not be sanitizer-clean.
 *
 * WHY A SEAM RATHER THAN A PRODUCTION EDIT
 * ----------------------------------------
 * The AAP freezes mod_h323.cpp (rule R2-1: zero production translation units
 * are edited) and permits a production change only as a last resort, for the
 * minimum strictly required for testability (0.8.1, directive 4).  Here it is
 * not required at all, because this suite already compiles the production
 * translation unit into itself and switch_core_new_memory_pool() is a MACRO
 * (switch_core.h:633) rather than a function - so the allocation is
 * interceptable at the preprocessor, with no edit to the module and no change
 * to its symbol surface.  The interception is bounded to exactly the region
 * that needs it: defined just above the module include, restored
 * immediately after it, so nothing outside mod_h323.cpp is affected - including
 * this file's own module-lifetime pool below, which must keep the real
 * allocator because a different owner destroys it.
 *
 * WHY DESTROYING THE RECORDED POOLS IS SAFE
 * -----------------------------------------
 * Because nothing escapes.  `pool` is written once, tested once, and never read
 * again; no switch_core_alloc(), no switch_core_strdup() and no subpool is
 * taken from it anywhere in mod_h323.cpp.  Destroying it after the call has
 * returned therefore cannot invalidate any live reference, and the recorded
 * pointer is the only remaining reference in the process.  This is the test
 * cleaning up an allocation the code under test abandoned - not a substitute
 * for a production fix, and not a suppression: the pool really is released, so
 * the sanitizer sees a balanced process.
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

/* The interception, scoped to the module translation unit and nothing else. */
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

	/* m_stop_gk is the flag StartGkClient() tests to abandon its retry loop
	 * (mod_h323.cpp:670-673 and :675-678).  The LAN-search case ARMS it before
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
 * NOT ONE OF THESE PORTS IS EVER BOUND, and that is a structural property of
 * this suite rather than an incidental one.  ReadConfig() constructs an
 * H323ListenerTCP but does not open it (mod_h323.cpp:583); only
 * StartListener() opens a socket, and every StartListener() call site is
 * retargeted onto fst_h323_start_listener(), which records the requested
 * address and opens nothing.  The suite therefore cannot collide with another
 * process, with a parallel copy of itself, or with a service that happens to
 * hold one of these numbers - and the Initialise() cases assert the endpoint's
 * own listener list is empty afterwards, which is the positive proof of it.
 *
 * They remain FIXED and DISTINCT rather than random because each one is now
 * pure test data: a distinct value per document lets each case assert that the
 * address production asked to listen on is the one ITS document configured, and
 * never another case's.
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
 * The consequence is decisive for every case that builds a capability table.
 * AddAllCapabilities() (mod_h323.cpp:404) enumerates that factory and validates
 * each match against the media-format registry, so once ANY PProcess has been
 * destroyed no audio capability can ever be added again - by a directly
 * constructed endpoint or by the module's own.  A per-case FSProcess, or any
 * arrangement that destroys one process and then creates another, therefore
 * leaves whichever case runs second observing a hollowed-out module: not
 * because mod_h323 is wrong, but because the harness had already burnt down the
 * registry underneath it.
 *
 * THE ISOLATION STRATEGY, STATED EXACTLY
 * --------------------------------------
 * Exactly ONE PProcess-derived object exists in THIS process for the whole run,
 * and it is the one the MODULE creates.  The module-load case loads the module,
 * which constructs that FSProcess (mod_h323.cpp:167) while both registries are
 * still fully populated; every later case ADOPTS it through PTLib's public
 * singleton accessor instead of constructing a second; and the last declared
 * case shuts the module down, which is the only thing that destroys it.  So no
 * PProcess is destroyed until every assertion has been made, the factories are
 * populated for the entire run, the one-live-PProcess invariant holds at every
 * instant, and the module is observed fully initialised rather than degraded.
 *
 * The one case that cannot fit inside that arrangement is the
 * configuration-absent branch, which must be declared first AND must have a
 * PProcess to construct an endpoint against.  It is given an address space of
 * its own instead of a share of this one: it runs in a separately exec'd helper
 * process, so the process it brings up is not a second live PProcess here and
 * its destruction empties no factory here.  The invariant above is therefore stated per process
 * and holds exactly as written.  See fst_h323_run_readconfig_isolated().
 *
 * The harness therefore normally owns no process at all.  fst_h323_process
 * below stays NULL for the whole of a healthy run and exists only as the
 * fallback for a run in which the module load did not happen - so that the
 * direct-object cases still have a process to work against and report their own
 * verdicts instead of collapsing.  Ownership is unambiguous either way: the
 * release helper frees only a process this file created, never the module's.
 */
static FSProcess *fst_h323_process = NULL;

/*
 * ---------------------------------------------------------------------------
 * PLUGIN-SEARCH-PATH CONTAINMENT
 * ---------------------------------------------------------------------------
 *
 * PTLib resolves its plugin directory from the environment before falling back
 * to the P_DEFAULT_PLUGIN_DIR compiled into libpt: PPluginManager honours
 * PTLIBPLUGINDIR and, for backward compatibility, the older PWLIBPLUGINDIR.
 * Whichever it settles on it then ENUMERATES RECURSIVELY and dlopen()s every
 * shared object that matches, running each one's initialisers, before any test
 * code gets a say.  That happens as a side effect of bringing a PProcess up, so
 * the first PProcess this run creates - the module's, in a healthy run - is the
 * moment of exposure.
 *
 * Neither variable is sanitised for us.  mod_h323 never touches them - unlike
 * mod_opal, which sets PTLIBPLUGINDIR in its load function (mod_opal.cpp:108) -
 * so a suite that simply let a process come up would inherit whatever the
 * invoking environment happened to say and execute code from it.  A test binary
 * is run by `make check' out of an environment nobody audits, which makes an
 * inherited search path an arbitrary-code-execution seam (CWE-427, CWE-829).
 *
 * So both variables are pinned, unconditionally and before the first PProcess,
 * to a fixed directory that does not exist.  Nothing can be enumerated in a
 * directory that is not there, and "/no/thanks" is deliberately the same value
 * the sibling production module already uses, so harness and module agree.
 *
 * WHY setenv() AND NOT putenv()
 * ----------------------------
 * setenv() COPIES both the name and the value into storage the C library owns,
 * so nothing of ours is retained by the environment.  putenv() does the
 * opposite: it retains the caller's buffer, which makes it correct only with a
 * writable object of static storage duration and makes the common
 * `putenv((char *) "NAME=value")` idiom a const-correctness violation - the
 * environment then holds a mutable pointer into read-only memory, and anything
 * that writes through it, including a later putenv() of the same name in another
 * library, is undefined behaviour.  setenv() removes the hazard rather than
 * arguing it is unreachable, and it needs no cast at all.
 *
 * The overwrite flag is 1 because the point is to REPLACE whatever the invoking
 * environment said, not to supply a default for an unset variable.
 *
 * It is wired in two places on purpose: the suite setup hook, which FCTX runs
 * before every case body, and the acquire helper below, which is the only place
 * in this file that constructs a PProcess.  Either alone would be sufficient
 * today; having both means no future re-ordering and no new case can
 * reintroduce the exposure.  It is idempotent, so paying for it twice costs
 * nothing.
 *
 * NOTHING ABOUT THE PIN IS CACHED, AND THAT IS THE POINT.  setenv() can fail -
 * it returns -1 and sets errno on an allocation failure or an invalid name - so
 * a helper that assumed success and remembered it would report containment that
 * does not exist, and every assertion built on that memory would pass while the
 * process came up against an attacker-supplied search path.  The pin is
 * therefore re-applied and RE-VERIFIED BY READBACK on every call, and the
 * verdict is derived from the environment as it is at that instant rather than
 * from a flag.  Two setenv() calls and two getenv()/strcmp() pairs are far too
 * cheap for the saving to be worth the failure mode.
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
 * ---------------------------------------------------------------------------
 * PROCESS-ISOLATED EXECUTION, FOR THE CONFIGURATION-ABSENT CASE ONLY
 * ---------------------------------------------------------------------------
 *
 * WHY A SEPARATE PROCESS IS NECESSARY, AND NOT MERELY CONVENIENT
 * -------------------------------------------------------------
 * Two requirements collide inside one address space, and no ordering of cases
 * inside a single process satisfies both.
 *
 *   (a) The configuration-absent failure branch must be the FIRST declared
 *       case, so it runs before any successful load has left module state
 *       behind and its verdict cannot be an artefact of what ran earlier.
 *
 *   (b) The subject of that branch is FSH323EndPoint::ReadConfig(), and an
 *       FSH323EndPoint cannot exist without a live PProcess: H323EndPoint's
 *       constructor calls PProcess::Current(), which on an uninitialised
 *       process prints "Catastrophic failure" and terminates the binary
 *       outright.  So the case must bring a PProcess up.
 *
 * The collision is that PProcess::~PProcess() runs PostShutdown(), which calls
 * DestroySingletons() on every PFactory and IRREVERSIBLY empties both the
 * OpalMediaFormat registry and the H323CapabilityFactory key list.  They are
 * never repopulated.  FSH323EndPoint::AddAllCapabilities() (mod_h323.cpp:404)
 * enumerates that factory, so once ANY PProcess in the process has been
 * destroyed, no later load can add a single audio capability - and the module
 * load case's capability assertions would then be measuring the harness's own
 * ordering rather than mod_h323.  A harness-owned PProcess in the parent could
 * be left alive instead of destroyed, but then the module's own unconditional
 * `h323_process = new FSProcess()` (mod_h323.cpp:167) would be the second live
 * PProcess, which PTLib does not permit.
 *
 * Running the configuration-absent branch in a SEPARATE PROCESS IMAGE dissolves
 * the collision instead of trading one requirement against the other: the helper
 * brings up its own PProcess in its own address space and exits, the parent's
 * PTLib factories are never touched, and the parent's first and only PProcess is
 * still the one the module creates.  The case is declared first, and the module
 * load that follows it still observes fully populated factories.
 *
 * HOW THE CHILD IS ISOLATED: fork() IMMEDIATELY FOLLOWED BY exec()
 * ---------------------------------------------------------------
 * By the time any case body runs, this process has already started the
 * FreeSWITCH core, so it is MULTI-THREADED.  fork() duplicates that address
 * space into a child with exactly one thread, and every lock another thread
 * happened to hold at the instant of the fork is duplicated in the HELD state
 * with no owner left to release it.  The child then deadlocks the first time it
 * needs one.  That is not hypothetical here: the core's log queue, the memory
 * pool allocator and PTLib's internal mutexes are all live, and the
 * configuration-absent body needs all three.  POSIX permits only
 * async-signal-safe functions between fork() and exec in a multi-threaded
 * process, and allocating, logging, parsing XML or constructing a C++ object is
 * none of those things.
 *
 * So the forked image DOES NOTHING BUT EXEC.  Between fork() and exec it
 * performs exactly two calls, alarm() and execve(), both on POSIX's
 * async-signal-safe list, plus _exit() on the single path where execve fails.
 * No allocation, no logging, no XML, no PTLib, no C++ construction, no locking.
 * execve() then replaces the address space wholesale, which discards every
 * inherited lock, every inherited thread state and every inherited stdio buffer
 * in one step: there is nothing left to deadlock on.
 *
 * The image exec'd is THIS SAME BINARY, re-entered from main() and bootstrapped
 * from scratch - its own core, its own pools, its own threads, its own single
 * PProcess.  It is selected into helper mode by an ENVIRONMENT MARKER rather
 * than an extra argv entry, because FCTX's command-line parser reads a bare
 * positional argument as a test-name filter and exits on an unrecognised option
 * (switch_fct.h fctkern__cl_parse), so argv is not ours to extend.  The helper's
 * argv is therefore exactly one element, the program path, and the marker
 * travels in the environment.
 *
 * Everything execve() needs - path, argv and envp - is built in the PARENT
 * before the fork, precisely so that the child needs no allocator to reach exec.
 * The parent releases the plan on every path, including immediately after a
 * successful fork: the child has its own copy of that memory, so freeing it in
 * the parent cannot affect the exec.
 *
 * The watchdog is unchanged in spirit and stronger in reach.  alarm() is armed
 * before the exec and a pending alarm SURVIVES an exec - it is a per-process
 * timer, not a signal handler - so the helper inherits the watchdog covering its
 * whole life including its own bootstrap, without having to arm one for itself.
 * The parent's deadline-and-kill remains the independent backstop.
 *
 * TWO CORES IN ONE RUN DO NOT COLLIDE, and that is a property of the framework
 * rather than an arrangement made here.  Every FST core derives its log and
 * database directories from its own pid (switch_test.h:105 and :110), so the two
 * processes share neither, and FST_CORE_BEGIN passes no SCF_USE_SQL, so neither
 * opens a core database at all.  The helper's STANDARD OUTPUT is nonetheless
 * discarded: it runs the same FCTX driver, so its console output would interleave
 * with this run's and corrupt the collected result.  Its standard ERROR is kept,
 * because that is where FST_CORE_BEGIN writes when a core fails to come up at all
 * (switch_test.h:296-298), and losing that message would turn a diagnosable
 * failure into a bare exit code.
 *
 * WHY THE RESULT IS ENCODED AS AN EXIT CODE
 * -----------------------------------------
 * FCTX's assertion state lives in the parent, so an fst_check() evaluated in the
 * helper would be recorded in a counter that dies with it.  The helper therefore
 * makes its checks as plain comparisons and reports ONE distinguishing exit code
 * per outcome; the parent turns those codes back into assertions with messages.
 * The codes are deliberately above the range a signal or a libc failure would
 * produce, so an unexpected value is unambiguous.
 *
 * The helper still ends with _exit() rather than exit(), and still leaves its
 * PProcess standing: the address space is about to be reclaimed wholesale, and
 * running PTLib's global teardown or FCTX's would buy nothing but risk.
 */
#define FST_H323_CHILD_OK                 0	/* every child-side check held      */
#define FST_H323_CHILD_UNPINNED          40	/* plugin path not verifiably pinned */
#define FST_H323_CHILD_NO_PROCESS        41	/* PProcess did not come up          */
#define FST_H323_CHILD_NO_ENDPOINT       42	/* endpoint construction failed      */
#define FST_H323_CHILD_FIRST_NOT_FALSE   43	/* first ReadConfig() did not fail   */
#define FST_H323_CHILD_FIRST_LISTENERS   44	/* first ReadConfig() left a listener */
#define FST_H323_CHILD_SECOND_NOT_FALSE  45	/* repeat ReadConfig() did not fail  */
#define FST_H323_CHILD_SECOND_LISTENERS  46	/* repeat left a listener            */
#define FST_H323_CHILD_EXEC_FAILED       47	/* fork succeeded, execve() did not  */

/*
 * The environment marker that selects helper mode, and the one mode this suite
 * defines.  The name is deliberately specific enough that nothing else can
 * collide with it, and the value names what the helper is for, so a marker
 * carrying anything else is treated as NOT helper mode rather than as a request
 * this binary does not understand.
 */
#define FST_H323_HELPER_ENV              "FST_MOD_H323_ISOLATED_HELPER"
#define FST_H323_HELPER_READCONFIG       "readconfig-missing-config"

/*
 * The image to exec.  /proc/self/exe is preferred over argv[0] because it is
 * absolute, immune to the working directory, and immune to an argv[0] the
 * invoker rewrote - which matters concretely here, since this binary is normally
 * reached through a libtool wrapper script that does rewrite it.  argv[0] is the
 * portable fallback for a platform without /proc.
 */
#define FST_H323_SELF_EXE                "/proc/self/exe"

/*
 * Helper-side watchdog, and the parent's own deadline.  The parent's is the
 * longer of the two on purpose: the alarm armed before the exec is the primary
 * escape and the parent's kill is the backstop.  The backstop is not redundant -
 * an exec preserves an IGNORED signal disposition, so a SIGALRM that some other
 * part of the process had already set to be ignored would disarm the alarm
 * silently.  Two independent mechanisms mean neither has to be trusted alone.
 *
 * Both are more generous than they were when the child merely ran a short body
 * in an inherited image, because the helper now performs a FULL core bootstrap of
 * its own before it reaches the assertions.  Measured helper wall time on this
 * host is on the order of a second; sixty is the safety factor, not the
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
 * Presence of the marker WHATEVER ITS VALUE, which is a deliberately different
 * question from the one above.
 *
 * WHY TWO PREDICATES AND NOT ONE
 * ------------------------------
 * The exact-value test decides whether this process should RUN the helper body;
 * this presence test decides whether it is allowed to SPAWN one.  Keeping them
 * separate removes a single point of failure: recursion is impossible unless
 * BOTH are wrong at once.
 *
 * If the value test alone were used for both, a marker that failed to match -
 * because the value was mistyped, because a future mode name diverged, or
 * because the comparison itself regressed - would leave a process that is a
 * helper but does not know it, and it would spawn a helper of its own, which
 * would do the same.  The recursion is self-sustaining rather than bounded,
 * because each generation arms a fresh watchdog, so it is not a hazard the
 * deadline can absorb.  The refusal below is therefore keyed on presence, which
 * holds for ANY value the parent might have written.
 */
static int fst_h323_helper_marker_present(void)
{
	return getenv(FST_H323_HELPER_ENV) != NULL ? 1 : 0;
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
 * The environment is copied entry by entry, DROPPING any inherited marker of the
 * same name whatever its value, so helper mode can neither be inherited by
 * accident nor be stale, and the marker this run wants is appended last.  Every
 * allocation is checked, because this runs under the CI static analyser as well
 * as the sanitizer.
 */
static int fst_h323_exec_plan_build(fst_h323_exec_plan_t * plan, const char *argv0, const char *mode)
{
	extern char **environ;
	const char *chosen = NULL;
	char *marker = NULL;
	switch_size_t marker_len = 0;
	switch_size_t name_len = 0;
	switch_size_t count = 0;
	switch_size_t i = 0;
	switch_size_t out = 0;

	if (!plan || !mode) {
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

	/* +2 for the appended marker and the NULL terminator. */
	if (!(plan->envp = (char **) calloc(count + 2, sizeof(char *)))) {
		fst_h323_exec_plan_release(plan);
		return 0;
	}

	name_len = strlen(FST_H323_HELPER_ENV);
	marker_len = name_len + strlen(mode) + 2;

	if (!(marker = (char *) malloc(marker_len))) {
		fst_h323_exec_plan_release(plan);
		return 0;
	}

	switch_snprintf(marker, marker_len, "%s=%s", FST_H323_HELPER_ENV, mode);

	for (i = 0; i < count; i++) {
		if (!strncmp(environ[i], FST_H323_HELPER_ENV "=", name_len + 1)) {
			continue;
		}

		if (!(plan->envp[out] = strdup(environ[i]))) {
			free(marker);
			fst_h323_exec_plan_release(plan);
			return 0;
		}

		out++;
	}

	plan->envp[out++] = marker;
	plan->envp[out] = NULL;

	return 1;
}

/*
 * The whole of the configuration-absent assertion set, evaluated in the HELPER.
 *
 * Runs in a freshly exec'd process with a core of its own, returns the exit code
 * the parent will decode, and touches no state the parent can observe.  Because
 * the helper is a real bootstrap rather than a duplicated image, everything here
 * is ordinary code against a healthy process - there is no inherited-lock
 * hazard to reason about and no restriction on what it may call.
 *
 * The endpoint is deleted on every path that constructed it, so the body is
 * clean under a static analyser even though _exit() would have reclaimed it
 * anyway.  The helper's PProcess is deliberately NOT released: the address space
 * is about to be discarded wholesale, and running PTLib's global teardown would
 * buy nothing but risk.
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
 * Run fst_h323_readconfig_child_body() in a SEPARATELY BOOTSTRAPPED helper
 * process and report what happened to it.
 *
 * Returns SWITCH_STATUS_SUCCESS when the helper exited normally, writing its exit
 * code to *code; SWITCH_STATUS_TIMEOUT when it had to be killed for exceeding the
 * parent's deadline; SWITCH_STATUS_FALSE when the plan could not be built, the
 * fork or the wait failed, or the helper died on a signal - in which case *sig
 * carries the terminating signal, SIGALRM being the watchdog firing.  *helper_pid
 * receives the pid whenever one was created, so the caller can identify - and on a
 * clean run remove - the helper's own pid-named log directory.
 *
 * The wait is a bounded poll rather than a blocking waitpid() so that no failure
 * mode of the helper can hang the suite: one that wedges before the alarm can
 * fire, or that inherited an ignored SIGALRM, is killed at the deadline and
 * reaped.
 */
static switch_status_t fst_h323_run_readconfig_isolated(const char *argv0, int *code, int *sig, pid_t *helper_pid)
{
	fst_h323_exec_plan_t plan;
	int devnull = -1;
	pid_t pid = -1;
	pid_t reaped = 0;
	int status = 0;
	int waited_ms = 0;
	switch_status_t result = SWITCH_STATUS_FALSE;

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

	/* Built BEFORE the fork: the child must not need the allocator. */
	if (!fst_h323_exec_plan_build(&plan, argv0, FST_H323_HELPER_READCONFIG)) {
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

		return SWITCH_STATUS_FALSE;
	}

	if (pid == 0) {
		/*
		 * ASYNC-SIGNAL-SAFE REGION - DO NOT ADD ANYTHING TO THIS BLOCK.
		 *
		 * Exactly four calls, all on POSIX's async-signal-safe list: dup2(),
		 * alarm(), execve() and - only where execve failed - _exit().  Nothing here
		 * allocates, locks, logs or constructs, which is the entire reason this
		 * fork is safe in a process whose core threads are already running.  The
		 * descriptor dup2() needs was opened by the parent before the fork, so the
		 * child never calls open() either.
		 *
		 * Only STANDARD OUTPUT is discarded, and only because the helper runs the
		 * same FCTX driver, so its console output would otherwise interleave with
		 * this run's and corrupt the collected result.  Standard error is left
		 * alone: that is where FST_CORE_BEGIN writes when a core fails to come up at
		 * all (switch_test.h:296-298), and losing it would turn a diagnosable
		 * failure into a bare exit code.
		 *
		 * The alarm is armed before the exec on purpose: a pending alarm survives
		 * an exec, so it covers the helper's own bootstrap as well as its body.
		 *
		 * _exit(), never exit(): if the exec fails, no inherited atexit handler
		 * and no inherited stdio buffer may run in this duplicated image.
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
		devnull = -1;
	}

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
 * Every FST core composes its log and database directory from its own pid under
 * the test's base directory (switch_test.h:105 and :110).  A second core in the
 * run therefore leaves a second directory behind, and cleaning that one up is
 * what keeps this design residue-neutral: the run leaves exactly the one
 * directory a single-core suite leaves, not two.
 *
 * Its contents are known and small.  The core opens no database here, because
 * FST_CORE_BEGIN passes no SCF_USE_SQL (switch_test.h:313-314), so the only
 * artefact is the preprocessed configuration the XML reader writes into the log
 * directory as "<conf-file>.fsxml", with a sibling ".tmp" should a write have
 * been interrupted (src/switch_xml.c:1743-1748).  Both names are removed
 * explicitly and then the directory itself, so the cleanup is bounded by name
 * rather than recursive - a test has no business deleting a tree it did not
 * enumerate, and anything unexpected keeps the directory alive and visible.
 *
 * It runs ONLY when the helper reported success.  On any other outcome those
 * same files are the primary evidence for what went wrong, so they are
 * deliberately preserved and the caller reports where they are.
 */
static void fst_h323_remove_helper_dir(pid_t helper_pid)
{
	char dir[1024] = "";
	char path[1152] = "";

	if (helper_pid <= 0) {
		return;
	}

	switch_snprintf(dir, sizeof(dir), "%s%s%lu", SWITCH_TEST_BASE_DIR_OVERRIDE, SWITCH_PATH_SEPARATOR, (unsigned long) helper_pid);

	switch_snprintf(path, sizeof(path), "%s%s%s", dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml.fsxml");
	(void) unlink(path);

	switch_snprintf(path, sizeof(path), "%s%s%s", dir, SWITCH_PATH_SEPARATOR, "freeswitch.xml.fsxml.tmp");
	(void) unlink(path);

	(void) rmdir(dir);
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
 * WHY IT IS CALLED AFTER Initialise() TOO
 * ---------------------------------------
 * H323EndPoint::StartListener(H323Listener *) documents a conditional transfer:
 * "if this returns TRUE, then the endpoint is responsible for deleting the
 * H323Listener listener object.  If FALSE is returned then the object is not
 * deleted and it is up to the caller to release the memory" (h323ep.h:532-545).
 * That transfer never happens in this suite, because every StartListener() call
 * site is retargeted onto fst_h323_start_listener(), which records the request
 * and adopts nothing.  Ownership therefore stays in m_listeners for EVERY case,
 * Initialise() or not, and this helper is the single release path for all of
 * them.  One owner, one release, so a double free is structurally impossible -
 * which is a stronger guarantee than the split rule it replaces, where a case
 * released or did not release depending on how far into production it had run.
 *
 * The Initialise() cases pair the call with an assertion that the endpoint's
 * own H323ListenerList (h323ep.h:2027, :3114) is EMPTY, so "the endpoint owns
 * nothing" is proven rather than assumed before this helper frees anything.
 *
 * The returned count is the ownership verdict itself: a case asserts the number
 * released equals the number its document declared, which fails if production
 * dropped a record, if a listener was never constructed, or if something else
 * had already taken it.
 *
 * FSH323EndPoint is taken rather than the test subclass because m_listeners is
 * public on the production class (mod_h323.h:270): that lets the module-load
 * case drain the endpoint the MODULE built, reached through the public
 * FSProcess::GetH323EndPoint() (mod_h323.h:230-232), with the same helper the
 * direct-object cases use.
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
 * Total, idempotent reclamation of everything this suite can retain BETWEEN
 * cases: the registered h323.conf provider, the loaded module's own state, any
 * harness-owned fallback PProcess, and the module-lifetime pool.
 *
 * WHY EVERY STEP IS IDEMPOTENT
 * ---------------------------
 * switch_xml_unbind_search_function_ptr() reports SWITCH_STATUS_FALSE when no
 * binding carries the pointer and changes nothing (src/switch_xml.c:315-338).
 * mod_h323_shutdown() frees its four globals through switch_safe_free(), which
 * NULLs each pointer after freeing it (src/include/switch_utils.h:881), then
 * deletes the process and NULLs that too (mod_h323.cpp:188-199) - so it is
 * safe whether the module was loaded, never loaded, or already shut down.  The
 * process release and the pool destroy are both guarded on their own handles.
 * The sweep can therefore be called at any point, any number of times.
 *
 * WHY IT IS NOT INVOKED FROM FST_TEARDOWN
 * --------------------------------------
 * FST_TEARDOWN runs after EVERY case, and this suite deliberately hands state
 * from one case to those after it: the module-load case leaves the
 * module loaded, its PProcess alive and its pool alive ON PURPOSE, because the
 * cases that follow are meant to observe a loaded module and the last of them
 * exists to assert that shutting it down works.  An unconditional per-case
 * teardown would demolish exactly the state those cases must observe.
 * Reclamation is anchored instead where it is both unconditional and correct: a
 * single cleanup tail in every case that retains anything, plus this total sweep
 * in the last declared case.
 *
 * Two framework facts make that placement sound.  A fatal check merely breaks
 * out of the enclosing test body - fct_req expands to `if (!ok) { break; }`
 * (switch_fct.h:3668-3669) - and FCTX re-enters the whole fixture-suite body
 * once per declared case, running only the case whose number matches
 * (switch_fct.h:3507-3516).  So a fatal check in one case can skip that case's
 * own tail but never a later case, which is why the last case is a dependable
 * safety net; and why no case below may make a fatal check after mutating state
 * that outlives it.
 *
 * ORDER IS LOAD-BEARING
 * ---------------------
 *   1. unbind the provider first, so nothing that follows can trigger a
 *      configuration lookup that re-enters it;
 *   2. shut the module down next - that is the last thing which reads the
 *      module state carved out of the pool, and the only thing that destroys
 *      the PProcess the module created;
 *   3. release a harness-owned fallback process should one exist, AFTER
 *      shutdown, so the one-live-PProcess invariant is never breached.  In a
 *      healthy run this is a no-op, because the harness owns no process;
 *   4. destroy the module-lifetime pool once nothing points into it;
 *   5. release any root pool the module abandoned, last.  Ordered last purely
 *      so that it also covers anything the four steps above could themselves
 *      have caused to be recorded; in a healthy run the per-case teardown has
 *      already emptied the list and this releases nothing.
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
 *   1. readconfig_without_configuration_fails ....... FIRST: the
 *      configuration-absent branch, so its verdict cannot be an artefact of
 *      anything that ran before it.  Executed in a SEPARATELY EXEC'D HELPER
 *      PROCESS, which is what lets it be first without disturbing this process -
 *      see below.
 *   2. module_load_registers_endpoint_interface ..... the full module load, and
 *      the first case to bring a PProcess up in THIS address space, so the
 *      H323CapabilityFactory and the OpalMediaFormat registry are still
 *      populated and the module is observed fully initialised rather than
 *      degraded.  It leaves the module loaded for everything that follows.
 *   3. gatekeeper_registration_disabled_when_gk_address_empty ... runs the real
 *      Initialise() so the no-registration guard is executed, then asserts the
 *      toolkit was never asked for a gatekeeper.
 *   4. gatekeeper_lan_search_address_preserved_verbatim ... drives
 *      StartGkClient() directly and asserts the "*" LAN-search request reached
 *      the toolkit.
 *   5. listeners_parsed_from_configuration ........... ReadConfig() only, and
 *      the counterpart to cases 3 and 6: it asserts that parsing a listener
 *      does NOT start one.
 *   6. codec_prefs_negotiation_order ................. the second and last case
 *      to run Initialise() on an endpoint of its own.
 *   7. module_shutdown_releases_resources ............ LAST: observes the module
 *      case 2 loaded and reclaims everything it allocated.
 *
 * WHY CASE 1 RUNS IN A PROCESS OF ITS OWN RATHER THAN SIMPLY BEING DECLARED FIRST
 * ------------------------------------------------------------------------------
 * PProcess::~PProcess() irreversibly empties both PTLib factories, so whichever
 * capability-building code runs after a PProcess has been destroyed sees nothing
 * to build from.  Case 1 cannot avoid bringing a PProcess up - H323EndPoint's
 * constructor calls PProcess::Current() and terminates the binary when no
 * process exists - and it cannot leave one alive either, because the module's
 * own load unconditionally constructs a second, which PTLib does not permit.
 * Both requirements are satisfied by giving case 1 its own address space: the
 * helper's process comes and goes without touching the parent's factories, and
 * the parent's first and only PProcess is still the module's.  That address
 * space is a freshly exec'd image of this binary rather than a fork of it, for
 * the async-signal-safety reason set out at fst_h323_run_readconfig_isolated();
 * case 1 asserts the property as well as relying on it, checking that the parent
 * still owns no PProcess afterwards.
 *
 * The alternative - accepting the reverse order, with a direct-object case's
 * process destroyed before the load - makes the module load add no audio
 * capability at all, which is an artefact of harness ordering rather than a
 * property of mod_h323 and is precisely what this arrangement exists to avoid.
 *
 * Cases 3-6 construct endpoints of their own but never a second process: they
 * adopt the live one through fst_h323_process_acquire().  Two H323EndPoint
 * objects may coexist - the class is not a singleton and its constructor binds
 * nothing - and no port is contended for because no port is ever bound: every
 * listener hand-off is intercepted.  The distinct port per document is there so
 * each case can assert the address ITS document configured, not to keep two
 * binds apart.  At no instant are two PProcess-derived objects alive.
 */
FST_CORE_BEGIN("conf_h323")
{
	FST_SUITE_BEGIN(mod_h323)
	{
		/*
		 * This hook must be PRESENT.  FST_CORE_BEGIN sets fst_core == 2, and
		 * every FST_TEST_BEGIN then fatally requires both fst_pool and a started
		 * fst_timer (switch_test.h:451-453).  FST_SETUP_BEGIN is the only thing
		 * that creates them (switch_test.h:407-412), so omitting it would make
		 * every case below fail fatally.
		 *
		 * Its body pins PTLib's plugin search path.  This is the earliest point
		 * guaranteed to run before ANY case body, which is what makes it the
		 * right place: PTLib enumerates and dlopen()s that directory while a
		 * PProcess comes up, so the containment has to be installed before the
		 * first construction, whichever case happens to cause it.  See
		 * fst_h323_pin_plugin_path() for the full argument.
		 *
		 * The result is deliberately discarded HERE and only here: a setup hook
		 * has no assertion vocabulary - FST_SETUP_BEGIN runs outside any test
		 * body, so a failed check would have nothing to attribute itself to - so
		 * this call is the early installation, not the guarantee.  The guarantee
		 * is enforced where it matters: fst_h323_process_acquire() re-verifies
		 * and returns NULL if it cannot, and every case reaches it through
		 * fst_requires().
		 *
		 * It also zeroes the toolkit observation record, so every case sees only
		 * the calls IT caused.  Doing it here rather than per case makes the
		 * isolation unconditional: a case cannot forget, and cannot inherit a
		 * count from a case that broke out early on a fatal check.
		 */
		FST_SETUP_BEGIN()
		{
			(void) fst_h323_pin_plugin_path();
			fst_h323_toolkit_reset();
		}
		FST_SETUP_END()

		/*
		 * Reclaims exactly one thing, and deliberately nothing else.
		 *
		 * WHAT IT RECLAIMS
		 * ----------------
		 * Every root pool the module abandoned during the case that just ran.
		 * FSH323EndPoint::ReadConfig() allocates one on entry and never destroys
		 * or uses it (mod_h323.cpp:469), so each case that reads configuration -
		 * five of the seven below, directly or through the module load - strands
		 * one.  The recording seam above captures them; this is where they are
		 * released.
		 *
		 * This hook is the right anchor for that and only that, because a
		 * recorded pool is the one resource in this suite that no case owns and
		 * no later case reads: nothing in mod_h323.cpp holds a pointer derived
		 * from it, and nothing in this file does either.  Releasing it after
		 * every case is therefore unconditional and cannot demolish anything -
		 * and being here rather than in the case bodies means it happens even
		 * when a fatal precondition breaks a case out early, which is precisely
		 * when a case's own tail would be skipped.
		 *
		 * WHAT IT DELIBERATELY DOES NOT RECLAIM
		 * -------------------------------------
		 * Everything else.  There is nothing here to unload: the harness never
		 * dlopens or dlcloses mod_h323, it calls the module's own entry points by
		 * name.  And no module state may be reclaimed here, because this hook
		 * runs after EVERY case while the suite deliberately hands the loaded
		 * module from the second case to every case after it - see
		 * fst_h323_suite_state_cleanup() for that argument in full.  Note also
		 * that FST_TEARDOWN_BEGIN destroys fst_pool before this body is entered
		 * (switch_test.h:425-432), so nothing backed by fst_pool could be
		 * released here in any case.
		 *
		 * What makes that safe is a structural rule the cases below keep, stated
		 * exactly.  NO case makes a fatal check after registering the
		 * configuration provider or after creating the module-lifetime pool -
		 * those are the two resources nothing else would reclaim, so every check
		 * that follows either of them is non-fatal and control always reaches
		 * that case's single cleanup tail instead of breaking out to this hook.
		 *
		 * Two things a fatal check can still strand, and why neither matters.  A
		 * harness-owned fallback PProcess, which only exists at all when the
		 * module load did not happen: the direct-object cases acquire before
		 * checking that PTLib came up, so a break there retains it - and the last
		 * case's sweep releases it.  And anything allocated from fst_pool, which
		 * FST_TEARDOWN_BEGIN destroys on the way in.
		 *
		 * The release count is discarded HERE and only here, for the same reason
		 * the setup hook discards its result: a teardown body runs outside any
		 * test's assertion scope, so a check made here would have no case to
		 * attribute itself to.  The property is asserted where it is genuinely
		 * attributable instead - the case that reads configuration checks that
		 * the seam recorded a pool, and the last declared case checks that none
		 * survives and that the recorder never overflowed.
		 */
		FST_TEARDOWN_BEGIN()
		{
			(void) fst_h323_release_recorded_pools();
		}
		FST_TEARDOWN_END()

		/*
		 * CASE 1 - the configuration-absent failure branch.  DECLARED FIRST, and
		 * RUN IN A PROCESS OF ITS OWN.
		 *
		 * Asserted on ReadConfig() directly, and deliberately NOT on
		 * mod_h323_load(): FSH323EndPoint::Initialise() discards ReadConfig()'s
		 * status (mod_h323.cpp:381) and returns TRUE unconditionally
		 * (mod_h323.cpp:457), so mod_h323_load() can never report a
		 * configuration failure and an assertion on it would prove nothing.
		 *
		 * DECLARED FIRST so its verdict cannot be an artefact of anything that ran
		 * before it: no module has been loaded, no configuration provider has ever
		 * been registered, and the parent process has never brought a PProcess up.
		 *
		 * ISOLATED because being first and bringing a PProcess up are mutually
		 * exclusive inside one address space - see
		 * fst_h323_run_readconfig_isolated() for the full argument, in short that
		 * ~PProcess() would irreversibly empty the two PTLib factories the module
		 * load case depends on.  The helper makes the assertions; this body asserts
		 * on the helper's outcome AND on the parent state the isolation is there to
		 * protect.
		 *
		 * INDEPENDENT OF THE LOADED MODULE, deliberately.  The subject is an
		 * endpoint the helper constructs itself, no provider is registered, and
		 * mod_h323 registers no XML search function of its own - so the lookup
		 * misses in both the binding list and the static root regardless of
		 * whether a module is loaded, which is what makes the failure branch
		 * deterministic.
		 *
		 * Socket-free: ReadConfig() only CONSTRUCTS H323ListenerTCP objects
		 * (mod_h323.cpp:583); OpenH323 binds in H323ListenerTCP::Open(), which
		 * is reached from H323EndPoint::StartListener() and which ReadConfig()
		 * never calls.  On this path nothing is even constructed.
		 *
		 * m_pi, m_ai and m_endpointname are deliberately NOT asserted on this
		 * path: their defaults are applied at mod_h323.cpp:490-493, AFTER the
		 * failure return, and the constructor (mod_h323.cpp:599-610) leaves the two
		 * ints uninitialised.  The context and dialplan defaults applied at
		 * mod_h323.cpp:474-475 are likewise NOT asserted: they live in
		 * mod_h323_globals, a .cpp-file static (mod_h323.cpp:42) whose only setters
		 * are the file-static SWITCH_DECLARE_GLOBAL_STRING_FUNC wrappers
		 * (mod_h323.cpp:44-47), so no public seam exposes them.  An assertion on
		 * them would rest purely on this suite's internal linkage to the module
		 * translation unit, and asserting through internal linkage is what makes a
		 * harness brittle: it breaks on a refactor that changes nothing observable.
		 * The stronger property is asserted on the public seams instead - the
		 * failure branch is deterministic and leaves no residue, so the helper
		 * repeats it and requires the identical observable result.
		 */
		FST_TEST_BEGIN(readconfig_without_configuration_fails)
		{
			switch_status_t isolated = SWITCH_STATUS_FALSE;
			int child_code = -1;
			int child_signal = 0;
			pid_t helper_pid = -1;

			/*
			 * ---------------------------------------------------------------
			 * THE HELPER'S OWN ENTRY POINT.
			 * ---------------------------------------------------------------
			 *
			 * When this process IS the exec'd helper, the whole of its work is
			 * the body below and its whole result is an exit code.  It is placed
			 * in the FIRST declared case because FCTX runs cases in declaration
			 * order, so _exit()ing here guarantees no later case ever runs in the
			 * helper: the helper cannot load the module, cannot construct a
			 * second PProcess, and cannot report a verdict of its own into the
			 * parent's tally.
			 *
			 * _exit() rather than return, deliberately: returning would run
			 * FST_CORE_END's switch_core_destroy() and then FCTX's final report,
			 * and the helper has no business tearing a core down or printing a
			 * summary that the parent will print properly a moment later.  It
			 * also means no atexit handler and no leak-sanitizer at-exit check
			 * fires in a process that is mid-suite by construction.
			 *
			 * This is reached AFTER the setup hook, so fst_pool and the plugin
			 * pin are already in place exactly as they are for any other case.
			 *
			 * No alarm is armed here: the parent armed one immediately before the
			 * exec and a pending alarm survives an exec, so this image is already
			 * covered - including through its own core bootstrap, which happened
			 * before this line was reached.
			 */
			if (fst_h323_in_helper_mode()) {
				fflush(NULL);
				_exit(fst_h323_readconfig_child_body());
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
				 * nothing more, so it goes. */
				fst_h323_remove_helper_dir(helper_pid);
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
		 * CASE 2 - a configuration-present module load registers the endpoint
		 * interface.
		 *
		 * mod_h323_load() is the module's own entry point, reached by name
		 * rather than through a dlopen: SWITCH_MODULE_LOAD_FUNCTION expands to
		 * a plain definition with no storage class and mod_h323.cpp declares it
		 * inside SWITCH_BEGIN_EXTERN_C, so it has C linkage and external
		 * visibility.
		 *
		 * THE FIRST CASE TO BRING A PProcess UP, AND THAT IS WHY THE MODULE IS
		 * OBSERVED FULLY INITIALISED.  Case 1 ran its own process in a separately
		 * exec'd helper precisely so that this remains true: no PProcess has ever been
		 * constructed or destroyed in THIS address space, so the
		 * H323CapabilityFactory and the OpalMediaFormat registry are still fully
		 * populated and the Initialise() inside the load builds a real capability
		 * table, which this case then asserts.  Had any PProcess been created and
		 * destroyed here beforehand, PostShutdown() would have emptied both
		 * factories for good and the load could have added no audio capability at
		 * all: an artefact of harness ordering, and an intentionally degraded
		 * subject to assert against.  See the case-order note above the suite.
		 *
		 * The binding must be registered BEFORE the call, because the load
		 * path reads the configuration itself.  This case leaves the module
		 * loaded on purpose: every case after it observes a loaded module, and
		 * the last asserts that shutting it down works.
		 */
		FST_TEST_BEGIN(module_load_registers_endpoint_interface)
		{
			switch_loadable_module_interface_t *module_interface = NULL;
			switch_status_t status = SWITCH_STATUS_FALSE;
			switch_status_t bound = SWITCH_STATUS_FALSE;
			switch_status_t pooled = SWITCH_STATUS_FALSE;
			FSProcess *process = NULL;
			char capabilities[2048];

			/* Defensive: release a harness-owned fallback process should one
			 * somehow exist.  A no-op in a healthy run - case 1 runs its process
			 * in a separately exec'd helper and the parent harness owns none - and it keeps
			 * this case from ever asking PTLib for a second live PProcess. */
			fst_h323_process_release();

			/* This is where the run's first PProcess in this address space comes
			 * up - and therefore the point at which PTLib's plugin enumeration is
			 * either contained or not.  Asserted before the load below, and
			 * fatally: a suite that went on to dlopen an inherited plugin
			 * directory must not run at all. */
			fst_requires(fst_h323_plugin_path_is_pinned());

			/* THE LAST FATAL CHECK IN THIS CASE, and it is made before anything
			 * has been registered or allocated, so breaking out here leaves
			 * nothing behind - the defensive release above has just run.  It
			 * stays fatal on purpose: a second live PProcess would make PTLib
			 * abort inside the load below, so refusing to continue is the only
			 * safe response.  It is also the strongest available statement that no
			 * factory has been torn down yet - no PProcess has ever existed in
			 * this process image - and it holds despite case 1 running earlier
			 * precisely because case 1 ran its process in a separately exec'd
			 * helper. */
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

				/*
				 * LISTENER START AND OWNERSHIP VERDICT FOR THE LOADED MODULE.
				 *
				 * The injected loopback listener was the one handed over, so the
				 * wildcard fallback on 0.0.0.0:1720 was not taken, and the
				 * address production asked to listen on is the one THIS
				 * document configured.  The module's own endpoint then holds no
				 * toolkit listener, which is the positive proof that loading the
				 * module bound no port at all.
				 */
				fst_check_int_equals((int) endpoint.m_listeners.size(), 1);
				fst_check_int_equals(fst_h323_toolkit.listener_calls, 1);
				fst_check_int_equals(fst_h323_toolkit.listener_nulls, 0);
				fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
				fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_LOOPBACK);
				fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_PORT_MODULE_LOAD);
				fst_check_int_equals((int) endpoint.GetListeners().GetSize(), 0);

				/* This document leaves gk-address empty, so loading the module
				 * asked the toolkit for no gatekeeper either. */
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
				 * The module's listener is released HERE, by the case that
				 * caused it to be constructed, and the count is its ownership
				 * verdict.  It cannot be left to shutdown: mod_h323_shutdown()
				 * deletes the FSProcess (mod_h323.cpp:195), whose destructor
				 * deletes the endpoint (mod_h323.cpp:366-368), and
				 * ~FSH323EndPoint touches only StopGkClient() and
				 * ClearAllCalls() (mod_h323.cpp:612-617) - it never walks
				 * m_listeners.  With the hand-off intercepted, the base
				 * endpoint's list is empty too (asserted above), so nothing
				 * downstream would ever free it.
				 *
				 * Draining it now is safe for every case that follows: they read
				 * the module's interfaces, capabilities and process, never its
				 * listener records.
				 *
				 * The count is hoisted into a local before it is asserted, and
				 * that is MANDATORY rather than tidy: fst_check_int_equals
				 * expands its first argument twice (switch_fct.h:3845-3851), so
				 * calling a releasing function inside it would release once,
				 * report zero on the second evaluation, and fail an assertion
				 * that is actually true.  No side-effecting expression appears
				 * as an argument to any check macro in this file.
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
			 * and the last of them asserts that shutting the module down
			 * works. */
		}
		FST_TEST_END()

		/*
		 * CASE 3 - an empty gk-address leaves gatekeeper registration
		 * disabled.
		 *
		 * Runs the real Initialise(), so the production guard at
		 * mod_h323.cpp:451 is EXECUTED rather than reasoned about, and both of
		 * its observable consequences - no registration thread and no toolkit
		 * gatekeeper call - are asserted.  Socket-free all the same: the
		 * listener hand-off is intercepted by the double, so nothing is opened.
		 */
		FST_TEST_BEGIN(gatekeeper_registration_disabled_when_gk_address_empty)
		{
			FSH323TestEndPoint *endpoint = NULL;
			switch_loadable_module_interface_t *module_interface = NULL;
			bool initialised = false;
			int released = 0;

			fst_requires(fst_h323_process_acquire() != NULL);

			/* Initialise() registers an endpoint interface
			 * (mod_h323.cpp:388-391), so it needs a real module interface out of
			 * a real pool. */
			module_interface = switch_loadable_module_create_module_interface(fst_pool, "mod_h323_test_gk_disabled");
			fst_requires(module_interface != NULL);

			fst_requires(fst_h323_bind_config(fst_h323_conf_gk_disabled) == SWITCH_STATUS_SUCCESS);

			endpoint = new FSH323TestEndPoint();

			fst_check(endpoint != NULL);

			/*
			 * THE DECISION IS EXECUTED, NOT INFERRED.
			 *
			 * Initialise() reads the configuration itself (mod_h323.cpp:381) and
			 * then evaluates the gatekeeper guard at mod_h323.cpp:451.  Stopping
			 * at ReadConfig() would leave m_thread NULL whether or not that
			 * guard works - a pre-initialisation NULL is NULL for the trivial
			 * reason that nothing has run yet - so it would assert nothing about
			 * the guard.  Running Initialise() is what makes the NULL below
			 * evidence.
			 */
			initialised = endpoint->Initialise(module_interface);

			/* Initialise() returns TRUE unconditionally (mod_h323.cpp:457) */
			fst_check(initialised == true);

			/* An explicitly empty gk-address is stored empty ... */
			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check_string_equals((const char *) endpoint->TestGetGkAddress(), "");
			fst_check(endpoint->TestGetGkIdentifer().IsEmpty());
			fst_check(endpoint->TestGetGkInterface().IsEmpty());

			/* ... and registration is therefore never initiated.  The
			 * !m_gkAddress.IsEmpty() guard at mod_h323.cpp:451 short-circuited
			 * during the call above, so no FSGkRegThread was constructed or
			 * resumed - which is what makes this case free of network side
			 * effects even though it now runs the real decision. */
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);

			/* THE POSITIVE PROOF.  The toolkit was never asked to register with,
			 * or search for, a gatekeeper.  A NULL thread pointer alone cannot
			 * establish that; this can, because the sole UseGatekeeper() call
			 * site (mod_h323.cpp:663) records every invocation. */
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);
			fst_check_string_equals(fst_h323_toolkit.gk_address, "");

			/* endpoint-name was omitted from this document, so the hard-coded
			 * default at mod_h323.cpp:492 stands */
			fst_check_string_equals((const char *) endpoint->TestGetEndpointName(), "FreeSwitch");

			/* the interface was registered on the way through Initialise() */
			fst_check(endpoint->GetSwitchInterface() != NULL);

			/*
			 * LISTENER START AND OWNERSHIP VERDICT.
			 *
			 * The document's single listener stanza was parsed, was handed to
			 * StartListener() exactly once carrying the address and port THIS
			 * document configured, and the empty-list default-interface fallback
			 * was not taken.  The endpoint's own listener list is then empty,
			 * which is the positive proof that no socket was opened and that the
			 * endpoint owns nothing.
			 */
			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);
			fst_check_int_equals(fst_h323_toolkit.listener_calls, 1);
			fst_check_int_equals(fst_h323_toolkit.listener_nulls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_LOOPBACK);
			fst_check_string_has(fst_h323_toolkit.listener_address[0], FST_H323_PORT_GK_DISABLED);
			fst_check_int_equals((int) endpoint->GetListeners().GetSize(), 0);

			/* Ownership never transferred, so this case releases it, and the
			 * returned count is the verdict that exactly the one declared
			 * listener was accounted for. */
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
		 * CASE 4 - gk-address "*" REQUESTS A LAN GATEKEEPER SEARCH, the sentinel
		 * documented at h323.conf.xml:10 ("empty to disable, \"*\" to search
		 * LAN").
		 *
		 * The stored value is asserted, and then so is the decision it drives:
		 * StartGkClient() is executed and the toolkit request it makes is
		 * observed through the double.  Storage alone would not distinguish a
		 * working sentinel from a string that is copied and then ignored.
		 *
		 * WHY StartGkClient() IS CALLED DIRECTLY RATHER THAN THROUGH Initialise()
		 * ---------------------------------------------------------------------
		 * Not for convenience - Initialise() is unusable here, for two
		 * independent production reasons.
		 *
		 * First, a non-empty gk-address makes Initialise() construct an
		 * FSGkRegThread, SetAutoDelete() it and Resume() it (mod_h323.cpp:451-455).
		 * The suite would then be observing a self-deleting thread it cannot
		 * join, and every assertion about it would be a race.
		 *
		 * Second, and decisively: StartGkClient()'s early return clears
		 * m_stop_gk but does NOT clear m_thread (mod_h323.cpp:670-673 - only the
		 * normal exit at :686 nulls it).  So after that thread has run and
		 * deleted itself, m_thread still points at freed memory, and
		 * ~FSH323EndPoint -> StopGkClient() sees a non-NULL m_thread, sets
		 * m_stop_gk and spins in `while (m_stop_gk) { h_timer(2); }`
		 * (mod_h323.cpp:693-701) waiting for a thread that no longer exists.
		 * That is an UNBOUNDED HANG.  It is a pre-existing production defect,
		 * outside this engagement's remit to change, and this suite is careful
		 * not to trigger it: no case ever calls Initialise() with a non-empty
		 * gk-address, so m_thread is NULL at every destruction in this file.
		 *
		 * Calling StartGkClient() directly reaches the same UseGatekeeper()
		 * invocation with none of that: one call, no thread, no sleep, no RAS
		 * I/O, and a fully deterministic result.
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

			/* No listener was handed over, because Initialise() never ran */
			fst_check_int_equals(fst_h323_toolkit.listener_calls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);

			/* Nothing has asked the toolkit for a gatekeeper yet.  Asserted
			 * BEFORE the call so that the invocation counted afterwards can only
			 * have come from the StartGkClient() below. */
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			/*
			 * ARM THE PRODUCTION ABORT, THEN RUN THE DECISION.
			 *
			 * The double reports failure - it must, because the normal loop exit
			 * dereferences a NULL GetGatekeeper() (mod_h323.cpp:684-685) - so
			 * production enters its retry loop.  m_stop_gk set beforehand means
			 * the FIRST check inside that loop (mod_h323.cpp:670-673) returns,
			 * before h_timer() sleeps for gk-retry seconds and before
			 * RemoveGatekeeper() is reached.  One invocation, no sleep, no I/O.
			 *
			 * The retry argument must be > 0 or the loop is never entered at
			 * all, so the configured gk-retry is passed - the same value
			 * Initialise() would have passed (mod_h323.cpp:452).  The three
			 * PString pointers are the ones production passes; StartGkClient()
			 * ignores them and reads the members directly (mod_h323.cpp:663).
			 */
			endpoint->TestSetStopGk(true);
			fst_check(endpoint->TestGetStopGk() == true);

			endpoint->StartGkClient(endpoint->TestGetGkRetry(), NULL, NULL, NULL);

			/* THE LAN-SEARCH REQUEST ITSELF: the toolkit was asked exactly once,
			 * and the address it was asked with is the "*" sentinel verbatim -
			 * not expanded to a host, not normalised away, not dropped. */
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 1);
			fst_check_string_equals(fst_h323_toolkit.gk_address, "*");
			fst_check_int_equals((int) strlen(fst_h323_toolkit.gk_address), 1);

			/* and it carried the rest of the configured registration identity */
			fst_check_string_equals(fst_h323_toolkit.gk_identifier, "fst-gatekeeper");
			fst_check_string_equals(fst_h323_toolkit.gk_interface, "");

			/* Production consumed the abort flag on its way out, which is the
			 * observable proving the early-return branch at mod_h323.cpp:670-673
			 * is the one that ran - the normal exit at :686 would have left
			 * m_stop_gk set and nulled m_thread instead. */
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

			/* ReadConfig() allocates exactly one root pool on entry
			 * (mod_h323.cpp:469) and abandons it, so exactly one was recorded and
			 * is still held pending the teardown release. */
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

			/* still no gatekeeper activity: gk-address is empty here too, and
			 * nothing in this case asks the toolkit for one */
			fst_check(endpoint->TestGetGkAddress().IsEmpty());
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			/* ReadConfig() alone hands nothing to StartListener(): the two
			 * listeners exist as constructed, unopened objects and no start was
			 * ever attempted.  This is what separates "parsed" from "started",
			 * and it is asserted rather than left implicit. */
			fst_check_int_equals(fst_h323_toolkit.listener_calls, 0);
			fst_check_int_equals(fst_h323_toolkit.listener_default_calls, 0);
			fst_check_int_equals((int) endpoint->GetListeners().GetSize(), 0);

			/* This case constructs the most listeners of any, and owns every one
			 * of them: the released count must equal the two it declared.
			 * Hoisted because fst_check_int_equals expands its argument twice
			 * (switch_fct.h:3845-3851). */
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
		 * CASE 6 - the codec preference string "PCMA,PCMU,GSM,G729" is
		 * honoured in order.
		 *
		 * This case calls Initialise() on an endpoint of its own, because the
		 * capability table is built there (mod_h323.cpp:393-421); ReadConfig()
		 * only stores the preference string.  Its document pins the listener to
		 * 127.0.0.1 on FST_H323_PORT_CODEC_PREFS and leaves gk-address empty, so
		 * that the address the listener hand-off is asserted against is this
		 * case's own and not another's.  No socket is opened: the hand-off is
		 * intercepted by the double.
		 *
		 * It works because the module-load case created the one PProcess of this
		 * process and nothing has destroyed it, so both PTLib
		 * factories are still populated and AddAllCapabilities() has something
		 * to add.  That is the whole point of the declaration order.
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
			int released = 0;
			bool initialised = false;

			/* All fatal checks precede the binding registration, so a fatal
			 * exit can never orphan a binding into a later case.
			 *
			 * This case MUST run while the first PProcess of the process
			 * lifetime is still alive, and it does: acquire ADOPTS the module's
			 * process rather than creating a second, and nothing destroys a
			 * PProcess before the last declared case.  Once any PProcess has
			 * been destroyed the capability factory and the media-format registry
			 * are empty for good and AddAllCapabilities() can add nothing. */
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

			/* the configured loopback listener was the one started, so the
			 * empty-list wildcard fallback at mod_h323.cpp:441-442 was not
			 * taken, and gk-address is empty so no RAS thread exists and the
			 * toolkit was never asked for a gatekeeper */
			fst_check_int_equals((int) endpoint->m_listeners.size(), 1);
			fst_check(endpoint->TestGetGkRegistrationThread() == NULL);
			fst_check_int_equals(fst_h323_toolkit.gk_calls, 0);

			/* LISTENER START AND OWNERSHIP VERDICT, on the address THIS
			 * document configured rather than any other case's */
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
			 * module, which the module-load case loaded and the last case shuts down;
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
			 * inside the last declared case, keeps it out of the per-case
			 * teardown that owns fst_pool.
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

			/* The closing statement on the root-pool recording seam, made here
			 * because this is the last declared case and therefore the only point
			 * from which the whole run can be characterised.
			 *
			 * Three properties, each of which would be a real defect if it failed.
			 * The seam recorded something, so it was genuinely in force for the
			 * five cases that read configuration rather than silently bypassed.
			 * Nothing it recorded survives, so every pool the module abandoned was
			 * released and the process the sanitizer examines at exit is balanced.
			 * And the recorder never overflowed its fixed table, which is the one
			 * way a pool could have escaped the sweep unnoticed.
			 *
			 * The total is also logged, in the same spirit as the capability table
			 * the codec case logs: an assertion says only that the property held,
			 * whereas the number makes the run's own accounting readable in CI
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
	}
	FST_SUITE_END()
}
FST_CORE_END()
