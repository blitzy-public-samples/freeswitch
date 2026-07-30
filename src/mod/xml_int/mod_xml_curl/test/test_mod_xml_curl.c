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
 * test_mod_xml_curl.c -- mod_xml_curl tests
 *
 */

/*
 * ---------------------------------------------------------------------------
 * HARNESS ARCHITECTURE
 * ---------------------------------------------------------------------------
 *
 * This suite covers the opt-in BadgerFish JSON response decoder that
 * mod_xml_curl gained alongside its original XML-only decode step, plus the
 * hardening that decoder is required to carry.  It observes the production
 * module; it does not widen it.  Nothing is de-staticised, no new header is
 * introduced, and the module's symbol surface is unchanged.
 *
 * WHY THE PRODUCTION TRANSLATION UNIT IS INCLUDED
 * -----------------------------------------------
 * Every subject under test is a file-static function: xml_url_fetch(),
 * do_config(), and the whole xml_curl_json_* family.  A convenience library
 * cannot export a file-static symbol, and promoting the translator to external
 * linkage purely to make it reachable would pollute the module's symbol
 * surface for no operational benefit.  The module source is therefore compiled
 * into this translation unit, which is the white-box technique already
 * sanctioned in-tree by src/mod/applications/mod_http_cache/test/test_aws.c
 * (which includes "../aws.c" the same way) and by tests/unit/switch_core.c.
 * The declared build contract for this target follows from that choice: one
 * translation unit, no second source file, no convenience library and no
 * _LDADD.
 *
 * One further observation point is needed on top of that, because the core
 * offers no way to enumerate the bindings do_config() registers: the public
 * switch_xml_bind_search_function() macro is interposed for the remainder of
 * this translation unit so the bindings the module builds can be read back and
 * asserted.  The interposition and why it leaves the module's behaviour
 * untouched are documented at the point it is declared, below.
 *
 * WHY FST_CORE_BEGIN AND NOT FST_MODULE_BEGIN
 * -------------------------------------------
 * A real core is required.  do_config() reaches the core XML layer through
 * switch_xml_open_cfg(), which asserts on a NULL document root, and it
 * allocates every binding and every duplicated string from a live memory pool.
 * FST_MODULE_BEGIN is rejected for two separate reasons: it dlopens the built
 * shared object out of the module's .libs/ directory, which would load a
 * second copy of code that is already linked into this binary, and its
 * matching FST_MODULE_END() then performs an implicit, unasserted unload.
 * FST_SUITE_BEGIN loads nothing, which keeps this suite in charge of what it
 * exercises.  fst_requires_module() is likewise not used anywhere here: it is
 * a fatal precondition rather than a skip, so it would convert an absent
 * artifact into a red build instead of a quiet one.
 *
 * FIXTURES
 * --------
 * The nine paired fixtures under test/fixtures/ are the parity contract: a
 * BadgerFish JSON document and its XML twin must serialise to byte-identical
 * output through one and the same switch_xml_toxml() call.  Three pairs cover
 * each of the configuration, directory and dialplan provisioning sections.
 *
 * The XML side of every pair is read with switch_xml_parse_file_simple().
 * switch_xml_parse_file() is deliberately never used on a fixture: it takes
 * the global FILE_LOCK, writes <basename>.fsxml into the log directory, and
 * runs the configuration preprocessor, which deletes every whole line holding
 * <include>, </include> or <? -- which would erase a minified, single-line
 * fixture entirely.
 *
 * Adversarial inputs are in-source string literals rather than extra fixture
 * files, so the shipped fixture set stays exactly what it documents: the
 * canonical, well-formed contract.
 *
 * DETERMINISM
 * -----------
 * No case performs network I/O, opens a listening port, or depends on the
 * wall clock.  The two cases that need a response body on disk write it into
 * the core temp directory and remove it again.  The one case that needs a
 * configuration document injects it through switch_xml_bind_search_function_ret()
 * -- the same public binding API mod_xml_curl itself registers with -- and
 * removes it again with switch_xml_unbind_search_function_ptr() before the
 * case ends.  FCTX runs every case in one process in declaration order, so a
 * binding left behind would be visible to every later case.
 */

#include <switch.h>
#include <test/switch_test.h>

/*
 * -------------------------------------------------------------------------
 * OBSERVING THE BINDINGS do_config() BUILDS
 * -------------------------------------------------------------------------
 * do_config() allocates one xml_binding_t per configured binding, populates it,
 * hands it to the core as the binding's private data and then drops its own
 * reference.  The core exposes no way to enumerate registered bindings, so the
 * only way to assert what do_config() actually stored -- rather than merely that
 * it returned success -- is to record the pointer as it goes past.
 *
 * switch_xml_bind_search_function() is a macro in the public header that forwards
 * to switch_xml_bind_search_function_ret() with no out-parameter, so interposing
 * on it for the remainder of this translation unit records each binding while
 * making exactly the call the module makes today.  Reading the core's
 * implementation confirms the forwarded call is behaviour-identical: the optional
 * out-parameter is only assigned, inside a lock the function already holds.
 *
 * Nothing in the module is de-staticised, no production line is edited, and the
 * case that uses this reads the recorded bindings only after do_config() has
 * already freed the configuration tree it parsed them from -- which is what makes
 * the string members prove they were duplicated into the pool rather than
 * borrowed from that tree.
 */
#define FST_XC_MAX_OBSERVED_BINDINGS 4

static void *fst_xc_observed_bindings[FST_XC_MAX_OBSERVED_BINDINGS];
static int fst_xc_observed_binding_count = 0;

static void fst_xc_observe_binding(void *user_data)
{
	if (fst_xc_observed_binding_count < FST_XC_MAX_OBSERVED_BINDINGS) {
		fst_xc_observed_bindings[fst_xc_observed_binding_count] = user_data;
	}

	fst_xc_observed_binding_count++;
}

#undef switch_xml_bind_search_function
#define switch_xml_bind_search_function(_f, _s, _u) \
	(fst_xc_observe_binding((void *) (_u)), switch_xml_bind_search_function_ret(_f, _s, _u, NULL))

#include "../mod_xml_curl.c"

/*
 * Fixture directory, composed at compile time from the base directory the build
 * defines for this target.  A bare relative path would silently resolve against
 * whatever directory the test happened to be started from.
 */
#define FST_XC_FIXTURE_DIR SWITCH_TEST_BASE_DIR_OVERRIDE SWITCH_PATH_SEPARATOR "fixtures" SWITCH_PATH_SEPARATOR

/* Response body written to disk by the case that drives the body reader. */
#define FST_XC_TEMP_BODY "test_mod_xml_curl_body.json"

/* Document written to disk by the case that drives the file: URL shortcut. */
#define FST_XC_TEMP_XML "test_mod_xml_curl_shortcut.xml"

/* Documents written to disk by the configuration-parsing case, one per binding. */
#define FST_XC_TEMP_DIRECTORY_XML "test_mod_xml_curl_directory.xml"
#define FST_XC_TEMP_DIALPLAN_XML "test_mod_xml_curl_dialplan.xml"

