/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2025, Anthony Minessale II <anthm@freeswitch.org>
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
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 * test_mod_xml_curl.c -- mod_xml_curl tests
 *
 */
#include <switch.h>
#include <test/switch_test.h>
#include "../mod_xml_curl.c"

/*
 * This suite compiles the module translation unit into itself. That is the only way to reach the
 * subjects under test, because all of them - the response-format dispatch helpers, do_config()
 * and xml_url_fetch() - are file static and must stay that way: nothing is de-staticised and no
 * symbol is re-exported merely to make it testable.
 *
 * What is proven here:
 *
 *   1. Nine paired fixtures establish that a BadgerFish JSON provisioning document and its XML
 *      twin serialise to byte-identical strings, so the opt-in JSON path yields the same
 *      switch_xml_t the pre-existing XML path yields.
 *   2. Every JSON failure mode - malformed payload, wrongly shaped document, non-JSON response
 *      Content-Type, unreadable body - is declined by the decoder so that the caller falls
 *      through to the untouched switch_xml_parse_file() call.
 *   3. The response-format parameter is accepted by do_config() alongside the pre-existing
 *      binding parameters, and the new binding member is NULL whenever the parameter is absent,
 *      which is the mechanism behind the strict backward-compatibility guarantee.
 *
 * Every case is hermetic: no network peer, no listening socket, no curl invocation and no
 * dependence on wall-clock time. The negative paths are reachable precisely because the file
 * static helpers can be driven directly on fixture bytes.
 */

/*
 * Fixture location, assembled at compile time.
 *
 * The tree's own runner chdir()s into this test/ directory before executing a collected binary,
 * whereas a plain "make check" runs from the module build directory, so a bare relative path
 * would resolve under one harness and silently miss under the other. Both -DSWITCH_TEST_BASE_DIR_*
 * defines come from the test target in the module's Makefile.am, and switch_test.h defines
 * SWITCH_TEST_BASE_DIR_OVERRIDE itself when the build does not supply one, so this concatenation
 * is unconditional and needs no run-time formatting.
 */
#define XC_FIXTURE_DIR SWITCH_TEST_BASE_DIR_OVERRIDE SWITCH_PATH_SEPARATOR "fixtures" SWITCH_PATH_SEPARATOR

/* The Content-Type a provisioning gateway is expected to answer with once a binding opts in. */
#define XC_JSON_CONTENT_TYPE "application/json"

/*
 * Stand-in gateway URL. It only ever reaches a log message: no case in this suite lets
 * xml_url_fetch() run, so nothing ever resolves or connects to it. The .invalid TLD is reserved
 * by RFC 2606 precisely so that it can never be registered.
 */
#define XC_TEST_URL "http://xml-curl.test.invalid/provision"

/*
 * An xml_curl.conf document injected through the XML binding mechanism by the response-format
 * parsing case. The first binding deliberately carries no response-format parameter and the
 * second one does, so a single do_config() pass exercises both the legacy shape and the new
 * parameter arm, and proves the addition leaves the pre-existing parameters untouched.
 *
 * The second binding also carries enable-post-var, which is the one pre-existing parameter with
 * state outside the module pool: it makes do_config() build a hash and a heap-allocated node on
 * the globals list, releasable only by the module's own shutdown function. Including it here is
 * deliberate - it proves the new parameter coexists with the most stateful existing arm, and it
 * makes the case's teardown correct by necessity rather than by accident.
 */
#define XC_TEST_CONFIG_DOCUMENT \
	"<document type=\"freeswitch/xml\">" \
	"<section name=\"configuration\">" \
	"<configuration name=\"xml_curl.conf\" description=\"cURL XML Gateway\">" \
	"<bindings>" \
	"<binding name=\"legacy_xml_binding\">" \
	"<param name=\"gateway-url\" value=\"" XC_TEST_URL "\" bindings=\"configuration\"/>" \
	"<param name=\"method\" value=\"post\"/>" \
	"<param name=\"timeout\" value=\"10\"/>" \
	"<param name=\"response-max-bytes\" value=\"4096\"/>" \
	"</binding>" \
	"<binding name=\"json_binding\">" \
	"<param name=\"gateway-url\" value=\"" XC_TEST_URL "\" bindings=\"configuration\"/>" \
	"<param name=\"method\" value=\"post\"/>" \
	"<param name=\"enable-post-var\" value=\"domain\"/>" \
	"<param name=\"response-format\" value=\"json\"/>" \
	"</binding>" \
	"</bindings>" \
	"</configuration>" \
	"</section>" \
	"</document>"

