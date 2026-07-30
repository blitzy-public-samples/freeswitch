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
 * This is a white-box-free harness: it links against the module's own
 * translation unit through the convenience library declared by the parent
 * Makefile.am and reaches its subjects exclusively through legitimate C++
 * access.  Nothing is de-staticised, no symbol is re-exported, no `friend'
 * declaration is added and not one line of mod_opal.cpp / mod_opal.h is
 * modified.
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
 * Every injected listener binds loopback on a fixed high unprivileged port.
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

/* The configuration file name FSManager::ReadConfig() asks for. */
#define TEST_OPAL_CONFIG_FILE "opal.conf"

/*
 * Listener coordinates for the injected configuration.  Loopback plus a fixed
 * high unprivileged port keeps FSManager::Initialise()'s StartListener() call
 * harmless: the shipped sample's $${local_ip_v4} would resolve to a real
 * interface and its port 1720 is privileged, and omitting the listener stanza
 * altogether is worse still because ReadConfig() supplies no port default and
 * Initialise() then wildcard-binds the default H.323 signalling port.
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
	switch_xml_t xml = NULL;

	(void) params;
	(void) user_data;

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
	xml = switch_xml_parse_str_dynamic((char *) TEST_OPAL_CONFIG_XML, SWITCH_TRUE);

	return xml;
}

/*
 * Register the injected configuration provider.  Never called from inside the
 * provider itself: switch_xml_locate() holds a read lock across the callback
 * that the bind and unbind paths take for writing.
 */
static switch_status_t test_opal_bind_config(void)
{
	return switch_xml_bind_search_function_ret(test_opal_xml_config_provider,
											   SWITCH_XML_SECTION_CONFIG,
											   NULL,
											   NULL);
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
 * ---------------------------------------------------------------------------
 * The suite
 * ---------------------------------------------------------------------------
 *
 * A real core is required: switch_xml_open_cfg() asserts that the main XML
 * root exists, which only the full bootstrap provides.  The configuration
 * directory is resolved from the compile-time test base directory joined with
 * the name passed here, so "conf_opal" selects this module's own fixture root.
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
		 * Case 5 - module load and endpoint-interface registration.
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

			status = mod_opal_load(&loaded_interface, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(loaded_interface != NULL);

			if (loaded_interface) {
				fst_check(loaded_interface->endpoint_interface != NULL);

				if (loaded_interface->endpoint_interface) {
					fst_check_string_equals(loaded_interface->endpoint_interface->interface_name,
											TEST_OPAL_INTERFACE_NAME);
				}
			}

			fst_check(test_opal_unbind_config() == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/*
		 * Case 6 - module shutdown.
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

			status = mod_opal_shutdown();
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Nothing PTLib-derived may outlive the suite. */
			test_opal_release_process();
			fst_check(test_opal_process == NULL);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