/*
 * Serialise one fixture pair.
 *
 * The JSON side is driven exactly the way xml_curl_json_decode_response() drives
 * it after a 200 response: the production reader produces the bytes and the
 * production translator produces the tree.  The XML side is parsed with the
 * simple parser.  Both trees are then serialised with the identical call, which
 * is what makes the comparison meaningful at all.
 *
 * On success *from_json and *from_xml belong to the caller and must be released
 * with switch_safe_free().  The XML parser's diagnostic is copied into
 * xml_error before the tree is freed, because switch_xml_error() points into
 * the tree.
 */
static switch_status_t fst_xc_render_pair(const char *stem, const char *section, char **from_json, char **from_xml,
										 char *xml_error, switch_size_t xml_error_len)
{
	char json_path[1024] = "";
	char xml_path[1024] = "";
	char *json_text = NULL;
	switch_xml_t json_tree = NULL;
	switch_xml_t xml_tree = NULL;
	switch_status_t status = SWITCH_STATUS_FALSE;

	*from_json = NULL;
	*from_xml = NULL;

	if (xml_error && xml_error_len) {
		*xml_error = '\0';
	}

	switch_snprintf(json_path, sizeof(json_path), "%s%s.json", FST_XC_FIXTURE_DIR, stem);
	switch_snprintf(xml_path, sizeof(xml_path), "%s%s.xml", FST_XC_FIXTURE_DIR, stem);

	if (!(json_text = xml_curl_json_read_file(json_path, XML_CURL_MAX_BYTES))) {
		return SWITCH_STATUS_FALSE;
	}

	json_tree = xml_curl_json_to_xml(json_text, section);
	switch_safe_free(json_text);

	if (!json_tree) {
		return SWITCH_STATUS_FALSE;
	}

	if (!(xml_tree = switch_xml_parse_file_simple(xml_path))) {
		switch_xml_free(json_tree);
		return SWITCH_STATUS_FALSE;
	}

	if (xml_error && xml_error_len) {
		switch_copy_string(xml_error, switch_str_nil(switch_xml_error(xml_tree)), xml_error_len);
	}

	*from_json = switch_xml_toxml(json_tree, SWITCH_FALSE);
	*from_xml = switch_xml_toxml(xml_tree, SWITCH_FALSE);

	if (*from_json && *from_xml) {
		status = SWITCH_STATUS_SUCCESS;
	}

	switch_xml_free(json_tree);
	switch_xml_free(xml_tree);

	return status;
}

/*
 * 1 when the translator refuses the document, 0 when it accepts it.  Any tree the
 * translator does hand back is released here, so a wrong expectation reports a
 * failure instead of leaking.
 */
static int fst_xc_rejects(const char *text, const char *section)
{
	switch_xml_t xml = xml_curl_json_to_xml(text, section);

	if (!xml) {
		return 1;
	}

	switch_xml_free(xml);

	return 0;
}

/* 1 when the translator accepts the document.  The mirror image of fst_xc_rejects(). */
static int fst_xc_accepts(const char *text, const char *section)
{
	switch_xml_t xml = xml_curl_json_to_xml(text, section);

	if (!xml) {
		return 0;
	}

	switch_xml_free(xml);

	return 1;
}

/*
 * Translate `text` and return its serialisation, or NULL when the translator
 * refused it.  The caller owns the result and must release it with
 * switch_safe_free().
 */
static char *fst_xc_render(const char *text, const char *section)
{
	switch_xml_t xml = xml_curl_json_to_xml(text, section);
	char *rendered = NULL;

	if (!xml) {
		return NULL;
	}

	rendered = switch_xml_toxml(xml, SWITCH_FALSE);
	switch_xml_free(xml);

	return rendered;
}

/*
 * Write `len` bytes to `path`, replacing anything already there.  Returns 1 on
 * success.  Both xml_curl_json_read_file() and xml_url_fetch() take a file name
 * rather than a buffer, because the production fetch streams the HTTP response
 * into a temporary file before anything decodes it.
 */
static int fst_xc_write_file(const char *path, const char *text, switch_size_t len)
{
	switch_ssize_t wrote;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

	if (fd < 0) {
		return 0;
	}

	wrote = write(fd, text, len);
	close(fd);

	return (wrote == (switch_ssize_t) len) ? 1 : 0;
}

/*
 * The xml_curl.conf template injected for the configuration-parsing case.
 *
 * Two bindings, deliberately: the first opts in to JSON, the second does not, so
 * one do_config() run covers both halves of the response-format contract.  The
 * first binding also carries a broad sample of the module's other parameters,
 * because response-format sits at the tail of a chain of nineteen and must not
 * disturb any of the other eighteen.
 *
 * The two %s conversions take the paths of the two documents below, which that
 * case writes to disk itself.  A file: gateway URL is answered by parsing that
 * file and returning before any HTTP request is attempted, so the bindings
 * do_config() builds can be driven end to end through switch_xml_locate() with
 * no network peer, no listening port, and no dependency on the content of any
 * shipped file.
 */
static const char fst_xc_conf_template[] =
	"<document type=\"freeswitch/xml\">"
	"<section name=\"configuration\">"
	"<configuration name=\"xml_curl.conf\" description=\"cURL XML Gateway\">"
	"<bindings>"
	"<binding name=\"json_binding\">"
	"<param name=\"gateway-url\" value=\"file:%s\" bindings=\"directory\"/>"
	"<param name=\"gateway-credentials\" value=\"provisioner:secret\"/>"
	"<param name=\"auth-scheme\" value=\"digest\"/>"
	"<param name=\"method\" value=\"POST\"/>"
	"<param name=\"timeout\" value=\"7\"/>"
	"<param name=\"disable-100-continue\" value=\"false\"/>"
	"<param name=\"enable-cacert-check\" value=\"true\"/>"
	"<param name=\"enable-ssl-verifyhost\" value=\"true\"/>"
	"<param name=\"cookie-file\" value=\"/tmp/test_mod_xml_curl_cookies\"/>"
	"<param name=\"use-dynamic-url\" value=\"true\"/>"
	"<param name=\"enable-post-var\" value=\"variable_sip_from_user\"/>"
	"<param name=\"response-max-bytes\" value=\"65536\"/>"
	"<param name=\"response-format\" value=\"json\"/>"
	"</binding>"
	"<binding name=\"xml_binding\">"
	"<param name=\"gateway-url\" value=\"file:%s\" bindings=\"dialplan\"/>"
	"</binding>"
	"</bindings>"
	"</configuration>"
	"</section>"
	"</document>";

/*
 * The two documents those bindings resolve to.  Both are written out by the case
 * rather than read from the tree, and both are deliberately free of anything the
 * configuration preprocessor removes -- it deletes every whole line holding
 * <include>, </include> or <?, and the file: shortcut goes through it.
 *
 * The fixture configuration root has no directory section at all, so a directory
 * hit can only have come through the binding.  The dialplan root does have a
 * context named "default", so the extension name below is what distinguishes the
 * binding's answer from the static root's.
 */
static const char fst_xc_directory_document[] =
	"<document type=\"freeswitch/xml\"><section name=\"directory\"><user id=\"1000\"/></section></document>\n";

static const char fst_xc_dialplan_document[] =
	"<document type=\"freeswitch/xml\"><section name=\"dialplan\"><context name=\"default\">"
	"<extension name=\"binding_only\"/></context></section></document>\n";

/*
 * Configuration provider for the case above.  switch_xml_locate() consults every
 * registered binding before it falls back to the static document root, so this
 * is how one binary presents a configuration the fixture root deliberately does
 * not contain.  It answers exactly one lookup and declines everything else.
 */