/*
 * Temporary xml_curl.conf provider.
 *
 * switch_xml_locate() consults registered bindings before the static configuration root, so
 * registering this function lets one test binary hand do_config() a configuration document that
 * the suite's own conf/freeswitch.xml deliberately does not contain. Any lookup other than
 * xml_curl.conf is declined by returning NULL, which sends switch_xml_locate() on to the next
 * binding and then to the static root, leaving every unrelated configuration lookup exactly as
 * it would have been.
 *
 * This is the same public binding API mod_xml_curl itself registers with, rather than the
 * root-replacement hook, which has no callers anywhere in the tree.
 */
static switch_xml_t xc_test_config_provider(const char *section, const char *tag_name, const char *key_name, const char *key_value,
											switch_event_t *params, void *user_data)
{
	const char *document = (const char *) user_data;

	if (zstr(key_value) || strcmp(key_value, "xml_curl.conf") || zstr(document)) {
		return NULL;
	}

	/*
	 * dup = SWITCH_TRUE: switch_xml_parse_str_dynamic() duplicates the template and flags the
	 * root as dynamic, so the switch_xml_free() that do_config() performs on the document it was
	 * handed releases that duplicate. The template itself is a string literal and is never freed.
	 */
	return switch_xml_parse_str_dynamic((char *) document, SWITCH_TRUE);
}

/*
 * One fixture-pair parity check, expanded once per stem so that a failure names the offending
 * stem in the FCTX report and carries its own line number.
 *
 * All locals are declared at the top of the block, and fst_parse_json_file() is invoked last
 * among the declarations because it declares a cJSON variable of its own. That ordering is what
 * keeps this source compilable both under the tree's -Wdeclaration-after-statement build and
 * under MSVC, whose project file is deliberately not extended with this test source.
 *
 * Step 1 is a fixture sanity gate: it proves the .json fixture really is well-formed JSON, so a
 * corrupted fixture is reported as a fixture problem instead of masquerading as a translator
 * bug. It is the exact mirror of the switch_xml_error() gate applied to the .xml side.
 *
 * Only non-fatal checks are used once memory is owned, so control always reaches the releases at
 * the bottom - the CI build always enables the address sanitizer, which makes a leak a build
 * failure rather than merely a test failure.
 */
#define XC_CHECK_FIXTURE_PARITY(stem) \
	{ \
		switch_xml_t json_side = NULL; \
		switch_xml_t xml_side = NULL; \
		char *json_serialised = NULL; \
		char *xml_serialised = NULL; \
		const char *parse_error = NULL; \
		fst_parse_json_file(json_probe, XC_FIXTURE_DIR stem ".json"); \
		cJSON_Delete(json_probe); \
		json_side = xml_curl_json_decode_response(XC_FIXTURE_DIR stem ".json", XC_JSON_CONTENT_TYPE, XML_CURL_MAX_BYTES, XC_TEST_URL); \
		xml_side = switch_xml_parse_file_simple(XC_FIXTURE_DIR stem ".xml"); \
		fst_xcheck(json_side != NULL, "BadgerFish decode of " stem ".json must produce a tree"); \
		fst_xcheck(xml_side != NULL, "pure parse of " stem ".xml must produce a tree"); \
		parse_error = xml_side ? switch_xml_error(xml_side) : NULL; \
		fst_xcheck(zstr(parse_error), "switch_xml_error() must be empty for " stem ".xml"); \
		if (json_side && xml_side) { \
			json_serialised = switch_xml_toxml(json_side, SWITCH_FALSE); \
			xml_serialised = switch_xml_toxml(xml_side, SWITCH_FALSE); \
		} \
		if (json_serialised && xml_serialised) { \
			fst_check_string_equals(json_serialised, xml_serialised); \
		} else { \
			fst_fail("serialisation of the " stem " pair produced nothing to compare"); \
		} \
		switch_safe_free(json_serialised); \
		switch_safe_free(xml_serialised); \
		if (json_side) { \
			switch_xml_free(json_side); \
		} \
		if (xml_side) { \
			switch_xml_free(xml_side); \
		} \
	}

/*
 * Assert that the translator declines a JSON document, and release anything it handed back on
 * the off chance that it did not. Used by the negative and robustness cases.
 */
#define XC_CHECK_TRANSLATOR_DECLINES(json_text, why) \
	{ \
		switch_xml_t declined = xml_curl_json_to_xml(json_text); \
		fst_xcheck(declined == NULL, why); \
		if (declined) { \
			switch_xml_free(declined); \
		} \
	}

FST_CORE_BEGIN("conf")
{
	FST_SUITE_BEGIN(mod_xml_curl)
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
		 * Group 1 - fixture-pair parity, three stems per provisioning section.
		 *
		 * Each case decodes the .json fixture through the very helper the response-format
		 * dispatch point calls, parses the .xml twin through the pure switch_xml parser, and
		 * requires both to serialise identically through one and the same
		 * switch_xml_toxml(x, SWITCH_FALSE) call. Equivalence is therefore demonstrated by
		 * construction rather than asserted structurally.
		 */

		/* configuration: deep nesting plus a long run of repeated same-name children. */
		FST_TEST_BEGIN(json_parity_configuration_modules)
		{
			XC_CHECK_FIXTURE_PARITY("configuration_modules")
		}
		FST_TEST_END()

		/* configuration: nested list-of-node structure with several attributes per element. */
		FST_TEST_BEGIN(json_parity_configuration_acl)
		{
			XC_CHECK_FIXTURE_PARITY("configuration_acl")
		}
		FST_TEST_END()

		/* configuration: flat parameter list - the simplest attribute-only shape. */
		FST_TEST_BEGIN(json_parity_configuration_event_socket)
		{
			XC_CHECK_FIXTURE_PARITY("configuration_event_socket")
		}
		FST_TEST_END()

		/* directory: one user with params and variables subtrees. */
		FST_TEST_BEGIN(json_parity_directory_user_simple)
		{
			XC_CHECK_FIXTURE_PARITY("directory_user_simple")
		}
		FST_TEST_END()

		/*
		 * directory: attribute-order sensitivity plus a non-ASCII value, which exercises the
		 * serialiser's numeric-character-reference path on both sides of the comparison.
		 */
		FST_TEST_BEGIN(json_parity_directory_user_params)
		{
			XC_CHECK_FIXTURE_PARITY("directory_user_params")
		}
		FST_TEST_END()

		/* directory: repeated <user> children under one parent - the canonical array case. */
		FST_TEST_BEGIN(json_parity_directory_domain_multi_user)
		{
			XC_CHECK_FIXTURE_PARITY("directory_domain_multi_user")
		}
		FST_TEST_END()

		/* dialplan: one extension, one condition, one action - nested objects, no arrays. */
		FST_TEST_BEGIN(json_parity_dialplan_single_extension)
		{
			XC_CHECK_FIXTURE_PARITY("dialplan_single_extension")
		}
		FST_TEST_END()

		/* dialplan: repeated <condition> children each holding their own <action> children. */
		FST_TEST_BEGIN(json_parity_dialplan_multi_condition)
		{
			XC_CHECK_FIXTURE_PARITY("dialplan_multi_condition")
		}
		FST_TEST_END()

		/* dialplan: repeated <extension> children - array-of-arrays nesting. */
		FST_TEST_BEGIN(json_parity_dialplan_multi_extension)
		{
			XC_CHECK_FIXTURE_PARITY("dialplan_multi_extension")
		}
		FST_TEST_END()

		/*
		 * Group 2 - malformed JSON.
		 *
		 * fst_parse_json_file() is deliberately NOT used in this group: its trailing
		 * fst_requires() is fatal on a parse failure and would abort the case instead of letting
		 * it observe the graceful decline. In-source literals also keep the malformed bytes
		 * visible right where they are asserted on, and spare the tree a fixture whose only
		 * purpose is to be broken.
		 */
		FST_TEST_BEGIN(json_malformed_payload_is_declined)
		{
			const char *truncated_object = "{\"configuration\": {\"modules\": {\"load\": [{\"@module\": \"mod_console\"}";
			const char *unterminated_string = "{\"configuration\": {\"@name\": \"unterminated}";
			const char *xml_instead_of_json = "<document type=\"freeswitch/xml\"><section name=\"configuration\"></section></document>";
			cJSON *probe = NULL;

			/*
			 * cJSON reports a parse failure by returning NULL rather than by aborting, which is
			 * what makes a non-fatal check the right instrument here. cJSON_Delete() tolerates
			 * NULL, so the release is unconditional and stays correct even if a future parser
			 * were to start accepting one of these payloads.
			 */
			probe = cJSON_Parse(truncated_object);
			fst_xcheck(probe == NULL, "cJSON_Parse must decline a truncated object");
			cJSON_Delete(probe);

			probe = cJSON_Parse(unterminated_string);
			fst_xcheck(probe == NULL, "cJSON_Parse must decline an unterminated string");
			cJSON_Delete(probe);

			probe = cJSON_Parse(xml_instead_of_json);
			fst_xcheck(probe == NULL, "cJSON_Parse must decline an XML payload");
			cJSON_Delete(probe);

			/*
			 * The translator declines the same bytes, returning NULL with nothing left to free,
			 * so the single dispatch point in xml_url_fetch() falls through to the untouched
			 * switch_xml_parse_file() call.
			 */
			XC_CHECK_TRANSLATOR_DECLINES(truncated_object, "a truncated object must not translate")
			XC_CHECK_TRANSLATOR_DECLINES(unterminated_string, "an unterminated string must not translate")
			XC_CHECK_TRANSLATOR_DECLINES(xml_instead_of_json, "an XML payload must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("", "an empty payload must not translate")
			XC_CHECK_TRANSLATOR_DECLINES(NULL, "a NULL payload must not translate")
		}
		FST_TEST_END()

		/*
		 * Parseable JSON that is not a BadgerFish provisioning document. Each shape must be
		 * refused outright rather than translated into a partial tree, so that the caller sees
		 * exactly one failure signal and falls back exactly once.
		 */
		FST_TEST_BEGIN(json_wrongly_shaped_document_is_declined)
		{
			XC_CHECK_TRANSLATOR_DECLINES("\"configuration\"", "a top-level string must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("42", "a top-level number must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": \"text\"}", "a scalar section value must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {}, \"directory\": {}}", "two top-level sections must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": [\"not-an-object\"]}}", "a non-object array element must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"settings\": {\"param\": [{\"@name\": \"a\"}, 7]}}}",
										 "a mixed array of objects and scalars must not translate")
		}
		FST_TEST_END()

		/*
		 * Group 3 - response Content-Type classification.
		 *
		 * The classifier is the first gate the decoder applies, and the one that stops a gateway
		 * which ignored the Accept header from being misread as a JSON peer. Type and subtype are
		 * compared as a whole token, case-insensitively, up to the first parameter separator.
		 */
		FST_TEST_BEGIN(json_content_type_classifier_boundaries)
		{
			/* Accepted: the JSON media type bare, with parameters, in any case, and with
			   leading linear whitespace ahead of it. */
			fst_xcheck(xml_curl_json_is_json_content_type("application/json"), "bare application/json must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("application/json; charset=utf-8"),
					   "application/json with a spaced charset parameter must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("application/json;charset=utf-8"),
					   "application/json with an unspaced parameter must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("Application/JSON"), "mixed-case Application/JSON must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("APPLICATION/JSON;charset=UTF-8"), "upper-case APPLICATION/JSON must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("  application/json"), "leading whitespace must be tolerated");

			/* Rejected: an absent or empty header, both XML media types, an unrelated type, and
			   any subtype that merely starts with or contains the JSON token. */
			fst_xcheck(!xml_curl_json_is_json_content_type(NULL), "an absent Content-Type must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type(""), "an empty Content-Type must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type("text/xml"), "text/xml must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type("application/xml"), "application/xml must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type("text/plain"), "text/plain must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type("application/jsonx"), "application/jsonx must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type("application/json-patch+json"), "application/json-patch+json must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type(";"), "a bare parameter separator must be rejected");
			fst_xcheck(!xml_curl_json_is_json_content_type(" "), "a whitespace-only Content-Type must be rejected");
		}
		FST_TEST_END()

		/*
		 * The mismatch fallback itself, driven through the helper the dispatch point actually
		 * calls. Every decline emits one warning and returns NULL, which is precisely what makes
		 * xml_url_fetch() re-enter the unmodified XML parse instead of failing the lookup.
		 */
		FST_TEST_BEGIN(json_content_type_mismatch_falls_back)
		{
			switch_xml_t decoded = NULL;

			/* A well-formed JSON body announced as XML is still declined: the decoder is gated
			   on what the gateway said it sent, not on what the bytes happen to be. */
			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.json", "text/xml", XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "a JSON body announced as text/xml must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.json", "application/xml", XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "a JSON body announced as application/xml must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.json", NULL, XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "a JSON body sent without any Content-Type must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			/* An XML body announced as JSON is declined by the translator rather than by the
			   classifier, which is the second half of the same fallback contract. */
			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.xml", XC_JSON_CONTENT_TYPE, XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "an XML body announced as application/json must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			/* A response body that never arrived. */
			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "no_such_fixture.json", XC_JSON_CONTENT_TYPE, XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "an absent response body must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			decoded = xml_curl_json_decode_response(NULL, XC_JSON_CONTENT_TYPE, XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "a NULL response body path must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			/* The binding's response ceiling is re-checked by the decoder rather than
			   re-implemented, so the pre-existing size cap governs the JSON path too. */
			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.json", XC_JSON_CONTENT_TYPE, 8, XC_TEST_URL);
			fst_xcheck(decoded == NULL, "a body larger than the binding ceiling must be declined");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}

			/* Positive control: the same body, the same ceiling and the right Content-Type is
			   accepted, which proves every decline above was caused by the condition under test
			   and not by something incidental to the fixture. */
			decoded = xml_curl_json_decode_response(XC_FIXTURE_DIR "configuration_acl.json", XC_JSON_CONTENT_TYPE, XML_CURL_MAX_BYTES, XC_TEST_URL);
			fst_xcheck(decoded != NULL, "the control decode of a JSON body announced as JSON must succeed");
			if (decoded) {
				switch_xml_free(decoded);
				decoded = NULL;
			}
		}
		FST_TEST_END()

		/*
		 * Group 4 - the response-format binding parameter.
		 *
		 * The deterministic failure branch is declared first so that it runs against a pristine
		 * binding list. conf/freeswitch.xml deliberately carries no xml_curl.conf section and no
		 * binding is registered at this point, so the binding list and the static root both miss,
		 * switch_xml_open_cfg() returns NULL and do_config() takes its documented error path.
		 */
		FST_TEST_BEGIN(do_config_without_configuration_returns_term)
		{
			switch_status_t status = SWITCH_STATUS_SUCCESS;

			/* globals.pool is deliberately left NULL: this path returns before any allocation
			   is attempted, which is itself part of what is being asserted. */
			status = do_config();
			fst_check_int_equals((int) status, (int) SWITCH_STATUS_TERM);
		}
		FST_TEST_END()

		/*
		 * The success branch. The injected document carries two bindings - the first with only
		 * pre-existing parameters and the second adding response-format - so a single do_config()
		 * pass covers both the legacy shape and the new arm, and shows the addition leaves the
		 * pre-existing parameters and their evaluation order untouched.
		 */
		FST_TEST_BEGIN(do_config_accepts_response_format_parameter)
		{
			switch_status_t bind_status = SWITCH_STATUS_FALSE;
			switch_status_t config_status = SWITCH_STATUS_FALSE;
			switch_status_t unbind_fetch_status = SWITCH_STATUS_FALSE;
			switch_status_t shutdown_status = SWITCH_STATUS_FALSE;
			switch_status_t unbind_provider_status = SWITCH_STATUS_FALSE;

			/* do_config() allocates each binding from globals.pool and duplicates every string
			   member into it, exactly as the module's load function arranges. The per-test pool
			   stands in for the module pool, which is why every binding built from it has to be
			   unregistered again before this case ends. */
			globals.pool = fst_pool;

			bind_status = switch_xml_bind_search_function_ret(xc_test_config_provider, switch_xml_parse_section_string("configuration"),
															 (void *) XC_TEST_CONFIG_DOCUMENT, NULL);

			config_status = do_config();

			/* Tear down before asserting. FCTX aborts a case by breaking out of it, so an
			   assertion placed above these calls could leave a live binding - holding a pointer
			   into a pool that is about to be destroyed - visible to every later case.
			 *
			 * The fetch function is unbound explicitly first so that its status can be asserted
			 * on below. The module's own shutdown function then runs, because it is the only
			 * thing that releases what do_config() allocated OUTSIDE the module pool: the
			 * enable-post-var hash and its heap-allocated list node. Its own repeat unbind of
			 * the fetch function is a harmless no-op. Everything else do_config() allocated came
			 * from the pool and is released with it. */
			unbind_fetch_status = switch_xml_unbind_search_function_ptr(xml_url_fetch);
			shutdown_status = mod_xml_curl_shutdown();
			unbind_provider_status = switch_xml_unbind_search_function_ptr(xc_test_config_provider);
			globals.pool = NULL;

			fst_xcheck(bind_status == SWITCH_STATUS_SUCCESS, "the temporary xml_curl.conf provider must register");
			fst_xcheck(config_status == SWITCH_STATUS_SUCCESS, "do_config() must accept a binding carrying response-format");

			/* do_config() only reports success after registering at least one binding, so a
			   successful unbind proves a binding really was built from the injected document. */
			fst_xcheck(unbind_fetch_status == SWITCH_STATUS_SUCCESS, "do_config() must have registered the fetch function");
			fst_xcheck(shutdown_status == SWITCH_STATUS_SUCCESS, "the module shutdown function must drain what do_config() allocated off-pool");
			fst_xcheck(unbind_provider_status == SWITCH_STATUS_SUCCESS, "the temporary provider must unregister");

			/* Shutdown empties the off-pool list it owns, which is what makes this case leak
			   free under the always-on address sanitizer. */
			fst_xcheck(globals.hash_root == NULL, "the module shutdown function must empty the post-var hash list");
		}
		FST_TEST_END()

		/*
		 * The backward-compatibility guarantee, asserted rather than merely intended: with no
		 * response-format parameter present nothing ever writes the member, so it stays at the
		 * NULL that do_config()'s wholesale memset() of every freshly allocated binding wrote.
		 */
		FST_TEST_BEGIN(binding_response_format_defaults_to_null)
		{
			xml_binding_t *binding = NULL;
			int poison_is_visible = 0;

			binding = (xml_binding_t *) switch_core_alloc(fst_pool, sizeof(*binding));
			fst_requires(binding);

			/* Poison every byte first, and observe the poison through the raw bytes of the
			   member rather than by loading the pointer, so that the NULL assertion below cannot
			   pass by accident on an allocator that happens to hand back zeroed memory. */
			memset(binding, 0xFF, sizeof(*binding));
			poison_is_visible = (((const unsigned char *) &binding->response_format)[0] == 0xFF);
			fst_xcheck(poison_is_visible, "the poison must be observable, or the next check proves nothing");

			/* This is the exact statement do_config() executes on every binding it allocates. */
			memset(binding, 0, sizeof(*binding));
			fst_xcheck(binding->response_format == NULL, "an absent response-format parameter must leave the member NULL");

			/* The member was appended after what used to be the last one, so no pre-existing
			   member offset moved - which is what keeps the structure layout compatible. */
			fst_xcheck((char *) &binding->response_format > (char *) &binding->curl_max_bytes,
					   "response_format must sit after curl_max_bytes, the member that used to be last");

			/* And when the parameter is present, do_config() duplicates its value into the
			   module pool and the opt-in comparison then matches case-insensitively. */
			binding->response_format = switch_core_strdup(fst_pool, "json");
			fst_xcheck(binding->response_format != NULL, "the response-format value must survive duplication into the pool");
			if (binding->response_format) {
				fst_check_string_equals(binding->response_format, "json");
				fst_xcheck(!strcasecmp(binding->response_format, "JSON"), "the opt-in value must compare case-insensitively");
			}
		}
		FST_TEST_END()

		/*
		 * Group 5 - translator robustness.
		 */
		FST_TEST_BEGIN(json_translator_rejects_unrepresentable_documents)
		{
			/* An empty document names no section, so there is nothing to translate. */
			XC_CHECK_TRANSLATOR_DECLINES("{}", "an empty document must not translate")

			/* A top-level array is not a BadgerFish document. */
			XC_CHECK_TRANSLATOR_DECLINES("[{\"configuration\": {}}]", "a top-level array must not translate")

			/* Non-string scalars are a translation error rather than an implicit coercion, at
			   every position the convention allows a value: attribute, text marker and child.
			   There is no guaranteed lexical round trip from 1 to "1" or from true to "true",
			   and the switch_xml builders accept only const char *. */
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": {\"@name\": 1}}}", "a numeric attribute value must not be coerced")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": {\"@name\": true}}}", "a boolean attribute value must not be coerced")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": {\"$\": 1.0}}}", "numeric text content must not be coerced")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": {\"$\": false}}}", "boolean text content must not be coerced")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"count\": 3}}", "a numeric child value must not be coerced")

			/* An explicit JSON null is refused too, rather than being read as an empty element
			   or as an empty string. */
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": null}}", "an explicit null child must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"param\": {\"@name\": null}}}", "an explicit null attribute value must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": null}", "an explicit null section must not translate")

			/* An empty key can name neither an element nor the text marker, and a bare "@" is not
			   an attribute name. Both have to be refused before they reach the duplicating
			   builders, which strdup() their name argument with no NULL or emptiness guard. */
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"\": {}}}", "an empty element name must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"configuration\": {\"@\": \"value\"}}", "a bare @ attribute name must not translate")
			XC_CHECK_TRANSLATOR_DECLINES("{\"\": {}}", "an empty section name must not translate")
		}
		FST_TEST_END()

		/*
		 * The whole parity design rests on one normalisation: an element with neither children
		 * nor text serialises as <name></name> and never as <name/>, on both the translated and
		 * the parsed side. The tree carries no precedent asserting that, and most fixture
		 * elements are exactly this shape, so it is asserted here directly - a divergence would
		 * fail all nine parity cases for a reason that has nothing to do with the translation.
		 */
		FST_TEST_BEGIN(empty_elements_serialise_identically_on_both_sides)
		{
			const char *json_text = "{\"configuration\": {\"settings\": {\"param\": {\"@name\": \"x\", \"@value\": \"y\"}}}}";
			const char *xml_text = "<document type=\"freeswitch/xml\"><section name=\"configuration\">"
				"<settings><param name=\"x\" value=\"y\"/></settings></section></document>";
			switch_xml_t json_side = NULL;
			switch_xml_t xml_side = NULL;
			char *json_serialised = NULL;
			char *xml_serialised = NULL;

			json_side = xml_curl_json_to_xml(json_text);
			xml_side = switch_xml_parse_str_dynamic((char *) xml_text, SWITCH_TRUE);

			fst_xcheck(json_side != NULL, "the translator must accept a minimal empty-element document");
			fst_xcheck(xml_side != NULL, "the parser must accept the equivalent XML document");

			if (json_side && xml_side) {
				json_serialised = switch_xml_toxml(json_side, SWITCH_FALSE);
				xml_serialised = switch_xml_toxml(xml_side, SWITCH_FALSE);
			}

			if (json_serialised && xml_serialised) {
				/* Neither side emits a self-closing tag, ... */
				fst_check_string_has(json_serialised, "<param name=\"x\" value=\"y\"></param>");
				fst_check_string_has(xml_serialised, "<param name=\"x\" value=\"y\"></param>");
				fst_check_string_does_not_have(json_serialised, "/>");
				fst_check_string_does_not_have(xml_serialised, "/>");
				/* ... and the two are byte identical, exactly as the nine fixture pairs are. */
				fst_check_string_equals(json_serialised, xml_serialised);
			} else {
				fst_fail("the empty-element self-check produced nothing to compare");
			}

			switch_safe_free(json_serialised);
			switch_safe_free(xml_serialised);
			if (json_side) {
				switch_xml_free(json_side);
			}
			if (xml_side) {
				switch_xml_free(xml_side);
			}
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