static switch_xml_t fst_xc_conf_search(const char *section, const char *tag_name, const char *key_name, const char *key_value,
									   switch_event_t *params, void *user_data)
{
	const char *document = (const char *) user_data;

	/* the core passes the requesting event through; this provider is content free */
	(void) params;

	if (zstr(section) || strcasecmp(section, "configuration")) {
		return NULL;
	}

	if (zstr(tag_name) || strcasecmp(tag_name, "configuration")) {
		return NULL;
	}

	if (zstr(key_name) || strcasecmp(key_name, "name")) {
		return NULL;
	}

	if (zstr(key_value) || strcasecmp(key_value, "xml_curl.conf")) {
		return NULL;
	}

	if (zstr(document)) {
		return NULL;
	}

	/*
	 * switch_xml_parse_str_dynamic() takes a non-const char * because the string
	 * parser rewrites its buffer in place.  With dup == SWITCH_TRUE it duplicates
	 * first and frees the duplicate from switch_xml_free(), so passing a pointer
	 * to immutable storage is safe and nothing is leaked on the failure path.
	 */
	return switch_xml_parse_str_dynamic((char *) document, SWITCH_TRUE);
}

FST_CORE_BEGIN("conf")
{
	FST_SUITE_BEGIN(mod_xml_curl)
	{
		FST_SETUP_BEGIN()
		{
			/*
			 * Nothing to arrange: every case builds its own inputs.  The block is
			 * mandatory all the same -- FST_CORE_BEGIN selects the full core, and
			 * FST_TEST_BEGIN then requires both the per-test memory pool and the
			 * soft timer that only this macro creates.
			 */
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			/*
			 * Safety net.  fst_requires() jumps straight to teardown on failure, so a
			 * case that registers an XML binding could otherwise leave it behind for
			 * every later case in this single process.  Both calls return
			 * SWITCH_STATUS_FALSE, harmlessly, when the function is not bound.
			 */
			switch_xml_unbind_search_function_ptr(fst_xc_conf_search);
			switch_xml_unbind_search_function_ptr(xml_url_fetch);
		}
		FST_TEARDOWN_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 1 -- the nine paired fixtures
		 * -------------------------------------------------------------------
		 * Each case proves one pair equivalent by construction: the JSON document
		 * goes through the production reader and the BadgerFish translator, its XML
		 * twin goes through the simple parser, and both trees are serialised with
		 * the identical switch_xml_toxml(x, SWITCH_FALSE) call.  Byte-identical
		 * output is the acceptance test; structural similarity is not enough,
		 * because attribute order, empty-element normalisation and entity encoding
		 * are all observable in the serialisation.
		 */

		FST_TEST_BEGIN(fixture_parity_configuration_modules)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				/*
				 * Independently confirm the shipped fixture is well-formed JSON with
				 * exactly one top-level key, so that a parity failure can only be
				 * attributed to the translator and never to a corrupt fixture.
				 */
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_modules.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("configuration_modules", "configuration", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			/* the XML twin must parse without any parser diagnostic at all */
			fst_check_string_equals(xml_error, "");
			/* deep nesting plus a long run of repeated same-name children */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_configuration_acl)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_acl.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("configuration_acl", "configuration", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* nested list-of-node structure with several attributes on every element */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_configuration_event_socket)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_event_socket.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("configuration_event_socket", "configuration", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* the simplest possible shape: a flat, attribute-only parameter list */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_directory_user_simple)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_user_simple.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_user_simple", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* one user carrying both a params and a variables subtree */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_directory_user_params)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_user_params.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_user_params", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* attribute-order sensitivity and a non-ASCII value in one document */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * Prove the two properties this pair exists for, rather than trusting that
			 * an equal comparison of two identically wrong strings means anything: the
			 * declared attribute order survives translation, the non-ASCII value is
			 * emitted as a numeric character reference, and a bare ampersand is
			 * escaped rather than passed through.
			 */
			fst_check_string_has(from_json, "<param value=\"1234\" name=\"password\">");
			fst_check_string_has(from_json, "Fern&#xE1;ndez");
			fst_check_string_has(from_json, "Sales &amp; Support");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_directory_domain_multi_user)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_domain_multi_user.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_domain_multi_user", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* the canonical array case: repeated <user> children under one parent */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_dialplan_single_extension)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_single_extension.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("dialplan_single_extension", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* one extension, one condition, one action -- nested objects, no arrays */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_dialplan_multi_condition)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_multi_condition.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("dialplan_multi_condition", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* repeated <condition> children, each holding actions of its own */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_dialplan_multi_extension)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_multi_extension.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("dialplan_multi_extension", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* array-of-arrays nesting: repeated extensions, each with repeated children */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 2 -- content-type negotiation
		 * -------------------------------------------------------------------
		 */

		FST_TEST_BEGIN(content_type_classifier_boundaries)
		{
			/* accepted: the media type bare, with parameters, in any case, padded */
			fst_xcheck(xml_curl_json_is_json_content_type("application/json") == 1, "a bare application/json must be accepted");
			fst_xcheck(xml_curl_json_is_json_content_type("application/json; charset=utf-8") == 1,
					   "a charset parameter must not defeat the match");
			fst_xcheck(xml_curl_json_is_json_content_type("application/json;charset=utf-8") == 1,
					   "a parameter without a space must not defeat the match");
			fst_xcheck(xml_curl_json_is_json_content_type("Application/JSON") == 1, "the media type must be matched case-insensitively");
			fst_xcheck(xml_curl_json_is_json_content_type("  application/json  ") == 1, "surrounding whitespace must be tolerated");

			/* refused: XML in either spelling, an unrelated type, a near miss, nothing at all */
			fst_xcheck(xml_curl_json_is_json_content_type("text/xml") == 0, "text/xml must not be decoded as JSON");
			fst_xcheck(xml_curl_json_is_json_content_type("application/xml") == 0, "application/xml must not be decoded as JSON");
			fst_xcheck(xml_curl_json_is_json_content_type("text/plain") == 0, "text/plain must not be decoded as JSON");
			fst_xcheck(xml_curl_json_is_json_content_type("text/json") == 0, "the type half must match too");
			fst_xcheck(xml_curl_json_is_json_content_type("application/jsonx") == 0, "the subtype is compared as a whole token");
			fst_xcheck(xml_curl_json_is_json_content_type("application/json-seq") == 0, "a different subtype must not match");
			fst_xcheck(xml_curl_json_is_json_content_type("") == 0, "an empty header must not be treated as JSON");
			fst_xcheck(xml_curl_json_is_json_content_type(NULL) == 0, "an absent header must not be treated as JSON");
			fst_xcheck(xml_curl_json_is_json_content_type(";charset=utf-8") == 0, "parameters with no media type must not match");
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 3 -- the negative paths, one case per hardening property
		 * -------------------------------------------------------------------
		 */

		FST_TEST_BEGIN(malformed_json_rejected)
		{
			/*
			 * fst_parse_json_file() is deliberately NOT used here.  It ends in a fatal
			 * fst_requires() on the parsed pointer, so a malformed document would abort
			 * the case rather than be observed.  The parser is driven directly on an
			 * in-source literal and the NULL is checked without stopping the run.
			 */
			const char *trailing_comma = "{\"directory\":{\"user\":{\"@id\":\"1000\",}}}";
			const char *unquoted_key = "{directory:{\"user\":{\"@id\":\"1000\"}}}";
			cJSON *parsed = cJSON_Parse(trailing_comma);

			fst_xcheck(parsed == NULL, "the parser itself must refuse a trailing comma");
			/* symmetrical on every path; cJSON_Delete() ignores NULL */
			cJSON_Delete(parsed);

			/* every malformed body converges on the same NULL, which is the XML fallback */
			fst_xcheck(fst_xc_rejects(trailing_comma, "directory"), "a trailing comma must not decode");
			fst_xcheck(fst_xc_rejects(unquoted_key, "directory"), "an unquoted key must not decode");
			fst_xcheck(fst_xc_rejects("not json at all", "directory"), "arbitrary text must not decode");
			fst_xcheck(fst_xc_rejects("<document type=\"freeswitch/xml\"/>", "directory"), "an XML body must not decode as JSON");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\" \"1000\"}}}", "directory"), "a missing colon must not decode");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(strict_body_termination)
		{
			/*
			 * cJSON_Parse() stops at the end of the first complete value and reports
			 * success, so a body carrying anything after it would be accepted with the
			 * tail silently ignored.  The decoder parses requiring NUL termination and
			 * then re-checks where the parse ended, so the whole body has to be exactly
			 * one document.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}GARBAGE", "directory"),
					   "trailing bytes after the document must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}}", "directory"),
					   "an extra closing brace must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"}}} {\"dialplan\":{\"context\":{}}}", "directory"),
					   "a second document in the same body must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"", "directory"),
					   "a truncated document must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000}}}", "directory"),
					   "an unterminated string must be refused");

			/* trailing whitespace is not trailing content */
			fst_xcheck(fst_xc_accepts("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}\r\n\t ", "directory"),
					   "trailing whitespace alone must still be accepted");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(response_body_read_is_exact)
		{
			/*
			 * The body a fetch decodes is the temporary file file_callback() streamed the
			 * response into, so the reader is what stands between a partial or padded
			 * file and the parser.
			 */
			static const char payload[] = "{\"directory\":{\"user\":{\"@id\":\"1000\"}}}\0HIDDEN";
			char path[1024] = "";
			char *got = NULL;

			switch_snprintf(path, sizeof(path), "%s%s%s", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, FST_XC_TEMP_BODY);

			/* the recorded length covers bytes past the NUL, so a NUL-terminated read
			   would hand the decoder a document hiding arbitrary trailing content */
			fst_requires(fst_xc_write_file(path, payload, sizeof(payload) - 1));
			got = xml_curl_json_read_file(path, XML_CURL_MAX_BYTES);
			fst_xcheck(got == NULL, "a body with an embedded NUL must be refused outright");
			switch_safe_free(got);

			/* the same reader accepts the same document once the hidden tail is gone */
			fst_requires(fst_xc_write_file(path, payload, strlen(payload)));
			got = xml_curl_json_read_file(path, XML_CURL_MAX_BYTES);
			fst_xcheck(got != NULL, "a clean body of the same shape must still be accepted");
			if (got) {
				fst_check_string_equals(got, payload);
			}
			switch_safe_free(got);

			/* a body past the binding's ceiling is refused rather than truncated */
			got = xml_curl_json_read_file(path, 8);
			fst_xcheck(got == NULL, "a body larger than the configured ceiling must be refused");
			switch_safe_free(got);

			/* an empty body is not a document */
			fst_requires(fst_xc_write_file(path, "", 0));
			got = xml_curl_json_read_file(path, XML_CURL_MAX_BYTES);
			fst_xcheck(got == NULL, "a zero length body must be refused");
			switch_safe_free(got);

			unlink(path);

			/* and neither a missing file nor a missing name is a document */
			got = xml_curl_json_read_file(path, XML_CURL_MAX_BYTES);
			fst_xcheck(got == NULL, "a missing body must be refused");
			switch_safe_free(got);

			got = xml_curl_json_read_file(NULL, XML_CURL_MAX_BYTES);
			fst_xcheck(got == NULL, "a NULL file name must be refused");
			switch_safe_free(got);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(section_binding_enforced)
		{
			char *rendered = NULL;

			/*
			 * The section the core asked for is threaded all the way into the translator,
			 * so a gateway cannot answer a directory lookup with a configuration document
			 * and have the core treat the result as directory data.
			 */
			fst_xcheck(fst_xc_rejects("{\"configuration\":{\"configuration\":{\"@name\":\"acl.conf\"}}}", "directory"),
					   "a configuration document must not satisfy a directory lookup");
			fst_xcheck(fst_xc_rejects("{\"dialplan\":{\"context\":{\"@name\":\"default\"}}}", "directory"),
					   "a dialplan document must not satisfy a directory lookup");
			fst_xcheck(fst_xc_rejects("{\"Directory\":{\"user\":{\"@id\":\"1000\"}}}", "directory"),
					   "the section name is compared exactly, not case-insensitively");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{}},\"dialplan\":{\"context\":{}}}", "directory"),
					   "a body with more than one top-level key must be refused");
			fst_xcheck(fst_xc_rejects("{}", "directory"), "a body with no top-level key must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}", NULL),
					   "a translation with no requested section must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}", ""),
					   "a translation with an empty requested section must be refused");

			/* the matching section is accepted, and the synthesised envelope names it */
			rendered = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "<document type=\"freeswitch/xml\">");
			fst_check_string_has(rendered, "<section name=\"directory\">");
			fst_check_string_has(rendered, "<user id=\"1000\">");
			switch_safe_free(rendered);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(xml_name_injection_rejected)
		{
			/*
			 * A JSON member name reaches switch_xml_add_child_d() and
			 * switch_xml_set_attr_d() verbatim, and the serializer writes names without
			 * escaping.  The name alphabet is therefore a whitelist, checked first on its
			 * own and then through the whole decoder.
			 */
			fst_xcheck(xml_curl_json_is_valid_xml_name("user") == 1, "a plain name must be accepted");
			fst_xcheck(xml_curl_json_is_valid_xml_name("_private") == 1, "a leading underscore must be accepted");
			fst_xcheck(xml_curl_json_is_valid_xml_name("vm-password") == 1, "a hyphen must be accepted");
			fst_xcheck(xml_curl_json_is_valid_xml_name("a.b") == 1, "a period must be accepted");
			fst_xcheck(xml_curl_json_is_valid_xml_name("ns:user") == 1, "one namespace colon must be accepted");
			fst_xcheck(xml_curl_json_is_valid_xml_name("x></x><evil") == 0, "markup in a name must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("a\" injected=\"yes") == 0, "a quote in a name must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("1user") == 0, "a leading digit must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("us er") == 0, "a space in a name must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("a:b:c") == 0, "two colons must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name(":x") == 0, "a leading colon must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("x:") == 0, "a trailing colon must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("caf\xC3\xA9") == 0, "a non-ASCII name must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name("") == 0, "an empty name must be refused");
			fst_xcheck(xml_curl_json_is_valid_xml_name(NULL) == 0, "a NULL name must be refused");

			fst_xcheck(fst_xc_rejects("{\"directory\":{\"x></x><evil\":{\"@id\":\"1\"}}}", "directory"),
					   "an element name may not forge markup");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@a\\\" injected=\\\"yes\":\"1\"}}}", "directory"),
					   "an attribute name may not forge an attribute");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@\":\"1\"}}}", "directory"), "a bare @ is not an attribute name");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"\":{\"@id\":\"1\"}}}", "directory"), "an empty element name must be refused");
			fst_xcheck(fst_xc_rejects("{\"x></x><evil\":{\"user\":{}}}", "x></x><evil"),
					   "even a section name that matches must still be a valid XML name");
			fst_xcheck(fst_xc_accepts("{\"directory\":{\"ns:user\":{\"@x:id\":\"1\"}}}", "directory"),
					   "namespaced names must still be accepted");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(resource_limits_enforced)
		{
			switch_stream_handle_t stream = { 0 };
			int i;

			/*
			 * No number, boolean or null token can reach the parser at all, because the
			 * lexical gate refuses every non-structural byte outside a string.  That
			 * closes the parser's number handling to remote input entirely, and it is
			 * also why a scalar is never silently coerced into a string.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":1000}}}", "directory"), "a numeric value must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":1e400}}}", "directory"),
					   "an overflowing numeric literal must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":-0.5}}}", "directory"), "a signed fraction must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":true}}}", "directory"), "a boolean value must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":false}}}", "directory"), "a false value must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":null}}}", "directory"), "an explicit null must be refused");

			/* the depth ceiling is enforced on the raw bytes, so it holds identically
			   however the vendored parser happens to be configured on a given build */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH + 8; i++) {
				stream.write_function(&stream, "%s", "{\"a\":");
			}
			stream.write_function(&stream, "%s", "{}");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH + 8; i++) {
				stream.write_function(&stream, "%s", "}");
			}
			stream.write_function(&stream, "%s", "}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"), "a document deeper than the ceiling must be refused");
			switch_safe_free(stream.data);

			/* one object may not carry more members than the ceiling allows */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":{");
			for (i = 0; i < XML_CURL_JSON_MAX_OBJECT_MEMBERS + 4; i++) {
				stream.write_function(&stream, "%s\"@a%d\":\"v\"", i ? "," : "", i);
			}
			stream.write_function(&stream, "%s", "}}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"), "an object wider than the ceiling must be refused");
			switch_safe_free(stream.data);

			/* repeated children under one name are bounded, which also bounds the
			   quadratic same-name insertion the core performs while building them */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":[");
			for (i = 0; i < XML_CURL_JSON_MAX_ARRAY_ELEMENTS + 4; i++) {
				stream.write_function(&stream, "%s{\"@id\":\"x\"}", i ? "," : "");
			}
			stream.write_function(&stream, "%s", "]}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"), "an array longer than the ceiling must be refused");
			switch_safe_free(stream.data);

			/* one value may not exceed the per-string ceiling */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":{\"@id\":\"");
			for (i = 0; i < (XML_CURL_JSON_MAX_STRING_BYTES / 8) + 8; i++) {
				stream.write_function(&stream, "%s", "xxxxxxxx");
			}
			stream.write_function(&stream, "%s", "\"}}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"), "a value longer than the ceiling must be refused");
			switch_safe_free(stream.data);

			/* and one name may not exceed the per-name ceiling */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"");
			for (i = 0; i < (XML_CURL_JSON_MAX_NAME_BYTES / 4) + 4; i++) {
				stream.write_function(&stream, "%s", "aaaa");
			}
			stream.write_function(&stream, "%s", "\":{}}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "an element name longer than the ceiling must be refused");
			switch_safe_free(stream.data);

			/* a document that stays inside every ceiling is still accepted, so the
			   ceilings bound abuse rather than legitimate provisioning documents */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":");
			for (i = 0; i < 20; i++) {
				stream.write_function(&stream, "%s", "{\"a\":");
			}
			stream.write_function(&stream, "%s", "{}");
			for (i = 0; i < 20; i++) {
				stream.write_function(&stream, "%s", "}");
			}
			stream.write_function(&stream, "%s", "}");
			fst_xcheck(fst_xc_accepts((const char *) stream.data, "directory"),
					   "a document inside every ceiling must still be accepted");
			switch_safe_free(stream.data);

			/* neither an empty array nor an array of non-objects is a canonical repetition */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":[]}}", "directory"), "an empty array must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":[\"a\",\"b\"]}}", "directory"), "an array of strings must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":[[{\"@id\":\"1\"}]]}}", "directory"), "a nested array must be refused");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(duplicate_members_and_mixed_content_rejected)
		{
			char *rendered = NULL;

			/*
			 * cJSON preserves duplicate members, so "@id" twice would silently be
			 * last-wins on an attribute and a repeated child key would quietly become two
			 * elements.  Either way the document a producer validated and the tree this
			 * module builds could disagree, so a duplicate is refused and an array stays
			 * the one and only way to express repetition.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"allowed-user\",\"@id\":\"privileged-user\"}}}", "directory"),
					   "a duplicate attribute member must be refused");
			fst_xcheck(fst_xc_rejects("{\"dialplan\":{\"context\":{\"extension\":{},\"extension\":{}}}}", "dialplan"),
					   "a duplicate child member must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{}},\"directory\":{\"user\":{}}}", "directory"),
					   "a duplicate top-level member must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"$\":\"one\",\"$\":\"two\"}}}", "directory"),
					   "a duplicate text member must be refused");

			/* mixed content cannot round trip: the serializer emits an element's text
			   only when the element has no children at all */
			fst_xcheck(fst_xc_rejects("{\"dialplan\":{\"context\":{\"$\":\"text\",\"extension\":{}}}}", "dialplan"),
					   "text alongside element children must be refused");
			fst_xcheck(fst_xc_rejects("{\"dialplan\":{\"context\":{\"extension\":{},\"$\":\"text\"}}}", "dialplan"),
					   "the order of the text member must not change that");

			/* the array form of the same repetition is accepted and yields both elements */
			rendered = fst_xc_render("{\"dialplan\":{\"context\":{\"@name\":\"default\","
									 "\"extension\":[{\"@name\":\"a\"},{\"@name\":\"b\"}]}}}", "dialplan");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "<extension name=\"a\">");
			fst_check_string_has(rendered, "<extension name=\"b\">");
			switch_safe_free(rendered);

			/* and text on a childless element is the canonical form the convention allows */
			rendered = fst_xc_render("{\"configuration\":{\"configuration\":{\"@name\":\"acl.conf\","
									 "\"note\":{\"$\":\"plain text\"}}}}", "configuration");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "<note>plain text</note>");
			switch_safe_free(rendered);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(string_encoding_validated)
		{
			char *rendered = NULL;

			/* the UTF-8 scanner on its own: cJSON does not validate encoding */
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "A", 1), 1);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xC3\xA1", 2), 2);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xE4\xBD\xA0", 3), 3);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xF0\x9F\x98\x80", 4), 4);
			/* a truncated sequence, a bare continuation byte, an overlong encoding, a
			   surrogate half and anything above U+10FFFF are all refused */
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xC3", 1), 0);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xA1", 1), 0);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xC0\xAF", 2), 0);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xED\xA0\x80", 3), 0);
			fst_check_int_equals(xml_curl_json_utf8_len((const unsigned char *) "\xF5\x80\x80\x80", 4), 0);

			/* the code point legality rule the serializer depends on */
			fst_xcheck(xml_curl_json_is_xml_char(0x09) == 1, "tab is legal XML character data");
			fst_xcheck(xml_curl_json_is_xml_char(0x0a) == 1, "newline is legal XML character data");
			fst_xcheck(xml_curl_json_is_xml_char(0x00) == 0, "U+0000 is never legal");
			fst_xcheck(xml_curl_json_is_xml_char(0x0b) == 0, "a vertical tab is not legal");
			fst_xcheck(xml_curl_json_is_xml_char(0x1f) == 0, "a C0 control is not legal");
			fst_xcheck(xml_curl_json_is_xml_char(0xd800) == 0, "a surrogate code point is not legal");
			fst_xcheck(xml_curl_json_is_xml_char(0xfffe) == 0, "U+FFFE is not legal");
			fst_xcheck(xml_curl_json_is_xml_char(0x110000) == 0, "a code point above U+10FFFF is not legal");

			/* the decoded-string validator */
			fst_xcheck(xml_curl_json_is_valid_text("Ana Fern\xC3\xA1ndez") == 1, "valid UTF-8 must be accepted");
			fst_xcheck(xml_curl_json_is_valid_text("a\xC3\x28") == 0, "invalid UTF-8 must be refused");
			fst_xcheck(xml_curl_json_is_valid_text("a\x0b" "b") == 0, "an XML-forbidden control must be refused");
			fst_xcheck(xml_curl_json_is_valid_text(NULL) == 0, "a NULL string must be refused");

			/*
			 * And the same rules through the whole decoder.  "alice\u0000admin" is the
			 * case that matters most: the builders measure with strlen(), so the escape
			 * would truncate the value to the identifier "alice" if it were ever decoded.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"alice\\u0000admin\"}}}", "directory"),
					   "a \\u0000 escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\bb\"}}}", "directory"),
					   "a backspace escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\fb\"}}}", "directory"),
					   "a form feed escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\u001fb\"}}}", "directory"),
					   "a control code point escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\ud800b\"}}}", "directory"),
					   "a lone surrogate must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\udc00\\ud800b\"}}}", "directory"),
					   "a reversed surrogate pair must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\qb\"}}}", "directory"),
					   "an unknown escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\\u00\"}}}", "directory"),
					   "a truncated escape must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"a\xC3\x28\"}}}", "directory"),
					   "raw invalid UTF-8 must be refused");

			/* valid UTF-8 and a correct surrogate pair both survive, and both come back
			   out as numeric character references because the serializer is asked for
			   UTF-8 encoding on the JSON and the XML side alike */
			rendered = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"Fern\xC3\xA1ndez\"}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "Fern&#xE1;ndez");
			switch_safe_free(rendered);

			rendered = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"a\\ud83d\\ude00b\"}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "&#x1F600;");
			switch_safe_free(rendered);
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 4 -- translator robustness and serialisation normalisation
		 * -------------------------------------------------------------------
		 */

		FST_TEST_BEGIN(translator_robustness)
		{
			char *rendered = NULL;

			fst_xcheck(fst_xc_rejects("", "directory"), "an empty document must be refused");
			fst_xcheck(fst_xc_rejects(NULL, "directory"), "a NULL document must be refused");
			fst_xcheck(fst_xc_rejects("   \t\r\n  ", "directory"), "a whitespace-only document must be refused");
			fst_xcheck(fst_xc_rejects("{}", "directory"), "a document with no members must be refused");
			fst_xcheck(fst_xc_rejects("[]", "directory"), "an empty top-level array must be refused");
			fst_xcheck(fst_xc_rejects("[{\"directory\":{\"user\":{}}}]", "directory"), "a top-level array must be refused");
			fst_xcheck(fst_xc_rejects("\"directory\"", "directory"), "a top-level string must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":\"user\"}", "directory"),
					   "a string where the section body belongs must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":[{\"user\":{}}]}", "directory"),
					   "an array where the section body belongs must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":{}}}}", "directory"),
					   "an object where an attribute value belongs must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":[]}}}", "directory"),
					   "an array where an attribute value belongs must be refused");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"$\":{}}}}", "directory"),
					   "an object where text belongs must be refused");

			/* the smallest canonical document still translates */
			rendered = fst_xc_render("{\"directory\":{\"user\":{}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "<user></user>");
			switch_safe_free(rendered);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(empty_element_serialisation_is_symmetric)
		{
			/*
			 * The parity design rests on one core guarantee: an element with neither
			 * children nor text always serialises as <name></name>, never as <name/>,
			 * whichever way the tree was built.  switch_xml_add_child() gives every new
			 * child an empty, non-NULL text pointer, and the parser creates children
			 * through that same function, so a parsed empty element and a translated one
			 * normalise identically.  Nothing else in the tree asserts it, so this does.
			 */
			static const char twin[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"1000\"><params/></user></section></document>";
			char *from_json = NULL;
			char *from_xml = NULL;
			switch_xml_t xml_tree = NULL;

			from_json = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"1000\",\"params\":{}}}}", "directory");
			fst_requires(from_json != NULL);

			xml_tree = switch_xml_parse_str_dynamic((char *) twin, SWITCH_TRUE);
			fst_requires(xml_tree != NULL);
			from_xml = switch_xml_toxml(xml_tree, SWITCH_FALSE);
			switch_xml_free(xml_tree);
			fst_requires(from_xml != NULL);

			fst_check_string_has(from_json, "<params></params>");
			fst_check_string_does_not_have(from_json, "<params/>");
			fst_check_string_has(from_xml, "<params></params>");
			fst_check_string_does_not_have(from_xml, "<params/>");
			/* the self-closing form is normalised the same way on both sides */
			fst_check_string_equals(from_json, from_xml);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 5 -- request path, log hygiene and the dispatch itself
		 * -------------------------------------------------------------------
		 */

		FST_TEST_BEGIN(log_hygiene_redaction)
		{
			char buf[128] = "";

			/*
			 * A configured gateway-url legitimately carries userinfo and query strings
			 * routinely carry tokens, so the one fallback warning has to be able to name
			 * the gateway without reproducing its credentials.
			 */
			fst_check_string_equals(xml_curl_json_redact_url("https://user:pass@example.com:8080/prov?token=abc#frag", buf, sizeof(buf)),
									"https://[redacted]@example.com:8080/prov?[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com/prov", buf, sizeof(buf)), "http://example.com/prov");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com/prov#frag", buf, sizeof(buf)),
									"http://example.com/prov?[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("example.com/prov?t=1", buf, sizeof(buf)), "example.com/prov?[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url(NULL, buf, sizeof(buf)), "(none)");
			fst_check_string_equals(xml_curl_json_redact_url("", buf, sizeof(buf)), "(none)");

			/*
			 * A response Content-Type is attacker-influenced, so it is rendered as
			 * printable ASCII only: a CRLF in a log line could otherwise forge a second
			 * entry, and a terminal escape could rewrite an operator's screen.
			 */
			fst_check_string_equals(xml_curl_json_sanitize_token("application/json", buf, sizeof(buf)), "application/json");
			fst_check_string_equals(xml_curl_json_sanitize_token("application/json\r\nX-Forged: yes", buf, sizeof(buf)),
									"application/json..X-Forged: yes");
			fst_check_string_equals(xml_curl_json_sanitize_token("a\033[2Jb", buf, sizeof(buf)), "a.[2Jb");
			fst_check_string_equals(xml_curl_json_sanitize_token(NULL, buf, sizeof(buf)), "(absent)");
			fst_check_string_equals(xml_curl_json_sanitize_token("", buf, sizeof(buf)), "(absent)");

			/* and the rendering is bounded, so neither can grow a log line without limit */
			fst_check_int_equals((int) strlen(xml_curl_json_sanitize_token("0123456789abcdef", buf, 8)), 7);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(accept_header_append_is_non_destructive)
		{
			switch_curl_slist_t *list = NULL;

			/*
			 * switch_curl_slist_append() returns NULL when it cannot allocate and leaves
			 * the caller's list untouched, so assigning its result straight back would
			 * drop and leak every header appended so far and send a request whose headers
			 * depended on an allocation outcome.  Appending through a temporary is what
			 * makes the failure recoverable.
			 */
			fst_xcheck(xml_curl_json_append_header(&list, "Content-Type: application/x-www-form-urlencoded") == 1,
					   "the first header must be appended");
			fst_requires(list != NULL);
			fst_xcheck(xml_curl_json_append_header(&list, "Accept: application/json") == 1, "the Accept header must be appended after it");
			fst_check_string_equals(list->data, "Content-Type: application/x-www-form-urlencoded");
			fst_requires(list->next != NULL);
			/* order is preserved: Accept is negotiated after the request content type */
			fst_check_string_equals(list->next->data, "Accept: application/json");
			fst_check(list->next->next == NULL);

			/* a refused append reports failure and changes nothing */
			fst_xcheck(xml_curl_json_append_header(&list, NULL) == 0, "a NULL header must be refused");
			fst_xcheck(xml_curl_json_append_header(&list, "") == 0, "an empty header must be refused");
			fst_xcheck(xml_curl_json_append_header(NULL, "X-Test: y") == 0, "a NULL list handle must be refused");
			fst_check_string_equals(list->data, "Content-Type: application/x-www-form-urlencoded");
			fst_check_string_equals(list->next->data, "Accept: application/json");
			fst_check(list->next->next == NULL);

			switch_curl_slist_free_all(list);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(decode_response_dispatch_and_fallback)
		{
			char path[1024] = "";
			switch_xml_t xml = NULL;

			switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, "directory_user_simple.json");

			/* the whole decode step end to end: a JSON body, a JSON content type and the
			   section that was actually requested */
			xml = xml_curl_json_decode_response(path, "application/json; charset=utf-8", XML_CURL_MAX_BYTES,
												"https://provisioner:secret@example.com/prov?token=abc", "directory");
			fst_xcheck(xml != NULL, "a JSON body with a JSON content type must decode");
			switch_xml_free(xml);

			/*
			 * Every remaining case returns NULL, and that NULL *is* the fallback: the
			 * caller then runs the unmodified switch_xml_parse_file() call, so a
			 * misconfigured or misbehaving gateway degrades to today's behaviour instead
			 * of failing the lookup.
			 */
			xml = xml_curl_json_decode_response(path, "text/xml", XML_CURL_MAX_BYTES, "https://example.com/prov", "directory");
			fst_xcheck(xml == NULL, "a text/xml content type must not be decoded as JSON");
			switch_xml_free(xml);

			xml = xml_curl_json_decode_response(path, "application/json", XML_CURL_MAX_BYTES, "https://example.com/prov", "dialplan");
			fst_xcheck(xml == NULL, "a body whose root key is not the requested section must be refused");
			switch_xml_free(xml);

			xml = xml_curl_json_decode_response(path, NULL, XML_CURL_MAX_BYTES, "https://example.com/prov", "directory");
			fst_xcheck(xml == NULL, "a response with no Content-Type at all must fall back");
			switch_xml_free(xml);

			xml = xml_curl_json_decode_response(path, "application/json", XML_CURL_MAX_BYTES, "https://example.com/prov", NULL);
			fst_xcheck(xml == NULL, "a fetch with no requested section must fall back");
			switch_xml_free(xml);

			xml = xml_curl_json_decode_response(NULL, "application/json", XML_CURL_MAX_BYTES, "https://example.com/prov", "directory");
			fst_xcheck(xml == NULL, "a fetch with no captured body must fall back");
			switch_xml_free(xml);

			/* the XML twin of that fixture is a valid response body, and it must still
			   not decode as JSON -- which is the mismatch case the module has to survive */
			switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, "directory_user_simple.xml");
			xml = xml_curl_json_decode_response(path, "application/json", XML_CURL_MAX_BYTES, "https://example.com/prov", "directory");
			fst_xcheck(xml == NULL, "an XML body announced as JSON must fall back rather than decode");
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(response_format_does_not_divert_file_url)
		{
			static const char document[] = "<document type=\"freeswitch/xml\">\n<section name=\"directory\">\n"
				"<user id=\"1000\"/>\n</section>\n</document>\n";
			const char *formats[3];
			char path[1024] = "";
			char url[1088] = "";
			xml_binding_t binding;
			switch_xml_t xml = NULL;
			switch_xml_t section_tag = NULL;
			switch_xml_t user_tag = NULL;
			int i;

			formats[0] = NULL;
			formats[1] = "json";
			formats[2] = "JSON";

			switch_snprintf(path, sizeof(path), "%s%s%s", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, FST_XC_TEMP_XML);
			switch_snprintf(url, sizeof(url), "file:%s", path);
			fst_requires(fst_xc_write_file(path, document, strlen(document)));

			/*
			 * The file: URL shortcut returns before any HTTP request is attempted, so
			 * there is no content type to negotiate and no Accept header to send.  It
			 * therefore stays XML-only whatever response-format says, and these three
			 * runs must be indistinguishable.  "JSON" also covers the case-insensitive
			 * comparison the parameter is documented with, and the first run covers the
			 * absent-parameter default that the wholesale memset() in do_config()
			 * guarantees.
			 */
			for (i = 0; i < 3; i++) {
				memset(&binding, 0, sizeof(binding));
				binding.url = (char *) url;
				binding.response_format = (char *) formats[i];

				if (!i) {
					fst_xcheck(binding.response_format == NULL, "an absent response-format leaves the member NULL");
				}

				xml = xml_url_fetch("directory", "user", "id", "1000", NULL, &binding);
				fst_requires(xml != NULL);
				section_tag = switch_xml_child(xml, "section");
				fst_requires(section_tag != NULL);
				user_tag = switch_xml_child(section_tag, "user");
				fst_requires(user_tag != NULL);
				fst_check_string_equals(switch_xml_attr_soft(user_tag, "id"), "1000");
				switch_xml_free(xml);
			}

			/* a fetch with no binding at all is refused before anything else happens */
			fst_check(xml_url_fetch("directory", "user", "id", "1000", NULL, NULL) == NULL);

			unlink(path);
		}
		FST_TEST_END()

		/*
		 * Declared last: this is the only case that mutates module-wide state, so
		 * nothing that runs after it can be affected by what it leaves behind.
		 */
		FST_TEST_BEGIN(response_format_configuration_parsing)
		{
			char dir_path[1024] = "";
			char dial_path[1024] = "";
			char *conf = NULL;
			xml_binding_t *json_binding = NULL;
			xml_binding_t *xml_binding = NULL;
			switch_xml_t located = NULL;
			switch_xml_t located_node = NULL;
			switch_xml_t extension_tag = NULL;

			switch_snprintf(dir_path, sizeof(dir_path), "%s%s%s", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
							FST_XC_TEMP_DIRECTORY_XML);
			switch_snprintf(dial_path, sizeof(dial_path), "%s%s%s", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
							FST_XC_TEMP_DIALPLAN_XML);
			fst_requires(fst_xc_write_file(dir_path, fst_xc_directory_document, strlen(fst_xc_directory_document)));
			fst_requires(fst_xc_write_file(dial_path, fst_xc_dialplan_document, strlen(fst_xc_dialplan_document)));

			conf = switch_mprintf(fst_xc_conf_template, dir_path, dial_path);
			fst_requires(conf != NULL);

			/*
			 * do_config() allocates every binding and duplicates every string member into
			 * the module pool, so it has to be given one.  The per-test pool is exactly
			 * right: it is destroyed at teardown, and every binding it backs is removed
			 * from the core before this case ends.
			 */
			globals.pool = fst_pool;

			/*
			 * The fixture configuration root deliberately carries no xml_curl.conf
			 * section, so with no provider registered switch_xml_open_cfg() misses in the
			 * binding list and in the static root alike and the module takes its
			 * documented error branch.  That is what makes this half deterministic.
			 */
			fst_xcheck(do_config() == SWITCH_STATUS_TERM, "an absent xml_curl.conf must be reported, not assumed");

			/* now present the configuration through the same public binding API
			   mod_xml_curl itself registers its fetch function with */
			fst_xcheck(switch_xml_bind_search_function_ret(fst_xc_conf_search, switch_xml_parse_section_string("configuration"),
														   (void *) conf, NULL) == SWITCH_STATUS_SUCCESS,
					   "the configuration provider must register");

			/*
			 * Both bindings have to be accepted: one carrying response-format alongside a
			 * broad sample of the module's other parameters, one carrying none of it at
			 * all.  response-format sits at the tail of a chain of nineteen, and success
			 * on this document is what shows the other eighteen still parse.
			 */
			fst_xc_observed_binding_count = 0;
			fst_xcheck(do_config() == SWITCH_STATUS_SUCCESS,
					   "a configuration mixing present and absent response-format must parse");
			fst_check_int_equals(fst_xc_observed_binding_count, 2);

			/*
			 * The binding that opted in carries the parameter, and it carries it as a
			 * copy: do_config() has already freed the configuration tree the value was
			 * parsed out of, so a member that still reads "json" here can only be the
			 * pool-duplicated string.
			 */
			json_binding = (xml_binding_t *) fst_xc_observed_bindings[0];
			fst_requires(json_binding != NULL);
			fst_requires(json_binding->response_format != NULL);
			fst_check_string_equals(json_binding->response_format, "json");

			/*
			 * The binding that omitted it is left at the NULL the wholesale memset()
			 * wrote, which is the whole mechanism by which a binding that does not ask
			 * for JSON stays XML-only.
			 */
			xml_binding = (xml_binding_t *) fst_xc_observed_bindings[1];
			fst_requires(xml_binding != NULL);
			fst_check(xml_binding->response_format == NULL);

			/*
			 * And the eighteen parameters that were already there are unchanged by the
			 * nineteenth: a sample spanning a string, an integer, a long, a boolean-gated
			 * flag, a size and the post-variable hash is read back off the same binding.
			 */
			fst_check_string_equals(json_binding->method, "POST");
			fst_check_string_equals(json_binding->cred, "provisioner:secret");
			fst_check_string_equals(json_binding->bindings, "directory");
			fst_check_string_equals(json_binding->cookie_file, "/tmp/test_mod_xml_curl_cookies");
			fst_check_int_equals(json_binding->timeout, 7);
			fst_check_int_equals((int) json_binding->curl_max_bytes, 65536);
			fst_check_int_equals(json_binding->use_dynamic_url, 1);
			fst_check_int_equals((int) json_binding->enable_cacert_check, 1);
			fst_check_int_equals((int) json_binding->enable_ssl_verifyhost, 1);
			/* the 100-continue parameter is a suppression: a false value clears the
			   default rather than setting a flag, and that inversion is easy to break */
			fst_check_int_equals(json_binding->disable100continue, 0);
			/* "digest" is OR-ed into the basic default rather than replacing it */
			fst_check_int_equals((int) json_binding->auth_scheme, (int) (CURLAUTH_BASIC | CURLAUTH_DIGEST));
			/* an explicit POST method is still a POST, not a GET-style request */
			fst_check_int_equals(json_binding->use_get_style, 0);
			fst_check(json_binding->vars_map != NULL);

			/* the binding that configured none of them keeps the module's own defaults */
			fst_check_string_equals(xml_binding->bindings, "dialplan");
			fst_check(xml_binding->method == NULL);
			fst_check(xml_binding->cred == NULL);
			fst_check_int_equals(xml_binding->timeout, 0);
			fst_check_int_equals(xml_binding->disable100continue, 1);
			fst_check_int_equals((int) xml_binding->auth_scheme, (int) CURLAUTH_BASIC);
			fst_check_int_equals((int) xml_binding->curl_max_bytes, (int) XML_CURL_MAX_BYTES);
			fst_check(xml_binding->vars_map == NULL);

			/*
			 * Close the loop.  The binding do_config() built FROM the response-format
			 * arm is now registered for the directory section, and the fixture
			 * configuration root has no directory section at all -- so a hit here can
			 * only have come through that binding, and it proves a binding carrying
			 * response-format still resolves the XML document its file: URL names,
			 * exactly as a binding without the parameter does.
			 */
			fst_xcheck(switch_xml_locate("directory", "user", "id", "1000", &located, &located_node, NULL,
										 SWITCH_FALSE) == SWITCH_STATUS_SUCCESS,
					   "the JSON-format binding must resolve its file: URL");
			if (located_node) {
				fst_check_string_equals(switch_xml_attr_soft(located_node, "id"), "1000");
			}
			switch_xml_free(located);
			located = NULL;
			located_node = NULL;

			/* the same round trip for the binding that carries no response-format; the
			   extension name is what distinguishes the binding's answer from the root's */
			fst_xcheck(switch_xml_locate("dialplan", "context", "name", "default", &located, &located_node, NULL,
										 SWITCH_FALSE) == SWITCH_STATUS_SUCCESS,
					   "the XML-format binding must resolve its file: URL");
			if (located_node && (extension_tag = switch_xml_child(located_node, "extension"))) {
				fst_check_string_equals(switch_xml_attr_soft(extension_tag, "name"), "binding_only");
			} else {
				fst_fail("the located dialplan context is not the one the binding names");
			}
			switch_xml_free(located);

			fst_check(switch_xml_unbind_search_function_ptr(fst_xc_conf_search) == SWITCH_STATUS_SUCCESS);

			/*
			 * Shutdown is asserted rather than left implicit.  It releases the
			 * enable-post-var hash this configuration created and removes the fetch
			 * bindings do_config() registered, so the pool this case borrowed can be torn
			 * down cleanly at teardown.
			 */
			fst_check(mod_xml_curl_shutdown() == SWITCH_STATUS_SUCCESS);
			globals.pool = NULL;

			switch_safe_free(conf);
			unlink(dir_path);
			unlink(dial_path);
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
