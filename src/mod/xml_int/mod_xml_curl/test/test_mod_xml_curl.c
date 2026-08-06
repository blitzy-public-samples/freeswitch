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
 * This suite covers mod_xml_curl's opt-in BadgerFish JSON response decoder and
 * the hardening that decoder is required to carry.  It observes the production
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
 * The eighteen paired fixtures under test/fixtures/ are the parity contract: a
 * BadgerFish JSON document and its XML twin must serialise to byte-identical
 * output through one and the same switch_xml_toxml() call.  Six pairs cover each
 * of the configuration, directory and dialplan provisioning sections, and each
 * pair has a case of its own that asserts the construct it was added for as well
 * as the parity.  All eighteen are named in the required-corpus list further
 * down, so a pair cannot silently leave the corpus.
 *
 * The XML side of every pair is read with switch_xml_parse_file_simple().
 * switch_xml_parse_file() is deliberately never used on a fixture: it takes
 * the global FILE_LOCK, writes <basename>.fsxml into the log directory, and
 * runs the configuration preprocessor, which deletes every whole line holding
 * <include>, </include> or <? -- which would erase a minified, single-line
 * fixture entirely.
 *
 * Adversarial inputs to the TRANSLATOR are in-source string literals rather than
 * extra fixture files, so the paired fixture set stays exactly what it
 * documents: the canonical, well-formed contract.  The five badgerfish_invalid_*
 * files beside those pairs are not translator inputs at all - they are the
 * counter-examples the standalone contract validator has to refuse, and only the
 * last case in this suite reads them.
 *
 * DRIVING THE PRODUCTION DISPATCH WITHOUT A NETWORK PEER
 * ------------------------------------------------------
 * Asserting that a JSON failure "falls back" is only meaningful if the assertion
 * runs the code that actually performs the fallback.  That code is the HTTP-200
 * block inside xml_url_fetch(): the format dispatch, and then the
 * switch_xml_parse_file() call that every JSON failure edge converges on.  A
 * test that only observes the decode helper returning NULL would stay green if
 * that parse call were deleted, which is precisely the property a fallback test
 * has to rule out.
 *
 * Reaching it needs an HTTP 200, and this suite is required to be hermetic: no
 * listening port, no peer, no wall-clock dependence.  Those two requirements are
 * reconciled by replacing the transport rather than the peer.  For the remainder
 * of this translation unit the four libcurl entry points the module uses --
 * easy_init, easy_perform, easy_getinfo and easy_cleanup -- plus slist_free_all
 * and switch_uuid_format are interposed, exactly as switch_xml_bind_search_function
 * is interposed above and for the same reason: it is the only way to observe and
 * steer a file-static code path without editing the module.  The handle stays a
 * REAL libcurl handle, so every option the module sets is set for real; only the
 * request itself is replaced by a canned response written into the module's own
 * temporary file.  Everything downstream of the transport -- the temporary file,
 * the content-type probe, the dispatch, the BadgerFish decode, the production XML
 * parse, the error and warning reporting and the unlink -- is the production code
 * path, unmodified.
 *
 * DETERMINISM
 * -----------
 * No case performs network I/O, opens a listening port, starts a thread, or
 * depends on the wall clock.
 *
 * Files.  Every case that needs a document on disk writes it into one private
 * directory created per test case under an unpredictable name, with owner-only
 * permissions, and removed again unconditionally.  A fixed name under the shared
 * temp directory would be both a collision between concurrent runs and a symlink
 * target an unprivileged local user could pre-create.
 *
 * Configuration.  The one case that needs a configuration document injects it
 * through switch_xml_bind_search_function_ret() -- the same public binding API
 * mod_xml_curl itself registers with -- and removes it again with
 * switch_xml_unbind_search_function_ptr() before the case ends.  FCTX runs every
 * case in one process in declaration order, so a binding left behind would be
 * visible to every later case.  The teardown block repeats every removal as a
 * safety net, because a failed assertion leaves a case early.
 *
 * Log lines.  switch_log_printf() enqueues onto a single FIFO queue that one
 * dispatcher thread drains, so a line is not visible to a bound logger the moment
 * it is written.  The cases that count log lines therefore use a queue barrier: a
 * unique sentinel line is emitted and waited for before the counters are armed,
 * and a second sentinel is emitted and waited for before they are read.  Because
 * the queue is FIFO and one thread drains it, observing a sentinel proves every
 * line enqueued before it has already been dispatched.  Every wait is bounded and
 * reports a failure rather than blocking.
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

/*
 * -------------------------------------------------------------------------
 * INTERPOSING THE HTTP TRANSPORT
 * -------------------------------------------------------------------------
 * The names below are redirected for the remainder of this translation unit so
 * that xml_url_fetch() can be driven end to end -- through the real HTTP-200
 * gate, the real format dispatch and the real switch_xml_parse_file() fallback --
 * with no peer, no port and no dependence on anything outside this process.
 *
 * Two different mechanisms are at work, and the difference matters:
 *
 *   switch_uuid_format() is declared by <switch.h>, which has already been
 *   processed, so this macro rewrites only the module's CALL.  The shim has to be
 *   declared explicitly, below, because nothing else declares it.  It exists so
 *   the response body can be written into the very temporary file the production
 *   fetch decodes: the file name is composed from a uuid the module generates
 *   itself, and recording the formatted value is the least invasive way to learn
 *   it.  The uuid bytes still come from the un-interposed switch_uuid_get(), so
 *   the production file name stays as unpredictable as it is in production.
 *
 *   The five curl names are declared by <switch_curl.h>, which the module source
 *   includes below -- AFTER these macros.  Their declarations are therefore
 *   rewritten into declarations of the shims, which is why the shims are defined
 *   with SWITCH_DECLARE and external linkage rather than as statics: they have to
 *   match the prototypes the header now emits.  switch_curl_easy_setopt is
 *   deliberately NOT interposed; it is itself a macro in that header, and
 *   redefining it would be a macro redefinition the build refuses.  Nothing is
 *   lost by leaving it alone, because switch_curl_easy_init() returns a genuine
 *   libcurl handle and every option the module sets is really set on it.
 *
 * slist_free_all is interposed to record the header list at the moment it is
 * released, which is the last point at which it is exactly what was handed to
 * CURLOPT_HTTPHEADER -- so the Accept negotiation is asserted from what the
 * request carried rather than from what the code appeared to append.
 */
static void fst_xc_uuid_format(char *buffer, const switch_uuid_t *uuid);

#define switch_uuid_format fst_xc_uuid_format
#define switch_curl_easy_init fst_xc_curl_easy_init
#define switch_curl_easy_perform fst_xc_curl_easy_perform
#define switch_curl_easy_getinfo fst_xc_curl_easy_getinfo
#define switch_curl_easy_cleanup fst_xc_curl_easy_cleanup
#define switch_curl_slist_free_all fst_xc_curl_slist_free_all

#include "../mod_xml_curl.c"

/*
 * -------------------------------------------------------------------------
 * THE DECODE CONTRACT THIS SUITE WAS WRITTEN AGAINST
 * -------------------------------------------------------------------------
 * The module's decode step takes one more argument than it used to: the name of
 * the binding whose fetch degraded, which the fallback event reports.  That
 * argument only ever reaches the event, so it is the production dispatch point
 * in xml_url_fetch() that has something to say about it, and the cases that
 * assert the event drive that dispatch point end to end.
 *
 * The case that covers the decode step in isolation predates the event and is
 * not about it.  Widening its seven calls to carry an argument they have no
 * opinion on would have rewritten a case whose subject did not change, and a
 * test rewritten for a reason unrelated to what it asserts is a test whose
 * history stops meaning anything.  The pre-existing five-argument spelling is
 * therefore kept as what it always was, right here, forwarding to the wider form
 * with no binding: `binding_name' is documented as optional at its definition,
 * and NULL selects exactly the behaviour that existed before it.
 *
 * This lives in the test translation unit rather than in the module because the
 * module has no caller for it -- xml_url_fetch() always has a binding -- and an
 * unused file-static function is a -Wunused-function error under the tree's
 * -Werror.  Nothing is de-staticised and no production symbol is added: the
 * module source above is already part of this translation unit, so this is one
 * more static function beside the ones it brought.
 */
static switch_xml_t xml_curl_json_decode_response(const char *filename, const char *content_type, switch_size_t max_bytes, const char *url,
												  const char *section)
{
	return xml_curl_json_decode_response_ex(filename, content_type, max_bytes, url, section, NULL);
}

/*
 * Fixture directory, composed at compile time from the base directory the build
 * defines for this target.  A bare relative path would silently resolve against
 * whatever directory the test happened to be started from.
 */
#define FST_XC_FIXTURE_DIR SWITCH_TEST_BASE_DIR_OVERRIDE SWITCH_PATH_SEPARATOR "fixtures" SWITCH_PATH_SEPARATOR

/*
 * Base names of the documents cases write to disk.  Each one is resolved inside
 * the private per-case directory below, never directly under the shared core
 * temporary directory.
 */

/* Response body written to disk by the case that drives the body reader. */
#define FST_XC_TEMP_BODY "body.json"

/* Document written to disk by the case that drives the file: URL shortcut. */
#define FST_XC_TEMP_XML "shortcut.xml"

/* Documents written to disk by the configuration-parsing case, one per binding. */
#define FST_XC_TEMP_DIRECTORY_XML "directory.xml"
#define FST_XC_TEMP_DIALPLAN_XML "dialplan.xml"

/* Response bodies written to disk by the case that drives the HTTP dispatch. */
#define FST_XC_TEMP_RESPONSE_XML "response.xml"

/* Target the case that proves a planted symlink is not written through uses. */
#define FST_XC_TEMP_CANARY "canary"

/*
 * Cookie jar paths used by the case that drives the jar safety gate.  One name per
 * shape the gate has to tell apart, because the gate's answer depends on what the
 * name already is: nothing at all, a private regular file of ours, a symlink, a
 * file other users can write, or a file belonging to somebody else.
 */
#define FST_XC_TEMP_COOKIE_NEW "cookiejar-new"
#define FST_XC_TEMP_COOKIE_KEEP "cookiejar-keep"
#define FST_XC_TEMP_COOKIE_LINK "cookiejar-link"
#define FST_XC_TEMP_COOKIE_LOOSE "cookiejar-loose"
#define FST_XC_TEMP_COOKIE_ALIEN "cookiejar-alien"

/* Directory fixture used to drive the gate's rule about the directory holding a jar. */
#define FST_XC_TEMP_COOKIE_DIR "cookiejar-dir"

/*
 * -------------------------------------------------------------------------
 * PRIVATE PER-CASE TEMPORARY DIRECTORY
 * -------------------------------------------------------------------------
 * Everything this suite writes goes inside one directory created per test case,
 * and none of it goes directly into SWITCH_GLOBAL_dirs.temp_dir.  That directory
 * is shared and, when the core has no configured override, is the system-wide
 * world-writable one, so a fixed file name there is two distinct problems: two
 * concurrent runs of this binary would truncate each other's documents, and an
 * unprivileged local user could pre-create the name as a symlink and have the
 * suite truncate whatever it pointed at.
 *
 * The name carries a uuid, so it is not guessable.  switch_dir_make() is used
 * rather than a recursive variant precisely because it fails when the name
 * already exists -- including when it exists as a symlink -- so the creation is
 * the exclusivity check and there is no window between testing and creating.  The
 * permissions are owner-only, and the files inside are created with O_EXCL and
 * O_NOFOLLOW so neither a leftover entry nor a symlink planted mid-run can be
 * written through.
 */
static char fst_xc_temp_dir[1024] = "";

/*
 * The cleanup verdict, kept across cases.  The active path is cleared once a case
 * is done with it, so the next case creates a fresh directory; it is copied here
 * first so that residue can still be named -- by the error the removal logs, and by
 * the case that asserts the previous case's directory is gone.
 *
 * The directory is this suite's isolation boundary, so a surviving entry means the
 * next case would run against state it did not create.  fst_xc_temp_dir_create()
 * refuses outright once fst_xc_temp_dir_failures is non-zero, which turns residue
 * into a failure of the next case rather than a diagnostic nobody reads.  The
 * counter is the condition rather than the residue name, because a directory that
 * survives rmdir() is residue too and carries no entry name to record.
 */
static char fst_xc_temp_dir_last[1024] = "";
static char fst_xc_temp_dir_residue[256] = "";
static int fst_xc_temp_dir_failures = 0;

/* Compose the absolute path of `name` inside the private directory. */
static const char *fst_xc_temp_path(const char *name, char *buf, switch_size_t buflen)
{
	switch_snprintf(buf, buflen, "%s%s%s", fst_xc_temp_dir, SWITCH_PATH_SEPARATOR, name);

	return buf;
}

/* Create the private directory.  Returns 1 on success, and is idempotent. */
static int fst_xc_temp_dir_create(void)
{
	switch_uuid_t uuid;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";

	if (fst_xc_temp_dir_failures) {
		/* An earlier teardown left something behind, so isolation is already lost and
		   there is nothing this function could do to restore it. */
		return 0;
	}

	if (*fst_xc_temp_dir) {
		return 1;
	}

	switch_uuid_get(&uuid);
	switch_uuid_format(uuid_str, &uuid);

	switch_snprintf(fst_xc_temp_dir, sizeof(fst_xc_temp_dir), "%s%stest_mod_xml_curl.%s", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
					uuid_str);

	if (switch_dir_make(fst_xc_temp_dir, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE | SWITCH_FPROT_UEXECUTE, NULL) != SWITCH_STATUS_SUCCESS) {
		*fst_xc_temp_dir = '\0';
		return 0;
	}

	return 1;
}

/*
 * Sticky record that some case's teardown did not finish.  Teardown runs outside
 * any test's assertion scope, so a failure there cannot be reported where it
 * happens; it is latched here instead and asserted by the setup of the next case
 * and by the final declared case, which is what turns unfinished cleanup into a
 * red run rather than a quiet one.
 */
static int fst_xc_teardown_failed = 0;

/*
 * Remove the private directory and everything this suite put in it, and report
 * whether that actually happened.  Returns 1 when the directory and every entry in
 * it are gone, 0 otherwise.  Called from the teardown block, so it runs whether a
 * case finished or left early, and it is safe to call when nothing was created.
 *
 * ENOENT is the expected answer for a document the case never wrote, so only other
 * errors count.  A failure is reported three ways so that it can be attributed after
 * the fact: the offending path is logged with its errno, the directory name is
 * persisted to fst_xc_temp_dir_last and the first offender to
 * fst_xc_temp_dir_residue before the active path is cleared, and
 * fst_xc_teardown_failed is latched so the next case's setup and the final declared
 * case both fail on it.  Nothing is short-circuited: a logger that would not unbind
 * must not stop the private directory being cleaned, or one reported defect would
 * become two.
 */
static int fst_xc_temp_dir_destroy(void)
{
	static const char *names[] = {
		FST_XC_TEMP_BODY, FST_XC_TEMP_XML, FST_XC_TEMP_DIRECTORY_XML, FST_XC_TEMP_DIALPLAN_XML, FST_XC_TEMP_RESPONSE_XML,
		FST_XC_TEMP_COOKIE_LINK, FST_XC_TEMP_CANARY, FST_XC_TEMP_COOKIE_NEW, FST_XC_TEMP_COOKIE_KEEP, FST_XC_TEMP_COOKIE_LOOSE,
		FST_XC_TEMP_COOKIE_ALIEN, NULL
	};
	char path[1024] = "";
	char entry[256] = "";
	switch_memory_pool_t *pool = NULL;
	switch_dir_t *dir = NULL;
	const char *found = NULL;
	int failures = 0;
	int pass = 0;
	int i = 0;

	if (!*fst_xc_temp_dir) {
		return 1;
	}

	for (i = 0; names[i]; i++) {
		fst_xc_temp_path(names[i], path, sizeof(path));

		if (unlink(path) != 0 && errno != ENOENT) {
			failures++;

			if (!*fst_xc_temp_dir_residue) {
				switch_copy_string(fst_xc_temp_dir_residue, names[i], sizeof(fst_xc_temp_dir_residue));
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "test_mod_xml_curl: [%s] could not be removed: %s\n", path,
							  strerror(errno));
		}
	}

	/*
	 * Anything the directory still holds is residue the sweep above did not know
	 * about.  It is named and counted before it is removed, because the name is the
	 * whole diagnostic.
	 *
	 * Two bounded passes: removing an entry part-way through an enumeration leaves it
	 * unspecified whether the entries after it are still reported, so a second pass
	 * covers anything the first could have missed.  The pool is created here rather
	 * than reusing fst_pool, which the teardown prologue has already destroyed.
	 */
	if (switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS) {
		for (pass = 0; pass < 2; pass++) {
			int seen = 0;

			if (switch_dir_open(&dir, fst_xc_temp_dir, pool) != SWITCH_STATUS_SUCCESS) {
				break;
			}

			while ((found = switch_dir_next_file(dir, entry, sizeof(entry)))) {
				seen++;
				failures++;

				if (!*fst_xc_temp_dir_residue) {
					switch_copy_string(fst_xc_temp_dir_residue, found, sizeof(fst_xc_temp_dir_residue));
				}

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "test_mod_xml_curl: unexpected residue [%s] left in [%s]\n", found,
								  fst_xc_temp_dir);
				unlink(fst_xc_temp_path(found, path, sizeof(path)));
			}

			switch_dir_close(dir);
			dir = NULL;

			if (!seen) {
				break;
			}
		}

		switch_core_destroy_memory_pool(&pool);
	}

	switch_copy_string(fst_xc_temp_dir_last, fst_xc_temp_dir, sizeof(fst_xc_temp_dir_last));

	if (rmdir(fst_xc_temp_dir) != 0 && errno != ENOENT) {
		failures++;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "test_mod_xml_curl: directory [%s] could not be removed: %s\n",
						  fst_xc_temp_dir, strerror(errno));
	}

	*fst_xc_temp_dir = '\0';
	fst_xc_temp_dir_failures += failures;

	if (failures) {
		/*
		 * Latched for the assertion scopes that can actually report it: the next case's
		 * setup and the final declared case.  This block's own caller is the teardown
		 * block, which runs outside any test, so the flag -- not the return value -- is
		 * what turns unfinished cleanup into a red run.
		 */
		fst_xc_teardown_failed = 1;
	}

	return failures == 0;
}

/*
 * -------------------------------------------------------------------------
 * THE INTERPOSED TRANSPORT
 * -------------------------------------------------------------------------
 * `canned` is what the next fetch will be answered with; the rest is what the
 * fetch was observed to do.  A case fills in the first group, calls
 * xml_url_fetch(), and then reads the second.
 */
#define FST_XC_MAX_SENT_HEADERS 16

/*
 * The fetch builds up to two header lists and releases them in a fixed order --
 * the Content-Type list first, then the list the Expect: suppression uses -- so
 * recording them into separate buckets in that order says which list each entry
 * actually landed on.  That distinction is the whole point of one of the
 * assertions below: when a binding both suppresses the 100-continue and asks for
 * JSON, the production code hands curl the second list and only the second list,
 * so an Accept entry appended to the first would be silently dropped from the
 * request.  Asserting "both entries, same bucket" is what rules that out.
 */
#define FST_XC_HEADER_LISTS 2
#define FST_XC_HEADER_LIST_CONTENT_TYPE 0
#define FST_XC_HEADER_LIST_EXPECT 1

/*
 * A fetch hands curl its header list once, or twice when it also suppresses the
 * 100-continue, so four slots is room to spare for recording every handoff.
 * Recording the handoff at all is what makes the Accept negotiation an assertion
 * about the request rather than about the list-building code: a list that is
 * built correctly but never given to curl is not sent, and the release-time
 * observation below cannot tell the difference on its own.
 */
#define FST_XC_MAX_HTTPHEADER_SETS 4

typedef struct {
	/* canned response */
	const char *body;
	switch_size_t body_len;
	const char *content_type;
	int content_type_present;
	long response_code;
	switch_CURLcode perform_result;
	/* observed */
	int perform_count;
	int body_write_failures;
	char sent_headers[FST_XC_HEADER_LISTS][FST_XC_MAX_SENT_HEADERS][192];
	int sent_header_count[FST_XC_HEADER_LISTS];
	int released_list_count;
	const void *released_list[FST_XC_HEADER_LISTS];
	/*
	 * When each interposed curl entry point ran.  Every shim bumps op_seq, so the
	 * fields below are ordinals that can be compared with one another -- which is
	 * the only way to assert that the response metadata is read while the handle
	 * is still alive rather than merely that it is read at all.
	 */
	int op_seq;
	switch_CURL *init_handle;
	int perform_op;
	int response_code_op;
	int content_type_op;
	int cleanup_op;
	int getinfo_after_cleanup;
	int getinfo_on_unknown_handle;
	int content_type_storage_freed;
	int content_type_alloc_failures;
	int content_type_served_from_handle;
	int setopt_count;
	int httpheader_set_count;
	const void *httpheader_list[FST_XC_MAX_HTTPHEADER_SETS];
	int httpheader_op[FST_XC_MAX_HTTPHEADER_SETS];
	const void *last_httpheader_list;
	int last_httpheader_op;
	switch_CURL *httpheader_handle;
	/* the temporary file name the fetch composed, learned from switch_uuid_format() */
	char last_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char last_body_path[1024];
} fst_xc_transport_t;

static fst_xc_transport_t fst_xc_transport;

/*
 * libcurl owns the string CURLINFO_CONTENT_TYPE hands back and frees it together
 * with the easy handle, so the fetch has to take that read while the handle is alive
 * and copy the value out.  These slots reproduce that ownership rather than describe
 * it: every handle the init shim hands out gets a slot, the first content-type read
 * on that handle allocates a copy the slot owns, and the cleanup shim overwrites that
 * copy and frees it.  So a read taken after cleanup is refused and counted rather
 * than served; a fetch that kept libcurl's pointer instead of copying the value reads
 * the overwritten bytes at the decode site, which the always-on address sanitizer
 * reports as a use-after-free; and a read against a handle this shim never issued is
 * refused too.
 */
#define FST_XC_MAX_TRACKED_HANDLES 4

typedef struct {
	switch_CURL *handle;
	char *content_type;
	int alive;
} fst_xc_handle_t;

static fst_xc_handle_t fst_xc_handles[FST_XC_MAX_TRACKED_HANDLES];
static int fst_xc_handle_count = 0;

static void fst_xc_handles_reset(void)
{
	int i = 0;

	for (i = 0; i < FST_XC_MAX_TRACKED_HANDLES; i++) {
		if (fst_xc_handles[i].content_type) {
			free(fst_xc_handles[i].content_type);
			fst_xc_handles[i].content_type = NULL;
		}
	}

	memset(fst_xc_handles, 0, sizeof(fst_xc_handles));
	fst_xc_handle_count = 0;
}

/*
 * Most recent slot registered for `handle`, or -1.  The search runs backwards
 * because libcurl is free to hand the same address out again after a cleanup, and
 * the newest registration is the one that describes the live handle.
 */
static int fst_xc_handle_find(switch_CURL *handle)
{
	int i = 0;

	if (!handle) {
		return -1;
	}

	for (i = fst_xc_handle_count - 1; i >= 0; i--) {
		if (fst_xc_handles[i].handle == handle) {
			return i;
		}
	}

	return -1;
}

/* Arm the transport for one fetch and clear everything observed from the last. */
static void fst_xc_transport_arm(const char *body, const char *content_type, long response_code)
{
	fst_xc_handles_reset();
	memset(&fst_xc_transport, 0, sizeof(fst_xc_transport));

	fst_xc_transport.body = body;
	fst_xc_transport.body_len = body ? strlen(body) : 0;
	fst_xc_transport.content_type = content_type;
	fst_xc_transport.content_type_present = content_type ? 1 : 0;
	fst_xc_transport.response_code = response_code;
	fst_xc_transport.perform_result = CURLE_OK;
}

/* 1 when the request built `header` onto the numbered list; see the note above. */
static int fst_xc_transport_header_in_list(int list, const char *header)
{
	int i = 0;

	if (list < 0 || list >= FST_XC_HEADER_LISTS) {
		return 0;
	}

	for (i = 0; i < fst_xc_transport.sent_header_count[list] && i < FST_XC_MAX_SENT_HEADERS; i++) {
		if (!strcmp(fst_xc_transport.sent_headers[list][i], header)) {
			return 1;
		}
	}

	return 0;
}

/* 1 when the request carried `header` on either list. */
static int fst_xc_transport_sent_header(const char *header)
{
	int list = 0;

	for (list = 0; list < FST_XC_HEADER_LISTS; list++) {
		if (fst_xc_transport_header_in_list(list, header)) {
			return 1;
		}
	}

	return 0;
}

/*
 * Which recorded list curl was actually left holding, identified by matching the
 * pointer handed to CURLOPT_HTTPHEADER against the pointers released afterwards.
 * This is a pointer-identity comparison on purpose: two lists can carry identical
 * text, and the question being answered is which object the request carried, not
 * which text was built.  Returns -1 when the fetch never handed curl a list, or
 * handed it one this shim never saw released.
 */
static int fst_xc_transport_request_list(void)
{
	int list = 0;

	if (!fst_xc_transport.last_httpheader_list) {
		return -1;
	}

	for (list = 0; list < FST_XC_HEADER_LISTS; list++) {
		if (fst_xc_transport.released_list[list] == fst_xc_transport.last_httpheader_list) {
			return list;
		}
	}

	return -1;
}

static int fst_xc_transport_request_header(const char *header)
{
	return fst_xc_transport_header_in_list(fst_xc_transport_request_list(), header);
}

/*
 * Reimplements the canonical uuid formatting so the value can be recorded on the
 * way past.  The bytes are still whatever switch_uuid_get() produced.
 */
static void fst_xc_uuid_format(char *buffer, const switch_uuid_t *uuid)
{
	static const char hex[] = "0123456789abcdef";
	const unsigned char *bytes = NULL;
	int i = 0;
	int o = 0;

	if (!buffer) {
		return;
	}

	if (!uuid) {
		*buffer = '\0';
		return;
	}

	bytes = (const unsigned char *) uuid->data;

	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			buffer[o++] = '-';
		}
		buffer[o++] = hex[(bytes[i] >> 4) & 0x0f];
		buffer[o++] = hex[bytes[i] & 0x0f];
	}

	buffer[o] = '\0';

	switch_copy_string(fst_xc_transport.last_uuid, buffer, sizeof(fst_xc_transport.last_uuid));
}

/*
 * A real libcurl handle, so the pointer the fetch works with is a genuine,
 * uniquely addressed object rather than a sentinel -- which is what makes the
 * per-handle ownership modelled above, and the option-handoff identity asserted
 * below, mean anything.  Registering it here is what lets every later shim tell
 * "this fetch's live handle" from "a handle that has already been destroyed".
 */
SWITCH_DECLARE(switch_CURL *) fst_xc_curl_easy_init(void)
{
	switch_CURL *handle = (switch_CURL *) curl_easy_init();

	fst_xc_transport.op_seq++;

	if (handle && fst_xc_handle_count < FST_XC_MAX_TRACKED_HANDLES) {
		fst_xc_handles[fst_xc_handle_count].handle = handle;
		fst_xc_handles[fst_xc_handle_count].content_type = NULL;
		fst_xc_handles[fst_xc_handle_count].alive = 1;
		fst_xc_handle_count++;
	}

	if (!fst_xc_transport.init_handle) {
		fst_xc_transport.init_handle = handle;
	}

	return handle;
}

/*
 * Destroys the handle and, with it, everything libcurl would have owned on its
 * behalf.  The Content-Type copy is overwritten before it is released so that a
 * fetch which retained the pointer instead of copying the value reads scrambled
 * bytes at the decode site -- a silent regression becomes a failing assertion,
 * and under the address sanitizer the same read is reported as a use-after-free.
 */
SWITCH_DECLARE(void) fst_xc_curl_easy_cleanup(switch_CURL *handle)
{
	int slot = fst_xc_handle_find(handle);

	fst_xc_transport.op_seq++;

	if (slot >= 0) {
		fst_xc_handles[slot].alive = 0;
		fst_xc_transport.cleanup_op = fst_xc_transport.op_seq;

		if (fst_xc_handles[slot].content_type) {
			size_t len = strlen(fst_xc_handles[slot].content_type);

			memset(fst_xc_handles[slot].content_type, '?', len);
			free(fst_xc_handles[slot].content_type);
			fst_xc_handles[slot].content_type = NULL;
			fst_xc_transport.content_type_storage_freed++;
		}
	}

	if (handle) {
		curl_easy_cleanup((CURL *) handle);
	}
}

/*
 * The request itself.  Writes the canned body into the temporary file the fetch
 * already opened, using the name the fetch composed -- so the production reader,
 * the production parser and the production unlink all act on exactly the file
 * they would act on in production.  A second descriptor is used rather than the
 * module's: nothing has been written through the module's descriptor at this
 * point, and closing it later neither truncates nor rewrites the file.
 */
SWITCH_DECLARE(switch_CURLcode) fst_xc_curl_easy_perform(switch_CURL *handle)
{
	int fd = -1;
	switch_ssize_t wrote = 0;

	(void) handle;

	fst_xc_transport.op_seq++;
	fst_xc_transport.perform_op = fst_xc_transport.op_seq;
	fst_xc_transport.perform_count++;

	if (!fst_xc_transport.body) {
		return fst_xc_transport.perform_result;
	}

	switch_snprintf(fst_xc_transport.last_body_path, sizeof(fst_xc_transport.last_body_path), "%s%s%s.tmp.xml", SWITCH_GLOBAL_dirs.temp_dir,
					SWITCH_PATH_SEPARATOR, fst_xc_transport.last_uuid);

	if ((fd = open(fst_xc_transport.last_body_path, O_WRONLY | O_TRUNC | O_NOFOLLOW, S_IRUSR | S_IWUSR)) < 0) {
		fst_xc_transport.body_write_failures++;
		return fst_xc_transport.perform_result;
	}

	wrote = write(fd, fst_xc_transport.body, fst_xc_transport.body_len);
	close(fd);

	if (wrote != (switch_ssize_t) fst_xc_transport.body_len) {
		fst_xc_transport.body_write_failures++;
	}

	return fst_xc_transport.perform_result;
}

/*
 * Serves the two pieces of response metadata the module reads, with libcurl's own
 * lifetime rules rather than an approximation of them.  Both reads must arrive while
 * the handle is alive; each records its ordinal so the case can assert that both
 * preceded the cleanup.  CURLINFO_CONTENT_TYPE hands back a pointer into
 * handle-owned storage that the caller must copy, and NULL when the response carried
 * no such header.  A read on a destroyed -- or never-issued -- handle is refused with
 * an error and counted, because in production that read is a use-after-free.
 *
 * The CURLINFO is decided BEFORE the variadic argument is touched, and each branch
 * retrieves it as exactly the pointer type libcurl documents for that info -- long *
 * for the response code, char ** for the content type.  va_arg has to be invoked
 * with a type compatible with the argument that was actually passed, and long *,
 * char ** and void * are three mutually incompatible types, so pulling the argument
 * out as a void * and casting afterwards is undefined behaviour even on the ABIs
 * where every object pointer happens to share a representation.  An info this shim
 * does not serve is refused without starting a variadic scan at all.
 */
SWITCH_DECLARE(switch_CURLcode) fst_xc_curl_easy_getinfo(switch_CURL *curl, switch_CURLINFO info, ...)
{
	va_list ap;
	long *code_out = NULL;
	char **type_out = NULL;
	int slot = fst_xc_handle_find(curl);
	int op = 0;

	fst_xc_transport.op_seq++;
	op = fst_xc_transport.op_seq;

	if (slot < 0) {
		fst_xc_transport.getinfo_on_unknown_handle++;
		return CURLE_BAD_FUNCTION_ARGUMENT;
	}

	if (!fst_xc_handles[slot].alive) {
		fst_xc_transport.getinfo_after_cleanup++;
		return CURLE_BAD_FUNCTION_ARGUMENT;
	}

	if (info == CURLINFO_RESPONSE_CODE) {
		va_start(ap, info);
		code_out = va_arg(ap, long *);
		va_end(ap);

		fst_xc_transport.response_code_op = op;

		if (code_out) {
			*code_out = fst_xc_transport.response_code;
		}

		return CURLE_OK;
	}

	if (info == CURLINFO_CONTENT_TYPE) {
		va_start(ap, info);
		type_out = va_arg(ap, char **);
		va_end(ap);

		fst_xc_transport.content_type_op = op;

		if (!type_out) {
			return CURLE_OK;
		}

		if (!fst_xc_transport.content_type_present) {
			*type_out = NULL;

			return CURLE_OK;
		}

		if (!fst_xc_handles[slot].content_type) {
			fst_xc_handles[slot].content_type = strdup(fst_xc_transport.content_type);
		}

		if (!fst_xc_handles[slot].content_type) {
			fst_xc_transport.content_type_alloc_failures++;
			*type_out = NULL;

			return CURLE_OK;
		}

		/*
		 * Recorded as a flag rather than as the pointer itself: the storage is
		 * released at cleanup, and a retained pointer is exactly the defect this
		 * model exists to expose, so the harness must not retain one either.
		 */
		fst_xc_transport.content_type_served_from_handle = 1;
		*type_out = fst_xc_handles[slot].content_type;

		return CURLE_OK;
	}

	return CURLE_BAD_FUNCTION_ARGUMENT;
}

/*
 * Records a header list as it is released.  That is the last moment at which the
 * list is exactly what was handed to CURLOPT_HTTPHEADER, so the Accept
 * negotiation can be asserted from what the request carried.
 */
SWITCH_DECLARE(void) fst_xc_curl_slist_free_all(switch_curl_slist_t *list)
{
	struct curl_slist *node = NULL;
	int bucket = fst_xc_transport.released_list_count;

	fst_xc_transport.op_seq++;
	fst_xc_transport.released_list_count++;

	if (bucket < 0 || bucket >= FST_XC_HEADER_LISTS) {
		curl_slist_free_all(list);
		return;
	}

	fst_xc_transport.released_list[bucket] = (const void *) list;

	for (node = list; node; node = node->next) {
		if (node->data && fst_xc_transport.sent_header_count[bucket] < FST_XC_MAX_SENT_HEADERS) {
			switch_copy_string(fst_xc_transport.sent_headers[bucket][fst_xc_transport.sent_header_count[bucket]], node->data,
							   sizeof(fst_xc_transport.sent_headers[bucket][0]));
			fst_xc_transport.sent_header_count[bucket]++;
		}
	}

	curl_slist_free_all(list);
}

/*
 * Recording a header list as it is released says what the fetch BUILT, not what it
 * GAVE CURL, and those are different claims: a list carrying a correct Accept entry
 * but never passed to CURLOPT_HTTPHEADER is not sent, and a fetch that suppresses the
 * 100-continue builds two lists and hands curl only the second.
 *
 * switch_curl_easy_setopt cannot be redirected the way the other five names are.
 * <switch_curl.h> makes it an object-like alias for curl_easy_setopt, and
 * <curl/curl.h> then makes curl_easy_setopt itself a function-like macro on every
 * supported toolchain, so defining a macro of either name ahead of those headers is a
 * redefinition the build refuses.  The seam is therefore taken one level lower and
 * after the fact: the module source above has already been compiled, so undefining
 * the macro here cannot affect it, and every call it made resolves to the ordinary
 * external function defined below.  That definition inherits the build's
 * -fvisibility=hidden, so it binds this translation unit's call sites and cannot
 * preempt libcurl for anyone else.  No transfer is ever performed and the production
 * code discards every setopt return value, so the shim records and returns success.
 *
 * The option is decided BEFORE the variadic argument is touched, and the argument is
 * retrieved as exactly the type the caller passed: the production call site hands
 * CURLOPT_HTTPHEADER a switch_curl_slist_t *, which <switch_curl.h> defines as struct
 * curl_slist *, so that is the type named here.  struct curl_slist * and void * are
 * incompatible types, so pulling the argument out as a void * and relying on the two
 * sharing a representation would be undefined behaviour even on the ABIs where they
 * do.  The value is widened to the generic const void * observation storage only
 * AFTER it has been read under its own type, so it stays comparable with the address
 * the free-all shim records.  An option this shim does not observe is returned on
 * without starting a variadic scan at all.
 */
#undef curl_easy_setopt

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...)
{
	va_list ap;
	int op = 0;

	fst_xc_transport.op_seq++;
	op = fst_xc_transport.op_seq;
	fst_xc_transport.setopt_count++;

	if (option == CURLOPT_HTTPHEADER) {
		switch_curl_slist_t *headers = NULL;
		const void *list = NULL;

		va_start(ap, option);
		headers = va_arg(ap, switch_curl_slist_t *);
		va_end(ap);

		list = (const void *) headers;

		if (fst_xc_transport.httpheader_set_count < FST_XC_MAX_HTTPHEADER_SETS) {
			fst_xc_transport.httpheader_list[fst_xc_transport.httpheader_set_count] = list;
			fst_xc_transport.httpheader_op[fst_xc_transport.httpheader_set_count] = op;
		}

		fst_xc_transport.httpheader_set_count++;
		fst_xc_transport.last_httpheader_list = list;
		fst_xc_transport.last_httpheader_op = op;
		fst_xc_transport.httpheader_handle = (switch_CURL *) handle;
	}

	return CURLE_OK;
}

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
	} else {
		/*
		 * One side serialised and the other did not, so this call fails -- and it must
		 * fail owning nothing.  Every caller reaches this helper through
		 * fst_requires(), which abandons the rest of the case on failure and so never
		 * reaches the switch_safe_free() pair that would have released a serialisation
		 * handed back alongside that failure.  Releasing it here is what makes failure
		 * mean exactly "no output", which is the only postcondition a caller that is
		 * about to be abandoned can rely on.
		 */
		switch_safe_free(*from_json);
		switch_safe_free(*from_xml);
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
 * How many times `needle` occurs in `haystack`, counting non-overlapping
 * occurrences.
 *
 * A parity case that only compared two serialisations would still pass if both
 * twins lost the construct the pair was written for, so the cases that stand on a
 * width count the elements in the output rather than trusting the comparison to
 * notice.  Returns 0 for an empty needle, so a caller's mistake cannot read as a
 * satisfied count.
 */
static int fst_xc_count_occurrences(const char *haystack, const char *needle)
{
	const char *at = haystack;
	switch_size_t len;
	int count = 0;

	if (zstr(haystack) || zstr(needle)) {
		return 0;
	}

	len = strlen(needle);

	while ((at = strstr(at, needle))) {
		count++;
		at += len;
	}

	return count;
}

/*
 * True when every member of a BadgerFish object is an attribute: a "@"-prefixed
 * member carrying a string.  Such an element has no text member and no child
 * element, which is the construct an attribute-only fixture exists to cover, and
 * which a serialisation comparison alone cannot distinguish from an element that
 * merely happens to look similar.
 *
 * An object with no members at all is not attribute-only - it is the empty
 * element, a different construct with its own pair - so it answers 0.
 */
static int fst_xc_json_attribute_only(const cJSON *node)
{
	const cJSON *member = NULL;

	if (!node || !cJSON_IsObject(node) || !node->child) {
		return 0;
	}

	for (member = node->child; member; member = member->next) {
		if (!member->string || member->string[0] != '@' || !cJSON_IsString(member)) {
			return 0;
		}
	}

	return 1;
}

/*
 * Write `len` bytes to `path`, replacing anything already there.  Returns 1 on
 * success.  Both xml_curl_json_read_file() and xml_url_fetch() take a file name
 * rather than a buffer, because the production fetch streams the HTTP response
 * into a temporary file before anything decodes it.
 *
 * `path` always names an entry inside the private per-case directory.  Any
 * previous copy is removed and the file is then created exclusively, so the
 * descriptor can only ever be a regular file this call created: O_EXCL rules out
 * writing through an entry that already existed, and O_NOFOLLOW refuses a symlink
 * outright rather than following it.  Because the enclosing directory is
 * owner-only and unpredictably named, no other user can win the gap between the
 * removal and the creation.
 */
static int fst_xc_write_file(const char *path, const char *text, switch_size_t len)
{
	switch_ssize_t wrote;
	int fd = -1;

	unlink(path);

	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)) < 0) {
		return 0;
	}

	wrote = write(fd, text, len);
	close(fd);

	return (wrote == (switch_ssize_t) len) ? 1 : 0;
}

/*
 * -------------------------------------------------------------------------
 * COUNTING PRODUCTION LOG LINES DETERMINISTICALLY
 * -------------------------------------------------------------------------
 * "Falls back with a warning" is only an assertion if the warning is counted, and
 * counting it needs care: switch_log_printf() does not call bound loggers, it
 * enqueues onto one FIFO queue that a single dispatcher thread drains while
 * holding the bind lock across every callback.  A line is therefore visible to a
 * logger some time after it was written, and a naive "did anything arrive
 * recently" test can be satisfied by a line an earlier case produced.
 *
 * The queue's own properties give an exact barrier instead.  Emitting a line that
 * cannot be confused with anything else and waiting until this logger has seen it
 * proves that every line enqueued before it has already been dispatched, because
 * the queue is FIFO and one thread drains it.  Two barriers -- one before the
 * counters are armed and one before they are read -- make the counts exact rather
 * than probable.  The wait is bounded and reports a failure instead of blocking;
 * that timeout is the only use of the clock, and it can only turn a hang into a
 * diagnosable failure.
 */
#define FST_XC_LOG_PATTERNS 3
#define FST_XC_LOG_TIMEOUT_US 10000000

/* The one JSON fallback warning, emitted by xml_curl_json_decode_response_ex(). */
#define FST_XC_LOG_FALLBACK "falling back to XML parsing"
#define FST_XC_LOG_IDX_FALLBACK 0

/* The error the production XML parse reports when it cannot parse. */
#define FST_XC_LOG_PARSE_ERROR "Error Parsing Result!"
#define FST_XC_LOG_IDX_PARSE_ERROR 1

/*
 * A third slot the caller fills in, used to assert the ABSENCE of a string from
 * everything the module logs.  The confidentiality cases need that direction:
 * proving a secret is redacted means proving no log line anywhere carried it, and
 * a count of zero over every dispatched line is the only assertion that says so.
 * Held in a buffer rather than as a borrowed pointer because the logger callback
 * runs on the core's log thread and must not depend on a caller's stack.
 */
#define FST_XC_LOG_IDX_ABSENT 2

static switch_memory_pool_t *fst_xc_log_pool = NULL;
static switch_mutex_t *fst_xc_log_mutex = NULL;
static switch_thread_cond_t *fst_xc_log_cond = NULL;
static const char *fst_xc_log_patterns[FST_XC_LOG_PATTERNS];
static int fst_xc_log_counts[FST_XC_LOG_PATTERNS];
static int fst_xc_log_armed = 0;
static int fst_xc_log_bound = 0;
static int fst_xc_log_barrier_seq = 0;
static int fst_xc_log_sentinel_seen = 0;
static char fst_xc_log_sentinel[64] = "";
static char fst_xc_log_absent[256] = "";

static switch_status_t fst_xc_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	(void) level;

	if (!fst_xc_log_mutex || !node || !node->content) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(fst_xc_log_mutex);

	if (*fst_xc_log_sentinel && strstr(node->content, fst_xc_log_sentinel)) {
		/* the barrier line itself, which is deliberately never counted */
		fst_xc_log_sentinel_seen = 1;
		switch_thread_cond_broadcast(fst_xc_log_cond);
	} else if (fst_xc_log_armed) {
		int i;

		for (i = 0; i < FST_XC_LOG_PATTERNS; i++) {
			if (fst_xc_log_patterns[i] && strstr(node->content, fst_xc_log_patterns[i])) {
				fst_xc_log_counts[i]++;
			}
		}
	}

	switch_mutex_unlock(fst_xc_log_mutex);

	return SWITCH_STATUS_SUCCESS;
}

/*
 * Bind the counting logger.  The mutex and condition are allocated from a pool
 * this capture owns rather than from fst_pool, because FST destroys the per-test
 * pool BEFORE the teardown body runs -- a logger still bound at that moment would
 * be waiting on freed memory.
 */
static int fst_xc_log_capture_start(void)
{
	memset(fst_xc_log_counts, 0, sizeof(fst_xc_log_counts));
	memset(fst_xc_log_patterns, 0, sizeof(fst_xc_log_patterns));
	fst_xc_log_armed = 0;
	fst_xc_log_sentinel_seen = 0;
	*fst_xc_log_sentinel = '\0';
	*fst_xc_log_absent = '\0';

	if (fst_xc_log_bound) {
		return 1;
	}

	if (switch_core_new_memory_pool(&fst_xc_log_pool) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	if (switch_mutex_init(&fst_xc_log_mutex, SWITCH_MUTEX_UNNESTED, fst_xc_log_pool) != SWITCH_STATUS_SUCCESS ||
		switch_thread_cond_create(&fst_xc_log_cond, fst_xc_log_pool) != SWITCH_STATUS_SUCCESS) {
		fst_xc_log_mutex = NULL;
		fst_xc_log_cond = NULL;
		switch_core_destroy_memory_pool(&fst_xc_log_pool);
		return 0;
	}

	if (switch_log_bind_logger(fst_xc_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		fst_xc_log_mutex = NULL;
		fst_xc_log_cond = NULL;
		switch_core_destroy_memory_pool(&fst_xc_log_pool);
		return 0;
	}

	fst_xc_log_bound = 1;

	return 1;
}

/*
 * Unbind and release, in that order and idempotently.  switch_log_unbind_logger()
 * takes the same lock the dispatcher holds across every callback, so once it has
 * returned SUCCESS no dispatch can still be inside fst_xc_logger() and the pool
 * backing the mutex and condition is safe to destroy.
 *
 * The pool is therefore destroyed ONLY after a successful unbind: if the unbind did
 * not remove this logger, a dispatch on the core's log thread can still be about to
 * enter fst_xc_logger(), and freeing the mutex it locks would be a use-after-free in
 * another thread.  On failure nothing is released and nothing is cleared -
 * fst_xc_log_bound stays set so the next call retries the unbind, the mutex and
 * condition stay valid so a late dispatch remains safe, and the failure is reported
 * and latched.  Returns 1 only when the logger is demonstrably unbound and its pool
 * released.
 */
static int fst_xc_log_capture_stop(void)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!fst_xc_log_bound) {
		return 1;
	}

	if ((status = switch_log_unbind_logger(fst_xc_logger)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "test teardown could not unbind the counting logger (status %d); its pool is deliberately not released\n", (int) status);
		fst_xc_teardown_failed = 1;
		return 0;
	}

	fst_xc_log_bound = 0;
	fst_xc_log_armed = 0;
	fst_xc_log_mutex = NULL;
	fst_xc_log_cond = NULL;

	if (fst_xc_log_pool) {
		switch_core_destroy_memory_pool(&fst_xc_log_pool);
	}

	return 1;
}

/* Emit a barrier line and wait until this logger has seen it.  Returns 1 when the
   queue has demonstrably drained past it. */
static int fst_xc_log_barrier(void)
{
	switch_time_t deadline = 0;
	int seen = 0;

	if (!fst_xc_log_bound || !fst_xc_log_mutex) {
		return 0;
	}

	switch_mutex_lock(fst_xc_log_mutex);
	switch_snprintf(fst_xc_log_sentinel, sizeof(fst_xc_log_sentinel), "fst-xc-log-barrier-%d", ++fst_xc_log_barrier_seq);
	fst_xc_log_sentinel_seen = 0;
	switch_mutex_unlock(fst_xc_log_mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s\n", fst_xc_log_sentinel);

	deadline = switch_micro_time_now() + FST_XC_LOG_TIMEOUT_US;

	switch_mutex_lock(fst_xc_log_mutex);
	while (!fst_xc_log_sentinel_seen && switch_micro_time_now() < deadline) {
		switch_thread_cond_timedwait(fst_xc_log_cond, fst_xc_log_mutex, 50000);
	}
	seen = fst_xc_log_sentinel_seen;
	*fst_xc_log_sentinel = '\0';
	switch_mutex_unlock(fst_xc_log_mutex);

	return seen;
}

/*
 * Name the string whose ABSENCE the next armed run must demonstrate, or clear the
 * slot with NULL.  Set before fst_xc_log_arm(), which is what installs it.
 */
static void fst_xc_log_watch_absent(const char *needle)
{
	if (!fst_xc_log_mutex) {
		return;
	}

	switch_mutex_lock(fst_xc_log_mutex);
	if (zstr(needle)) {
		*fst_xc_log_absent = '\0';
	} else {
		switch_copy_string(fst_xc_log_absent, needle, sizeof(fst_xc_log_absent));
	}
	switch_mutex_unlock(fst_xc_log_mutex);
}

/* Start counting the two production messages -- and the watched string, when one
   has been named -- from zero. */
static void fst_xc_log_arm(void)
{
	if (!fst_xc_log_mutex) {
		return;
	}

	switch_mutex_lock(fst_xc_log_mutex);
	memset(fst_xc_log_counts, 0, sizeof(fst_xc_log_counts));
	fst_xc_log_patterns[FST_XC_LOG_IDX_FALLBACK] = FST_XC_LOG_FALLBACK;
	fst_xc_log_patterns[FST_XC_LOG_IDX_PARSE_ERROR] = FST_XC_LOG_PARSE_ERROR;
	fst_xc_log_patterns[FST_XC_LOG_IDX_ABSENT] = *fst_xc_log_absent ? fst_xc_log_absent : NULL;
	fst_xc_log_armed = 1;
	switch_mutex_unlock(fst_xc_log_mutex);
}

static void fst_xc_log_disarm(void)
{
	if (!fst_xc_log_mutex) {
		return;
	}

	switch_mutex_lock(fst_xc_log_mutex);
	fst_xc_log_armed = 0;
	switch_mutex_unlock(fst_xc_log_mutex);
}

static int fst_xc_log_count(int index)
{
	int count = 0;

	if (!fst_xc_log_mutex || index < 0 || index >= FST_XC_LOG_PATTERNS) {
		return -1;
	}

	switch_mutex_lock(fst_xc_log_mutex);
	count = fst_xc_log_counts[index];
	switch_mutex_unlock(fst_xc_log_mutex);

	return count;
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
 * The xml_curl.conf injected for the binding-notice confidentiality case.
 *
 * One binding, and a gateway URL that plants the same token in all three places a
 * real provisioning URL carries a secret: the userinfo password, a path segment and
 * a query value.  A single token in three positions is deliberately stronger than
 * three separate ones, because one absence assertion then covers every position at
 * once, and any one of them reaching a log line trips it.
 *
 * The authority is a loopback address and a port nothing listens on.  do_config()
 * only records the URL and registers the binding, so nothing is ever contacted; the
 * distinctive port is there to be recognised in the redacted rendering, which is
 * what proves the notice still identifies the gateway it registered.
 */
static const char fst_xc_notice_conf[] =
	"<document type=\"freeswitch/xml\">"
	"<section name=\"configuration\">"
	"<configuration name=\"xml_curl.conf\" description=\"cURL XML Gateway\">"
	"<bindings>"
	"<binding name=\"notice_binding\">"
	"<param name=\"gateway-url\" value=\"http://provisioner:n0t1ce-canary@127.0.0.1:19011/tenants/n0t1ce-canary/directory"
	"?apikey=n0t1ce-canary\" bindings=\"directory\"/>"
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

/*
 * -------------------------------------------------------------------------
 * MODULE-WIDE STATE, AND THE ONE PLACE IT IS GIVEN BACK
 * -------------------------------------------------------------------------
 * do_config() allocates every binding and duplicates every string member into
 * globals.pool, so the one case that drives it has to lend the module a pool.  It
 * deliberately does NOT lend it fst_pool.  FST destroys the per-test pool at the
 * TOP of the teardown macro, before the teardown body runs, and a failed
 * assertion inside a test body goes straight to teardown -- so a case that had
 * already registered bindings out of fst_pool would reach the teardown body with
 * every binding pointing into freed memory, and the unbinding itself would be
 * reading it.
 *
 * A pool this suite owns removes the ordering problem entirely: it is created
 * when the module is lent state and destroyed only after the bindings that live
 * in it have been removed from the core.  Everything the case mutates is released
 * through this one function, it is idempotent, and it is called both at the end
 * of the case and from the teardown block, so no assertion outcome can leave
 * module state behind.
 */
static switch_memory_pool_t *fst_xc_module_pool = NULL;
static char *fst_xc_module_conf = NULL;

static int fst_xc_module_pool_create(void)
{
	if (fst_xc_module_pool) {
		return 1;
	}

	if (switch_core_new_memory_pool(&fst_xc_module_pool) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	globals.pool = fst_xc_module_pool;

	return 1;
}

static void fst_xc_module_state_cleanup(void)
{
	/* the injected configuration provider, if this case got as far as registering it */
	switch_xml_unbind_search_function_ptr(fst_xc_conf_search);

	/*
	 * The module's own shutdown releases the enable-post-var hashes the parsed
	 * configuration created and removes the fetch bindings do_config() registered.
	 * It is safe unconditionally: with nothing configured the hash list is empty
	 * and the unbind reports SWITCH_STATUS_FALSE harmlessly.
	 */
	mod_xml_curl_shutdown();

	globals.pool = NULL;

	/* only now, with no binding left pointing into it */
	if (fst_xc_module_pool) {
		switch_core_destroy_memory_pool(&fst_xc_module_pool);
	}

	switch_safe_free(fst_xc_module_conf);
}

/*
 * -------------------------------------------------------------------------
 * DRIVING ONE COMPLETE FETCH
 * -------------------------------------------------------------------------
 * Every HTTP sub-case follows the same protocol, and the order inside it is
 * load-bearing, so it lives in exactly one place:
 *
 *   1. drain the log queue, so nothing an earlier sub-case emitted can be
 *      counted against this one;
 *   2. arm the pattern counters;
 *   3. arm the canned response and run the real xml_url_fetch();
 *   4. drain the log queue again, so everything THIS fetch emitted has certainly
 *      been dispatched before the counters are read;
 *   5. disarm, so nothing emitted afterwards is counted either.
 *
 * The drain is a barrier, not a sleep.  The core has one log queue drained by one
 * thread, so a sentinel line observed by the bound logger proves every line
 * enqueued before it has already been delivered to every bound logger.  The only
 * clock reading anywhere in the protocol is the bounded timeout that turns a
 * wedged log thread into a reported failure instead of a hung test.
 */
static int fst_xc_fetch_barrier_ok = 0;

/*
 * The same protocol with the lookup's key_value supplied by the caller.  The
 * confidentiality cases need that: key_value is the last field of the POST form
 * body the module builds, so planting a value that appears nowhere else in the
 * process is the only way to prove by observation that the body itself never
 * reaches a log line.  Every other case wants the fixed identity below, so the
 * original signature is kept as a wrapper rather than changed at 30 call sites.
 */
static switch_xml_t fst_xc_drive_fetch_ex(xml_binding_t *binding, const char *section, const char *key_value, const char *body,
										  const char *content_type, long response_code)
{
	switch_xml_t xml = NULL;

	fst_xc_fetch_barrier_ok = fst_xc_log_barrier();
	fst_xc_log_arm();
	fst_xc_transport_arm(body, content_type, response_code);

	xml = xml_url_fetch(section, "user", "id", key_value, NULL, binding);

	if (!fst_xc_log_barrier()) {
		fst_xc_fetch_barrier_ok = 0;
	}

	fst_xc_log_disarm();

	return xml;
}

static switch_xml_t fst_xc_drive_fetch(xml_binding_t *binding, const char *section, const char *body, const char *content_type, long response_code)
{
	return fst_xc_drive_fetch_ex(binding, section, "1000", body, content_type, response_code);
}

/*
 * The id attribute of the single <user> a fetched provisioning document carries,
 * or the empty string when the document is not shaped that way.  The two response
 * bodies the HTTP case uses name different ids, so this is what distinguishes the
 * tree the BadgerFish translator built from the tree switch_xml_parse_file()
 * built -- which is the only way to prove which decoder actually ran.
 */
static const char *fst_xc_fetched_user_id(switch_xml_t xml)
{
	switch_xml_t section_tag = NULL;
	switch_xml_t user_tag = NULL;

	if (!xml || !(section_tag = switch_xml_child(xml, "section")) || !(user_tag = switch_xml_child(section_tag, "user"))) {
		return "";
	}

	return switch_xml_attr_soft(user_tag, "id");
}

/*
 * Remove the preprocessed copy switch_xml_parse_file() leaves in the log
 * directory.  When the parse succeeds the returned tree owns that path and
 * switch_xml_free() unlinks it; when it fails the production code frees the name
 * without unlinking the file, so the sub-case that drives a failing parse tidies
 * up after it rather than leaving residue behind.  The name is derived from the
 * temporary file the fetch composed, which is why the uuid is recorded.
 */
static void fst_xc_unlink_preprocessed(void)
{
	char path[1024] = "";

	if (!*fst_xc_transport.last_uuid) {
		return;
	}

	switch_snprintf(path, sizeof(path), "%s%s%s.tmp.xml.fsxml", SWITCH_GLOBAL_dirs.log_dir, SWITCH_PATH_SEPARATOR, fst_xc_transport.last_uuid);
	unlink(path);
}

/*
 * -------------------------------------------------------------------------
 * COUNTING WHAT ONE CALL LOGS
 * -------------------------------------------------------------------------
 * The two confidentiality cases at the tail of the suite do not drive a fetch, so
 * they cannot use the protocol above -- but the ordering that protocol depends on is
 * the same, and just as load-bearing: drain, watch, arm, act, drain again, read,
 * disarm.  One wrapper per subject keeps that order in a single place instead of
 * repeating it at every sub-case, and keeps each sub-case down to the two lines that
 * say what it is actually asserting.
 *
 * Both return how many log lines the call emitted that contain the needle, or -1 when
 * a barrier did not complete -- which the caller reports, rather than reading counters
 * the log thread may not have finished writing.  The watch is cleared on the way out
 * so that a following window cannot inherit it, and the call's own result is handed
 * back through the out-parameter so that "it logged nothing" and "it did nothing" can
 * never be confused for one another.
 */
static int fst_xc_count_config_log(const char *needle, switch_status_t *status)
{
	int hits = -1;

	if (!fst_xc_log_barrier()) {
		*status = SWITCH_STATUS_FALSE;
		return -1;
	}

	fst_xc_log_watch_absent(needle);
	fst_xc_log_arm();

	*status = do_config();

	if (fst_xc_log_barrier()) {
		hits = fst_xc_log_count(FST_XC_LOG_IDX_ABSENT);
	}

	fst_xc_log_disarm();
	fst_xc_log_watch_absent(NULL);

	return hits;
}

static int fst_xc_count_jar_log(const char *needle, const char *path, const char *url, int *accepted)
{
	int hits = -1;

	if (!fst_xc_log_barrier()) {
		*accepted = -1;
		return -1;
	}

	fst_xc_log_watch_absent(needle);
	fst_xc_log_arm();

	*accepted = xml_curl_cookie_jar_is_usable(path, url);

	if (fst_xc_log_barrier()) {
		hits = fst_xc_log_count(FST_XC_LOG_IDX_ABSENT);
	}

	fst_xc_log_disarm();
	fst_xc_log_watch_absent(NULL);

	return hits;
}

/*
 * -------------------------------------------------------------------------
 * THE STANDALONE CONTRACT VALIDATOR
 * -------------------------------------------------------------------------
 * tools/validate_badgerfish.py is the repo-side deliverable a backend team runs
 * against its own gateway's JSON output, before deployment, to prove the output
 * conforms to the profile this module accepts.  It is a python3 script using
 * only the standard library: no FreeSWITCH build, no core, no network.
 *
 * The helpers below let the last case in this suite exercise that script as a
 * black box - the same way the backend team will - and assert its exit status.
 * Nothing here touches module state, and nothing here is used by any other
 * case.
 */

/* The validator, resolved from the same compile-time base directory the fixture
   macro above uses, so the case is independent of the directory the binary is
   started from. */
#define FST_XC_VAL_TOOL SWITCH_TEST_BASE_DIR_OVERRIDE SWITCH_PATH_SEPARATOR "tools" SWITCH_PATH_SEPARATOR "validate_badgerfish.py"

/* The interpreter the script declares in its shebang.  Named explicitly rather
   than relying on the execute bit, so the case fails with a clear diagnostic on
   a host without python3 instead of on a mount without exec permission. */
#define FST_XC_VAL_PYTHON "python3"

/*
 * The floor for the conformant half of the corpus: every parity fixture this
 * suite ships, all eighteen of them.
 *
 * It is a MINIMUM rather than an exact count so that a nineteenth pair can be
 * added without editing this case - the sweep below runs the validator over
 * whatever is present and requires a clean verdict across all of it.  It is not
 * a smaller number, because a floor below the shipped corpus is a floor that
 * nothing has to satisfy: at nine, deleting every pair added after the original
 * nine would leave this case passing while the contract the validator exists to
 * police went uncovered.  The named list below closes the same hole from the
 * other side, since a count alone cannot say WHICH eighteen were run.
 */
#define FST_XC_VAL_MIN_PARITY_JSON 18

/* Stems of the conformant corpus: one prefix per bound provisioning section.
   A fixture outside these three prefixes - the deliberately non-conformant
   badgerfish_invalid_* samples - is never swept into the conformant run. */
static const char *fst_xc_val_section_prefixes[] = { "configuration_", "directory_", "dialplan_" };

/*
 * The conformant fixtures the validator MUST be run over, by name: the original
 * nine pairs and the nine that widened the corpus, each of which also has a
 * parity case of its own above.  Naming them is what makes the requirement an
 * identity rather than a headcount - eighteen unrelated files would satisfy a
 * count, and would prove nothing about the pair whose construct went missing.
 *
 * Additive by construction: the sweep below marks these off as it walks the
 * directory and separately runs everything it finds, so a new pair needs a new
 * line here only when it is meant to be mandatory.
 */
static const char *fst_xc_val_required_parity_json[] = {
	"configuration_modules.json",
	"configuration_acl.json",
	"configuration_event_socket.json",
	"directory_user_simple.json",
	"directory_user_params.json",
	"directory_domain_multi_user.json",
	"dialplan_single_extension.json",
	"dialplan_multi_condition.json",
	"dialplan_multi_extension.json",
	"configuration_load_width_ceiling.json",
	"configuration_attribute_only.json",
	"configuration_unicode_escapes.json",
	"directory_mixed_text_and_siblings.json",
	"directory_nesting_boundary.json",
	"directory_empty_elements.json",
	"dialplan_nested_arrays.json",
	"dialplan_attribute_only_unicode.json",
	"dialplan_empty_and_mixed.json"
};

/* Prefix of the committed non-conformant samples, each of which must be
   refused. */
#define FST_XC_VAL_INVALID_PREFIX "badgerfish_invalid_"

/*
 * The five counter-examples the contract enumerates, by name: an emitted
 * <document>/<section> envelope, more than one top-level key, a non-string leaf
 * value, an attribute directly under the root key, and a top-level key that is
 * not a bound section name.  Each is run on its own below and each must be
 * refused on its own.
 *
 * Named rather than matched by prefix because a prefix and a count cannot tell
 * "all five are still detected" from "one was deleted and an unrelated sixth
 * was added": the count would hold and the missing rule would stop being
 * covered.  Samples beyond these five are still swept afterwards, so the list
 * is a floor and not a ceiling.
 */
static const char *fst_xc_val_required_invalid_json[] = {
	"badgerfish_invalid_envelope_attributes.json",
	"badgerfish_invalid_multiple_top_level_keys.json",
	"badgerfish_invalid_non_string_leaf.json",
	"badgerfish_invalid_root_attribute.json",
	"badgerfish_invalid_unbound_section.json"
};

/*
 * Run one shell command and return the exit code the child actually reported.
 *
 * system() is what the command is run through, and its return value is NOT an
 * exit code: it is a wait status in the same encoding waitpid() produces, so it
 * has to be decoded rather than compared.  WIFEXITED() and WEXITSTATUS() are
 * the decoders for it, and both are already in scope here - <stdlib.h>, which
 * <switch.h> includes and which declares system() itself, is where they come
 * from - so no additional header is needed.  On Windows there is no wait status
 * to decode and system() returns the child's exit code directly, which the
 * fallback arm reflects.
 *
 * Returns -1, which no exit code of the validator can collide with, when the
 * shell could not be started at all or when the child did not exit normally.
 * The two are distinguished by the caller's diagnostic, not conflated with a
 * conformance verdict.
 */
static int fst_xc_val_run(const char *command)
{
	int status;

	if (!command) {
		return -1;
	}

	status = system(command);

	if (status == -1) {
		/* fork() or the shell itself failed; nothing ran */
		return -1;
	}

#ifdef WIFEXITED
	if (!WIFEXITED(status)) {
		/* killed by a signal, or stopped: not an exit code at all */
		return -1;
	}

	return WEXITSTATUS(status);
#else
	return status;
#endif
}

/*
 * Append one word to a command line, quoted so the shell can only ever read it
 * as that one word.
 *
 * This is the ONLY way a word reaches any command this case builds - the
 * interpreter, the validator's own path and every argument alike.  Embedding a
 * path directly between two literal quotes is what makes a checkout whose
 * directory contains an apostrophe silently run a different command, and the
 * paths here come from a compile-time base directory nobody in this file
 * chooses, so "our paths are fine" is an assumption rather than a property.
 *
 * The quoting is the POSIX one: wrap in single quotes, and render each embedded
 * single quote by closing the quoted run, emitting an escaped quote and opening
 * a new one ('\'').  Inside single quotes the shell interprets nothing else, so
 * no other character needs handling.  Returns 0 - which every caller turns into
 * a failed assertion rather than a mangled command - for an empty word or one
 * that would not fit the buffer.
 */
static int fst_xc_val_append_word(switch_stream_handle_t *stream, const char *word)
{
	/* Twice the 1024-byte path buffer the case builds its arguments in, which is
	   the worst case only a word made entirely of single quotes could approach,
	   plus room for the two delimiters and the terminator. */
	char quoted[2 * 1024 + 8];
	switch_size_t out = 0;
	switch_size_t i;

	if (!stream || zstr(word)) {
		return 0;
	}

	quoted[out++] = '\'';

	for (i = 0; word[i]; i++) {
		/* Worst case for one input byte is the four-byte '\'' sequence, plus the
		   closing quote and the terminator this loop has to leave room for. */
		if (out + 6 >= sizeof(quoted)) {
			return 0;
		}

		if (word[i] == '\'') {
			quoted[out++] = '\'';
			quoted[out++] = '\\';
			quoted[out++] = '\'';
			quoted[out++] = '\'';
		} else {
			quoted[out++] = word[i];
		}
	}

	quoted[out++] = '\'';
	quoted[out] = '\0';

	stream->write_function(stream, " %s", quoted);

	return 1;
}

/*
 * Begin a validator command: the interpreter and the tool, both quoted by the
 * helper above rather than by literal quotes in a format string, so the command
 * this case runs is the command it means on any checkout path.
 */
static int fst_xc_val_begin_command(switch_stream_handle_t *stream)
{
	if (!stream) {
		return 0;
	}

	if (!fst_xc_val_append_word(stream, FST_XC_VAL_PYTHON)) {
		return 0;
	}

	return fst_xc_val_append_word(stream, FST_XC_VAL_TOOL);
}

/*
 * True when `name` starts with `prefix`.  Spelled out rather than reaching for
 * strncmp() with a computed length at every call site.
 */
static int fst_xc_val_has_prefix(const char *name, const char *prefix)
{
	switch_size_t len;

	if (zstr(name) || zstr(prefix)) {
		return 0;
	}

	len = strlen(prefix);

	return strlen(name) >= len && !strncmp(name, prefix, len);
}

/*
 * True when `name` ends in the JSON extension.  The extension has to BE the
 * ending rather than merely appear somewhere in the name, so a stray
 * "directory_user.json.orig" left in the fixtures directory is not swept in.
 */
static int fst_xc_val_is_json(const char *name)
{
	switch_size_t len;

	if (zstr(name)) {
		return 0;
	}

	len = strlen(name);

	return len > 5 && !strcmp(name + len - 5, ".json");
}

/*
 * True when `name` is either half of a conformant parity PAIR -- the .json twin or
 * the .xml twin.  The deliberately non-conformant badgerfish_invalid_* samples are
 * excluded by the same prefix test that excludes them from the validator sweep, so a
 * caller that wants the shipped contract corpus and nothing else can use this.
 */
static int fst_xc_val_is_corpus_fixture(const char *name)
{
	switch_size_t i;
	switch_size_t len;

	if (zstr(name)) {
		return 0;
	}

	/* the .json half is exactly what the validator sweep accepts; the .xml twin is
	   the other half of the same pair and is tested for here */
	if (!fst_xc_val_is_json(name)) {
		len = strlen(name);

		if (!(len > 4 && !strcmp(name + len - 4, ".xml"))) {
			return 0;
		}
	}

	for (i = 0; i < switch_arraylen(fst_xc_val_section_prefixes); i++) {
		if (fst_xc_val_has_prefix(name, fst_xc_val_section_prefixes[i])) {
			return 1;
		}
	}

	return 0;
}

/*
 * True when `name` is one of the conformant parity fixtures: a .json file whose
 * stem begins with one of the three bound section names.
 */
static int fst_xc_val_is_parity_json(const char *name)
{
	switch_size_t i;

	if (!fst_xc_val_is_json(name)) {
		return 0;
	}

	for (i = 0; i < switch_arraylen(fst_xc_val_section_prefixes); i++) {
		if (fst_xc_val_has_prefix(name, fst_xc_val_section_prefixes[i])) {
			return 1;
		}
	}

	return 0;
}

static int fst_xc_val_is_invalid_sample(const char *name)
{
	return fst_xc_val_is_json(name) && fst_xc_val_has_prefix(name, FST_XC_VAL_INVALID_PREFIX);
}

/*
 * Index of `name` in a required-fixture list, or -1 when it is not a member.
 *
 * The index rather than a yes/no answer, because both callers need to record
 * WHICH member was found: one marks it off a checklist, the other skips a file
 * it has already run.
 */
static int fst_xc_val_index_of(const char *const *list, switch_size_t len, const char *name)
{
	switch_size_t i;

	if (!list || zstr(name)) {
		return -1;
	}

	for (i = 0; i < len; i++) {
		if (list[i] && !strcmp(list[i], name)) {
			return (int) i;
		}
	}

	return -1;
}

/* Returned when a path cannot be passed to a shell as one word, which is a
   harness failure and not a conformance verdict.  No exit code of the validator
   can collide with it. */
#define FST_XC_VAL_UNQUOTABLE (-2)

/*
 * Run the validator over exactly one fixture and return the exit code it
 * reported, FST_XC_VAL_UNQUOTABLE when the path could not be quoted, or -1 when
 * no child ran at all.
 *
 * One run per file is what makes a verdict attributable: a single run over a set
 * exits non-zero as soon as any member is refused, which is the right shape for
 * the conformant sweep - where every member must pass - and the wrong shape for
 * the counter-examples, where each must be refused ON ITS OWN or a validator
 * that had stopped detecting one rule could hide behind another still firing.
 *
 * `quiet` discards stderr as well as stdout.  It is set for the counter-examples
 * because their named violations are the expected outcome and would otherwise
 * fill a passing run's log with alarming text, and clear for fixtures that are
 * meant to pass, so that an unexpected violation names itself.
 */
static int fst_xc_val_run_one(const char *path, int quiet)
{
	switch_stream_handle_t stream = { 0 };
	int code = FST_XC_VAL_UNQUOTABLE;

	SWITCH_STANDARD_STREAM(stream);

	/* The interpreter and the tool path go through the same quoting every argument
	   does, so a repository path carrying an apostrophe cannot break the command
	   this helper hands to the shell. */
	if (fst_xc_val_begin_command(&stream) && fst_xc_val_append_word(&stream, path)) {
		stream.write_function(&stream, "%s", quiet ? " >/dev/null 2>&1" : " >/dev/null");
		code = fst_xc_val_run((const char *) stream.data);
	}

	switch_safe_free(stream.data);

	return code;
}


/*
 * -------------------------------------------------------------------------
 * COUNTING THE FALLBACK EVENTS ONE FETCH FIRES
 * -------------------------------------------------------------------------
 * The fallback is signalled twice from one place: a WARNING for a human and a
 * SWITCH_EVENT_CUSTOM subclass XML_CURL_JSON_FALLBACK_EVENT for a machine.  The
 * log harness above proves the first; this one proves the second, and the two
 * together are what let a case assert that the operator-facing signal and the
 * machine-facing signal pair one for one.
 *
 * Event delivery is asynchronous exactly as log delivery is: switch_event_fire()
 * hands the event to the core's dispatch queue and returns, so a bound consumer
 * sees it some time later and a naive "did anything arrive" test can be satisfied
 * by an event an earlier sub-case produced.  The remedy is the same barrier the
 * log capture uses, but the invariant it rests on has to be stated, because one
 * queue is not by itself enough.  The core owns ONE FIFO event dispatch queue,
 * yet it can ADD dispatch workers once the queued backlog exceeds
 * DISPATCH_QUEUE_LEN per running worker, and with several workers draining that
 * one queue the order in which callbacks COMPLETE is no longer the order they
 * were enqueued in.  This harness is ordered because it starts with a single
 * worker and cannot come near that threshold: it fires a handful of events at a
 * time and every barrier waits for the queue to drain, so the instantaneous
 * backlog stays in single digits against a ceiling of ten thousand, no second
 * worker is ever launched, and callback completion is FIFO here.  Under that
 * stated invariant an event of this very subclass carrying a private barrier
 * header, observed by this consumer, proves every event enqueued before it has
 * already been delivered.  Two barriers -- one before the counters are armed and
 * one before they are read -- then make the counts exact rather than probable,
 * and the wait is bounded so a stalled dispatcher becomes a failure rather than a
 * hang.
 *
 * The barrier event is told apart from a real one by that private header alone,
 * never by its subclass: the production emitter adds exactly three headers and
 * none of them is this one, so an event without it can only have come from the
 * module.  Sharing the subclass is deliberate -- it is what puts the barrier in
 * the same queue as the events it has to order, which is the whole mechanism.
 */
#define FST_XC_EVT_MAX 4
#define FST_XC_EVT_TIMEOUT_US 10000000
#define FST_XC_EVT_BIND_ID "test_mod_xml_curl"
#define FST_XC_EVT_BARRIER_HEADER "Fst-Xc-Evt-Barrier"

/* The three headers the fallback event carries, copied out of each event as it
   arrives.  Copied rather than borrowed because the event is destroyed as soon as
   dispatch returns, and read back through a by-value accessor so a case never
   touches the consumer's state without the lock. */
typedef struct {
	char binding[128];
	char reason[64];
	char gateway[256];
} fst_xc_evt_record_t;

static switch_memory_pool_t *fst_xc_evt_pool = NULL;
static switch_mutex_t *fst_xc_evt_mutex = NULL;
static switch_thread_cond_t *fst_xc_evt_cond = NULL;
static fst_xc_evt_record_t fst_xc_evt_records[FST_XC_EVT_MAX];
static int fst_xc_evt_count = 0;
static int fst_xc_evt_armed = 0;
static int fst_xc_evt_bound = 0;
static int fst_xc_evt_barrier_seq = 0;
static int fst_xc_evt_sentinel_seen = 0;
static char fst_xc_evt_sentinel[64] = "";

static void fst_xc_evt_handler(switch_event_t *event)
{
	const char *barrier = NULL;

	if (!fst_xc_evt_mutex || !event) {
		return;
	}

	switch_mutex_lock(fst_xc_evt_mutex);

	barrier = switch_event_get_header(event, FST_XC_EVT_BARRIER_HEADER);

	if (barrier) {
		/* the barrier event itself, which is deliberately never counted; a stale one
		   from a window that has already been read is ignored outright */
		if (*fst_xc_evt_sentinel && !strcmp(barrier, fst_xc_evt_sentinel)) {
			fst_xc_evt_sentinel_seen = 1;
			switch_thread_cond_broadcast(fst_xc_evt_cond);
		}
	} else if (fst_xc_evt_armed) {
		if (fst_xc_evt_count < FST_XC_EVT_MAX) {
			fst_xc_evt_record_t *record = &fst_xc_evt_records[fst_xc_evt_count];

			switch_copy_string(record->binding, switch_event_get_header_nil(event, "Binding"), sizeof(record->binding));
			switch_copy_string(record->reason, switch_event_get_header_nil(event, "Fallback-Reason"), sizeof(record->reason));
			switch_copy_string(record->gateway, switch_event_get_header_nil(event, "Gateway"), sizeof(record->gateway));
		}

		/* counted even when there is no room to record it, so "one event" can never be
		   satisfied by an overflow that went unnoticed */
		fst_xc_evt_count++;
	}

	switch_mutex_unlock(fst_xc_evt_mutex);
}

/*
 * Bind the counting consumer.  The mutex and condition come from a pool this
 * capture owns rather than from fst_pool, for the reason the log capture documents:
 * FST destroys the per-test pool BEFORE the teardown body runs, and a consumer still
 * bound at that moment would be locking freed memory from the dispatch thread.
 */
static int fst_xc_evt_capture_start(void)
{
	memset(fst_xc_evt_records, 0, sizeof(fst_xc_evt_records));
	fst_xc_evt_count = 0;
	fst_xc_evt_armed = 0;
	fst_xc_evt_sentinel_seen = 0;
	*fst_xc_evt_sentinel = '\0';

	if (fst_xc_evt_bound) {
		return 1;
	}

	if (switch_core_new_memory_pool(&fst_xc_evt_pool) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	if (switch_mutex_init(&fst_xc_evt_mutex, SWITCH_MUTEX_UNNESTED, fst_xc_evt_pool) != SWITCH_STATUS_SUCCESS ||
		switch_thread_cond_create(&fst_xc_evt_cond, fst_xc_evt_pool) != SWITCH_STATUS_SUCCESS) {
		fst_xc_evt_mutex = NULL;
		fst_xc_evt_cond = NULL;
		switch_core_destroy_memory_pool(&fst_xc_evt_pool);
		return 0;
	}

	if (switch_event_bind(FST_XC_EVT_BIND_ID, SWITCH_EVENT_CUSTOM, XML_CURL_JSON_FALLBACK_EVENT, fst_xc_evt_handler,
						  NULL) != SWITCH_STATUS_SUCCESS) {
		fst_xc_evt_mutex = NULL;
		fst_xc_evt_cond = NULL;
		switch_core_destroy_memory_pool(&fst_xc_evt_pool);
		return 0;
	}

	fst_xc_evt_bound = 1;

	return 1;
}

/*
 * Unbind and release, in that order and idempotently, on the same reasoning the log
 * capture spells out: switch_event_unbind_callback() takes the same reader/writer
 * lock the dispatcher holds across every callback, so once it has reported success no
 * dispatch can still be inside fst_xc_evt_handler() and the pool backing the mutex
 * and condition is safe to destroy.  On failure nothing is released, the failure is
 * reported and latched, and a later call retries -- freeing a mutex a live dispatch
 * may still lock would be a use-after-free in another thread.
 */
static int fst_xc_evt_capture_stop(void)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!fst_xc_evt_bound) {
		return 1;
	}

	if ((status = switch_event_unbind_callback(fst_xc_evt_handler)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "test teardown could not unbind the fallback event consumer (status %d); its pool is deliberately not released\n",
						  (int) status);
		fst_xc_teardown_failed = 1;
		return 0;
	}

	fst_xc_evt_bound = 0;
	fst_xc_evt_armed = 0;
	fst_xc_evt_mutex = NULL;
	fst_xc_evt_cond = NULL;

	if (fst_xc_evt_pool) {
		switch_core_destroy_memory_pool(&fst_xc_evt_pool);
	}

	return 1;
}

/* Fire a barrier event and wait until this consumer has seen it.  Returns 1 when the
   dispatch queue has demonstrably drained past it, 0 when it could not be built or the
   bounded wait expired -- which the caller reports rather than reading counters the
   dispatch thread may not have finished writing. */
static int fst_xc_evt_barrier(void)
{
	switch_event_t *event = NULL;
	switch_time_t deadline = 0;
	int seen = 0;

	if (!fst_xc_evt_bound || !fst_xc_evt_mutex) {
		return 0;
	}

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, XML_CURL_JSON_FALLBACK_EVENT) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	switch_mutex_lock(fst_xc_evt_mutex);
	switch_snprintf(fst_xc_evt_sentinel, sizeof(fst_xc_evt_sentinel), "fst-xc-evt-barrier-%d", ++fst_xc_evt_barrier_seq);
	fst_xc_evt_sentinel_seen = 0;
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, FST_XC_EVT_BARRIER_HEADER, fst_xc_evt_sentinel);
	switch_mutex_unlock(fst_xc_evt_mutex);

	switch_event_fire(&event);

	deadline = switch_micro_time_now() + FST_XC_EVT_TIMEOUT_US;

	switch_mutex_lock(fst_xc_evt_mutex);
	while (!fst_xc_evt_sentinel_seen && switch_micro_time_now() < deadline) {
		switch_thread_cond_timedwait(fst_xc_evt_cond, fst_xc_evt_mutex, 50000);
	}
	seen = fst_xc_evt_sentinel_seen;
	*fst_xc_evt_sentinel = '\0';
	switch_mutex_unlock(fst_xc_evt_mutex);

	return seen;
}

static void fst_xc_evt_arm(void)
{
	if (!fst_xc_evt_mutex) {
		return;
	}

	switch_mutex_lock(fst_xc_evt_mutex);
	memset(fst_xc_evt_records, 0, sizeof(fst_xc_evt_records));
	fst_xc_evt_count = 0;
	fst_xc_evt_armed = 1;
	switch_mutex_unlock(fst_xc_evt_mutex);
}

static void fst_xc_evt_disarm(void)
{
	if (!fst_xc_evt_mutex) {
		return;
	}

	switch_mutex_lock(fst_xc_evt_mutex);
	fst_xc_evt_armed = 0;
	switch_mutex_unlock(fst_xc_evt_mutex);
}

/* How many fallback events the armed window captured, or -1 when nothing is bound. */
static int fst_xc_evt_captured(void)
{
	int count = 0;

	if (!fst_xc_evt_mutex) {
		return -1;
	}

	switch_mutex_lock(fst_xc_evt_mutex);
	count = fst_xc_evt_count;
	switch_mutex_unlock(fst_xc_evt_mutex);

	return count;
}

/* One captured event, by value, so a case reads three NUL-terminated strings it owns
   rather than holding the consumer's storage.  An out-of-range index yields an
   all-empty record, which fails a value assertion instead of crashing it. */
static fst_xc_evt_record_t fst_xc_evt_record(int index)
{
	fst_xc_evt_record_t record;

	memset(&record, 0, sizeof(record));

	if (!fst_xc_evt_mutex || index < 0 || index >= FST_XC_EVT_MAX) {
		return record;
	}

	switch_mutex_lock(fst_xc_evt_mutex);
	record = fst_xc_evt_records[index];
	switch_mutex_unlock(fst_xc_evt_mutex);

	return record;
}

/*
 * One complete fetch with BOTH captures armed around it, so a case can assert the
 * WARNING count and the event count against the same fetch.  The order is the log
 * protocol's order with the event barriers wrapped outside it, and it lives here for
 * the same reason that one does: every sub-case needs it identical.
 *
 *   1. drain the event queue, so nothing an earlier sub-case fired is counted here;
 *   2. arm the event counters;
 *   3. run the log protocol and the real xml_url_fetch() through fst_xc_drive_fetch();
 *   4. drain the event queue again, so everything THIS fetch fired has certainly been
 *      delivered before the counters are read;
 *   5. disarm, so nothing fired afterwards is counted either.
 */
static int fst_xc_evt_fetch_barrier_ok = 0;

static switch_xml_t fst_xc_evt_drive_fetch(xml_binding_t *binding, const char *section, const char *body, const char *content_type,
										   long response_code)
{
	switch_xml_t xml = NULL;

	fst_xc_evt_fetch_barrier_ok = fst_xc_evt_barrier();
	fst_xc_evt_arm();

	xml = fst_xc_drive_fetch(binding, section, body, content_type, response_code);

	if (!fst_xc_evt_barrier()) {
		fst_xc_evt_fetch_barrier_ok = 0;
	}

	fst_xc_evt_disarm();

	return xml;
}

FST_CORE_BEGIN("conf")
{
	FST_SUITE_BEGIN(mod_xml_curl)
	{
		FST_SETUP_BEGIN()
		{
			/*
			 * The preceding case's teardown has to have finished before this one is
			 * allowed to start.  Teardown runs outside any assertion scope, so this
			 * is where a latched cleanup failure becomes a reported one; without it
			 * an undeleted file or a logger that would not unbind would leave every
			 * later case running against state it does not own, silently.
			 */
			fst_requires(!fst_xc_teardown_failed);

			/*
			 * One private directory per case for everything written to disk.  The
			 * block is mandatory in any event -- FST_CORE_BEGIN selects the full
			 * core, and FST_TEST_BEGIN then requires both the per-test memory pool
			 * and the soft timer that only this macro creates.
			 */
			fst_requires(fst_xc_temp_dir_create());
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			/*
			 * The only cleanup this suite relies on.  fst_requires() jumps straight to
			 * teardown on failure, so anything a case registers has to be removable from
			 * here as well as from the case itself; every call below is idempotent and
			 * harmless when the case never got that far.
			 *
			 * Order matters: module state is released before the private directory is
			 * removed, because releasing it unbinds fetch functions whose documents live
			 * in that directory.
			 *
			 * Both stops latch fst_xc_teardown_failed, which the next case's setup asserts
			 * and the last declared case asserts cumulatively.  Neither is short-circuited
			 * on the other's failure: a logger that would not unbind must not stop the
			 * private directory being cleaned, or one reported defect would become two.
			 * The directory removal is asserted rather than merely attempted, because the
			 * same case would orphan more residue on every run.
			 */
			fst_xc_log_capture_stop();
			fst_xc_module_state_cleanup();
			switch_xml_unbind_search_function_ptr(xml_url_fetch);
			fst_requires(fst_xc_temp_dir_destroy());
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

		/*
		 * Declared before the first case that writes a document, because it establishes
		 * the guarantee every one of those cases depends on.
		 *
		 * SWITCH_GLOBAL_dirs.temp_dir is shared, and when the core has no configured
		 * override it is the system-wide world-writable directory.  A fixed file name
		 * there would be two distinct defects: two concurrent runs of this binary would
		 * truncate each other's documents, and any local user could pre-create the name
		 * as a symlink and have the suite write through it.  Every document this suite
		 * writes therefore lives inside a per-case directory whose name carries a uuid,
		 * created owner-only, with each file created rather than opened.
		 */
		FST_TEST_BEGIN(temp_documents_are_isolated_and_created)
		{
			char path[1024] = "";
			char canary[1024] = "";
			char buf[64] = "";
			struct stat st;
			switch_ssize_t got = 0;
			int fd = -1;

			/* a real directory, owner-only, and not the shared one */
			fst_requires(*fst_xc_temp_dir != '\0');
			fst_xcheck(strcmp(fst_xc_temp_dir, SWITCH_GLOBAL_dirs.temp_dir) != 0,
					   "the suite must not write straight into the shared temporary directory");
			fst_requires(lstat(fst_xc_temp_dir, &st) == 0);
			fst_xcheck(S_ISDIR(st.st_mode), "the private path must be a directory, not a symlink to one");
			fst_check_int_equals((int) (st.st_mode & 0777), (int) (S_IRUSR | S_IWUSR | S_IXUSR));

			/*
			 * And the directory the previous case used is gone.  Every case gets a fresh
			 * uuid, so "this directory is empty" holds by construction and proves nothing
			 * about cleanup; that the previous one no longer exists is the assertion that
			 * does.
			 */
			fst_requires(*fst_xc_temp_dir_last != '\0');
			fst_xcheck(strcmp(fst_xc_temp_dir_last, fst_xc_temp_dir) != 0, "each case must be given its own private directory");
			fst_xcheck(lstat(fst_xc_temp_dir_last, &st) != 0 && errno == ENOENT,
					   "the directory the previous case used must have been removed by its teardown");
			fst_check_int_equals(fst_xc_temp_dir_failures, 0);
			fst_xcheck(!*fst_xc_temp_dir_residue, "no case may leave residue behind in its private directory");

			/* and it starts empty, so no case can be affected by another case's documents */
			fst_check(lstat(fst_xc_temp_path(FST_XC_TEMP_BODY, path, sizeof(path)), &st) != 0);
			fst_check(lstat(fst_xc_temp_path(FST_XC_TEMP_XML, path, sizeof(path)), &st) != 0);
			fst_check(lstat(fst_xc_temp_path(FST_XC_TEMP_DIRECTORY_XML, path, sizeof(path)), &st) != 0);
			fst_check(lstat(fst_xc_temp_path(FST_XC_TEMP_DIALPLAN_XML, path, sizeof(path)), &st) != 0);

			/*
			 * A symlink planted at one of the names is not written through.  The entry is
			 * removed and a regular file is created in its place, so the target keeps its
			 * contents; O_NOFOLLOW refuses the descriptor outright if the removal were
			 * lost to a race, and O_EXCL refuses an entry that reappeared in the gap.
			 */
			fst_xc_temp_path(FST_XC_TEMP_CANARY, canary, sizeof(canary));
			fst_requires(fst_xc_write_file(canary, "untouched", 9));

			fst_xc_temp_path(FST_XC_TEMP_XML, path, sizeof(path));
			fst_requires(symlink(canary, path) == 0);
			fst_xcheck(fst_xc_write_file(path, "X", 1) == 1, "the document must still be written");

			fst_requires(lstat(path, &st) == 0);
			fst_xcheck(S_ISREG(st.st_mode), "the document must be a regular file, not the symlink that was planted");
			fst_check_int_equals((int) st.st_size, 1);

			fst_requires((fd = open(canary, O_RDONLY, 0)) > -1);
			got = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			fst_check_int_equals((int) got, 9);
			fst_xcheck(!strcmp(buf, "untouched"), "the symlink target must be untouched");
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

			fst_xc_temp_path(FST_XC_TEMP_BODY, path, sizeof(path));

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

		FST_TEST_BEGIN(envelope_ownership_reserved)
		{
			char *rendered = NULL;

			/*
			 * Matching the top-level key against the requested section is necessary but not
			 * sufficient, because the root member is translated INTO the <section> element
			 * the adapter just created.  switch_xml_set_attr() replaces an existing
			 * attribute IN PLACE rather than appending a second one, so a root-level
			 * BadgerFish "@name" would rewrite the section name the adapter had already set
			 * -- after the top-level key had been checked, and with no diagnostic.
			 *
			 * The consequence is not cosmetic.  switch_xml_locate() treats a
			 * <section name="result"> carrying a <result status="not found"> as a deliberate
			 * miss and goes on to satisfy the lookup from static local configuration.  A
			 * response could therefore substitute a silent "not found" for a directory
			 * lookup while the decode reported success: no fallback warning, no XML parse,
			 * and a caller that believes it asked the gateway and was told nothing exists.
			 *
			 * The envelope is core-owned, so every root-level attribute member is refused.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"@name\":\"result\",\"user\":{\"@id\":\"1000\"}}}", "directory"),
					   "a root @name may not rename the section");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"1000\"},\"@name\":\"result\"}}", "directory"),
					   "the position of the root @name in the object must not matter");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"@name\":\"directory\",\"user\":{}}}", "directory"),
					   "a root @name is refused even when it agrees with the requested section");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"@type\":\"freeswitch/xml\",\"user\":{}}}", "directory"),
					   "no root-level attribute member is contractual");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"$\":\"text\",\"@name\":\"result\"}}", "directory"),
					   "a root @name is refused alongside root text as well");

			/*
			 * The refusal is exactly one level deep: "@name" is the ordinary way a
			 * configuration document names itself, and every shipped fixture relies on it
			 * one level below the envelope.
			 */
			fst_xcheck(fst_xc_accepts("{\"configuration\":{\"configuration\":{\"@name\":\"acl.conf\"}}}", "configuration"),
					   "a child @name must still be accepted");
			fst_xcheck(fst_xc_accepts("{\"directory\":{\"user\":{\"@name\":\"result\"}}}", "directory"),
					   "even the sentinel spelling is legitimate below the envelope");

			/* and the envelope a caller receives names the section that was requested */
			rendered = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"1000\"}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "<section name=\"directory\">");
			fst_check_string_does_not_have(rendered, "name=\"result\"");
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

		FST_TEST_BEGIN(serializer_immune_sequence_rejected)
		{
			char *rendered = NULL;

			/*
			 * A name is drawn from a whitelist, but a VALUE only has to be legal XML
			 * character data -- and legal character data is not the same thing as data the
			 * serializer will escape.  switch_xml_ampencode() escapes '<' as &lt; in the
			 * ordinary case, but a '<' whose next byte is '!' is emitted RAW and puts the
			 * encoder into an immune mode that copies every remaining byte verbatim.  A
			 * single value of "<!" is therefore enough to leave the escaping regime for the
			 * rest of the string, so a response could forge attribute and element syntax
			 * with nothing but a quote and a bracket after it.
			 *
			 * The sequence is refused in the one common value validator, which is what
			 * makes the rule hold at every builder call site: an attribute value, text
			 * content, and any value a future arm might add.
			 */
			fst_xcheck(xml_curl_json_is_valid_text("<!") == 0, "the bare immune-mode trigger must be refused");
			fst_xcheck(xml_curl_json_is_valid_text("<![CDATA[x]]>") == 0, "a CDATA section must be refused");
			fst_xcheck(xml_curl_json_is_valid_text("<!-- x --><y z=\"1\"/>") == 0, "a comment opener must be refused");
			fst_xcheck(xml_curl_json_is_valid_text("1000<!DOCTYPE") == 0, "the sequence must be refused anywhere in the value");

			/* and a '<' that is NOT the trigger stays perfectly legal, because it is
			   escaped -- the check is exactly two characters wide and no wider */
			fst_xcheck(xml_curl_json_is_valid_text("a<b") == 1, "a lone < must still be accepted");
			fst_xcheck(xml_curl_json_is_valid_text("<") == 1, "a value that ends on < must not be over-read");
			fst_xcheck(xml_curl_json_is_valid_text("!<") == 1, "the reversed pair is not the trigger");
			fst_xcheck(xml_curl_json_is_valid_text("a<") == 1, "a trailing < must still be accepted");

			/* the same rule through the whole decoder, at both builder call sites */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"<![CDATA[1000]]>\"}}}", "directory"),
					   "an attribute value may not carry the immune-mode trigger");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"$\":\"<!-- \\\" injected=\\\"yes\"}}}", "directory"),
					   "text content may not carry the immune-mode trigger");

			/*
			 * And in its escaped spelling.  The lexical gate accepts \u003c, because U+003C
			 * is legal XML character data -- so this case only passes if the rule is applied
			 * to the DECODED value rather than to the payload bytes.
			 */
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"@id\":\"\\u003c!DOCTYPE\"}}}", "directory"),
					   "the escaped spelling of the trigger must be refused too");
			fst_xcheck(fst_xc_rejects("{\"directory\":{\"user\":{\"$\":\"a\\u003c\\u0021b\"}}}", "directory"),
					   "both halves of the trigger may be escaped");

			/* a legitimate lone '<' survives and comes back escaped, in an attribute and in
			   text alike, so no raw markup opener can reach the serialised document */
			rendered = fst_xc_render("{\"directory\":{\"user\":{\"@id\":\"a<b\",\"note\":{\"$\":\"1 < 2\"}}}}", "directory");
			fst_requires(rendered != NULL);
			fst_check_string_has(rendered, "id=\"a&lt;b\"");
			fst_check_string_has(rendered, "1 &lt; 2");
			fst_check_string_does_not_have(rendered, "<!");
			switch_safe_free(rendered);
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

			/* repeated children under one name are bounded before a single child is built,
			   so a long array is refused without ever paying for the insertions it asked
			   for. What bounds the COST of a width assembled from several member names --
			   each of which clears this check on its own -- is the separate per-parent
			   ceiling, pinned by adversarial_width_is_bounded below. */
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

		FST_TEST_BEGIN(adversarial_width_is_bounded)
		{
			switch_stream_handle_t stream = { 0 };
			char needle[32];
			char *rendered = NULL;
			const char *first = NULL;
			const char *last = NULL;
			int i;
			int j;

			/*
			 * Width is bounded because of what it COSTS to build.  switch_xml_add_child_d()
			 * reaches switch_xml_insert(), which walks the parent's ordered and same-name
			 * lists to their tail whenever the new offset is not smaller than the head's --
			 * and the translator passes a constant offset of zero so that insertion order
			 * becomes document order, so every append walks every sibling already present.
			 * n children under one parent therefore cost n*(n-1)/2 comparisons on each
			 * chain.  Neither of the other ceilings bounds that: the node ceiling would
			 * still admit XML_CURL_JSON_MAX_NODES children under a single parent, and the
			 * array ceiling is never reached by width accumulated across several member
			 * names.  These cases pin the per-parent ceiling that does bound it.
			 */

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":[");
			for (i = 0; i < XML_CURL_JSON_MAX_CHILDREN_PER_PARENT; i++) {
				stream.write_function(&stream, "%s{\"@id\":\"u%d\"}", i ? "," : "", i);
			}
			stream.write_function(&stream, "%s", "]}}");
			rendered = fst_xc_render((const char *) stream.data, "directory");
			switch_safe_free(stream.data);
			fst_requires(rendered != NULL);
			switch_snprintf(needle, sizeof(needle), "id=\"u%d\"", XML_CURL_JSON_MAX_CHILDREN_PER_PARENT - 1);
			fst_check_string_has(rendered, "id=\"u0\"");
			fst_check_string_has(rendered, needle);
			first = strstr(rendered, "id=\"u0\"");
			last = strstr(rendered, needle);
			fst_xcheck(first != NULL && last != NULL && first < last,
					   "a parent at the full ceiling must keep its children in document order");
			switch_safe_free(rendered);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":[");
			for (i = 0; i < XML_CURL_JSON_MAX_CHILDREN_PER_PARENT + 1; i++) {
				stream.write_function(&stream, "%s{\"@id\":\"u%d\"}", i ? "," : "", i);
			}
			stream.write_function(&stream, "%s", "]}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "one child past the per-parent ceiling must be refused");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{");
			for (i = 0; i < 3; i++) {
				stream.write_function(&stream, "%s\"u%d\":[", i ? "," : "", i);
				for (j = 0; j < 100; j++) {
					stream.write_function(&stream, "%s{\"@id\":\"x\"}", j ? "," : "");
				}
				stream.write_function(&stream, "%s", "]");
			}
			stream.write_function(&stream, "%s", "}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "width summed across several member names must be refused");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"u\":[");
			for (i = 0; i < XML_CURL_JSON_MAX_CHILDREN_PER_PARENT - 6; i++) {
				stream.write_function(&stream, "%s{\"@id\":\"x\"}", i ? "," : "");
			}
			stream.write_function(&stream, "%s", "]");
			for (i = 0; i < 10; i++) {
				stream.write_function(&stream, ",\"s%d\":{}", i);
			}
			stream.write_function(&stream, "%s", "}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "the ceiling must be enforced on single-object children too");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"user\":[");
			for (i = 0; i < XML_CURL_JSON_MAX_NODES; i++) {
				stream.write_function(&stream, "%s{}", i ? "," : "");
			}
			stream.write_function(&stream, "%s", "]}}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "a parent handed the whole node budget must be refused");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":{\"domain\":[");
			for (i = 0; i < 2; i++) {
				stream.write_function(&stream, "%s{\"user\":[", i ? "," : "");
				for (j = 0; j < XML_CURL_JSON_MAX_CHILDREN_PER_PARENT; j++) {
					stream.write_function(&stream, "%s{\"@id\":\"x\"}", j ? "," : "");
				}
				stream.write_function(&stream, "%s", "]}");
			}
			stream.write_function(&stream, "%s", "]}}");
			fst_xcheck(fst_xc_accepts((const char *) stream.data, "directory"),
					   "two parents may each sit at the per-parent ceiling");
			switch_safe_free(stream.data);
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

		/*
		 * The compact repeated-child document, which the array mapping exists to
		 * express and which must therefore never be refused as oversized.
		 *
		 * The decoder's resource ceilings bound what the translation ALLOCATES, and the
		 * volume of names it writes is not bounded by the length of the payload it
		 * reads: an array collapses n repeated children onto one key, so a key that
		 * occurs once in the input is written n times into the tree.
		 * {"directory":{"extension":[{},{},{},{},{}]}} is forty-four bytes of JSON and
		 * already writes forty-five bytes of element names, so charging name volume
		 * against the payload length would refuse the canonical shape - a silent,
		 * section-wide provisioning failure that looks like a malformed response.  Names
		 * are charged against their own expanded-output ceiling instead, and this case
		 * holds that apart from the input-size check.
		 *
		 * Parity is asserted rather than substring-matched, because the point is that
		 * the compact form produces exactly the tree its XML twin does; the ceilings
		 * that must still bite are asserted alongside it, so relaxing one accounting
		 * rule cannot quietly relax the others.
		 */
		FST_TEST_BEGIN(compact_repeated_children_are_not_refused)
		{
			static const char review_json[] = "{\"directory\":{\"extension\":[{},{},{},{},{}]}}";
			static const char review_xml[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<extension></extension><extension></extension><extension></extension>"
				"<extension></extension><extension></extension></section></document>";
			static const int within = XML_CURL_JSON_MAX_ARRAY_ELEMENTS;
			static const int beyond = XML_CURL_JSON_MAX_ARRAY_ELEMENTS + 1;
			char *from_json = NULL;
			char *from_xml = NULL;
			char *bulk = NULL;
			switch_xml_t xml_tree = NULL;
			switch_xml_t section_tag = NULL;
			switch_xml_t child = NULL;
			switch_size_t used = 0;
			switch_size_t room = 0;
			int i = 0;
			int counted = 0;

			from_json = fst_xc_render(review_json, "directory");
			fst_xcheck(from_json != NULL, "the canonical compact repeated-child document must not be refused");

			xml_tree = switch_xml_parse_str_dynamic((char *) review_xml, SWITCH_TRUE);
			fst_requires(xml_tree != NULL);
			from_xml = switch_xml_toxml(xml_tree, SWITCH_FALSE);
			switch_xml_free(xml_tree);
			fst_requires(from_xml != NULL);

			fst_check_string_equals(from_json, from_xml);
			switch_safe_free(from_json);
			switch_safe_free(from_xml);

			xml_tree = xml_curl_json_to_xml(review_json, "directory");
			fst_requires(xml_tree != NULL);
			section_tag = switch_xml_child(xml_tree, "section");
			fst_requires(section_tag != NULL);
			for (child = switch_xml_child(section_tag, "extension"); child; child = child->next) {
				counted++;
				fst_check_string_equals(child->name, "extension");
				fst_check_string_equals(child->txt, "");
			}
			fst_check_int_equals(counted, 5);
			switch_xml_free(xml_tree);
			xml_tree = NULL;

			/*
			 * A run right at the element ceiling is the worst case for separate name
			 * accounting: the payload is a few bytes per element while the tree receives
			 * the whole key each time, so the transformed name volume is an order of
			 * magnitude larger than the input.  Refusing it would mean the element
			 * ceiling could never actually be reached.
			 */
			room = (switch_size_t) (beyond * 4) + 128;
			bulk = (char *) malloc(room);
			fst_requires(bulk != NULL);

			switch_snprintf(bulk, room, "%s", "{\"directory\":{\"extension\":[");
			for (i = 0; i < within; i++) {
				used = strlen(bulk);
				switch_snprintf(bulk + used, room - used, "%s{}", i ? "," : "");
			}
			used = strlen(bulk);
			switch_snprintf(bulk + used, room - used, "%s", "]}}");

			fst_xcheck(fst_xc_accepts(bulk, "directory"), "a run of repeated children at the element ceiling must be accepted");

			switch_snprintf(bulk, room, "%s", "{\"directory\":{\"extension\":[");
			for (i = 0; i < beyond; i++) {
				used = strlen(bulk);
				switch_snprintf(bulk + used, room - used, "%s{}", i ? "," : "");
			}
			used = strlen(bulk);
			switch_snprintf(bulk + used, room - used, "%s", "]}}");

			fst_xcheck(fst_xc_rejects(bulk, "directory"), "a run of repeated children beyond the element ceiling must be refused");

			switch_safe_free(bulk);

			/*
			 * The charge helper every ceiling routes through tests the remaining headroom
			 * instead of the sum, so an addend large enough to wrap the counter is refused
			 * rather than silently accepted - the failure mode that would make every
			 * ceiling above bypassable with one enormous name.
			 */
			{
				switch_size_t counter = 0;
				int charged = 0;

				/*
				 * Each charge is taken once into a local and the local is asserted.
				 * fst_check_int_equals() expands its first operand twice - once in the
				 * comparison and once as a message argument - so a call with a side
				 * effect placed there would be applied twice, in an order the standard
				 * does not fix.
				 */
				charged = xml_curl_json_budget_charge(&counter, 10, 10);
				fst_check_int_equals(charged, 1);
				fst_xcheck(counter == 10, "an accepted charge must be applied");

				charged = xml_curl_json_budget_charge(&counter, 1, 10);
				fst_check_int_equals(charged, 0);
				fst_xcheck(counter == 10, "a refused charge must leave the counter untouched");

				charged = xml_curl_json_budget_charge(&counter, (switch_size_t) -1, 10);
				fst_check_int_equals(charged, 0);
				fst_xcheck(counter == 10, "an addend that would wrap the counter must be refused, not wrapped");

				charged = xml_curl_json_budget_charge(NULL, 1, 10);
				fst_check_int_equals(charged, 0);
			}

			/*
			 * Names are accounted separately from values, but they are still accounted:
			 * a single name beyond the per-name ceiling is refused.
			 */
			{
				char oversized[XML_CURL_JSON_MAX_NAME_BYTES + 64] = "";
				char document[XML_CURL_JSON_MAX_NAME_BYTES + 128] = "";

				memset(oversized, 'a', XML_CURL_JSON_MAX_NAME_BYTES + 1);
				oversized[XML_CURL_JSON_MAX_NAME_BYTES + 1] = '\0';
				switch_snprintf(document, sizeof(document), "{\"directory\":{\"%s\":{}}}", oversized);
				fst_xcheck(fst_xc_rejects(document, "directory"), "an element name beyond the per-name ceiling must still be refused");
			}
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
			 * A configured gateway-url legitimately carries userinfo, query strings
			 * routinely carry tokens and a provisioning path routinely carries a tenant
			 * or account identifier, so the one fallback warning has to be able to name
			 * the gateway without reproducing any of them. Only the scheme and the
			 * authority survive; everything from the first '/', '?' or '#' onwards is
			 * collapsed into a single marker.
			 */
			fst_check_string_equals(xml_curl_json_redact_url("https://user:pass@example.com:8080/prov?token=abc#frag", buf, sizeof(buf)),
									"https://[redacted]@example.com:8080/[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com/prov", buf, sizeof(buf)),
									"http://example.com/[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com/prov#frag", buf, sizeof(buf)),
									"http://example.com/[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("example.com/prov?t=1", buf, sizeof(buf)), "example.com/[redacted]");
			/* a bare gateway host has nothing to collapse, so it carries no marker and
			 * stays distinguishable from one that did */
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com", buf, sizeof(buf)), "http://example.com");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com:8080", buf, sizeof(buf)), "http://example.com:8080");
			/* the marker is the same however the remainder was spelled: a bare '/', a
			 * query with no path, and a fragment with no path all render identically */
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com/", buf, sizeof(buf)), "http://example.com/[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com?t=1", buf, sizeof(buf)), "http://example.com/[redacted]");
			fst_check_string_equals(xml_curl_json_redact_url("http://example.com#f", buf, sizeof(buf)), "http://example.com/[redacted]");
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
			switch_curl_slist_t *head = NULL;

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

			/*
			 * The 100-continue suppression appends to the SAME list when a binding both
			 * disables 100-continue and asks for JSON, because that configuration routes the
			 * Accept entry into the list the suppression then extends.  It therefore has to
			 * go through this helper as well: assigning the append result straight back would
			 * replace a list already carrying Accept with NULL on an allocation failure, leak
			 * it, and hand curl an empty header set while the fetch still believed it had
			 * negotiated JSON.  Composed through the helper, the two entries coexist in
			 * request order and a later refusal cannot disturb either of them.
			 */
			fst_xcheck(xml_curl_json_append_header(&list, "Expect:") == 1, "the Expect suppression must append after them");
			fst_check_string_equals(list->next->next->data, "Expect:");
			fst_check(list->next->next->next == NULL);

			/*
			 * The failure path of that third append is the one the finding named: the
			 * caller's own pointer must still be the head of the same three-entry list
			 * afterwards, because that pointer is what reaches CURLOPT_HTTPHEADER.  An
			 * append that assigned its result back would have replaced it with NULL and
			 * leaked all three entries while the fetch still believed it had negotiated
			 * JSON.
			 */
			head = list;
			fst_xcheck(xml_curl_json_append_header(&list, NULL) == 0, "a refusal after the third entry must still report failure");
			fst_xcheck(list == head, "a refused append must leave the caller's list pointer exactly as it was");
			fst_check_string_equals(list->data, "Content-Type: application/x-www-form-urlencoded");
			fst_check_string_equals(list->next->data, "Accept: application/json");
			fst_check_string_equals(list->next->next->data, "Expect:");
			fst_check(list->next->next->next == NULL);

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

			fst_xc_temp_path(FST_XC_TEMP_XML, path, sizeof(path));
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
		}
		FST_TEST_END()

		/*
		 * The complete HTTP fetch, with no peer.
		 *
		 * Every other case in this suite observes one piece of the fetch.  This one
		 * runs xml_url_fetch() itself: the real request-header composition, the real
		 * temporary file, the real HTTP-200 gate, the real response-format dispatch,
		 * the real switch_xml_parse_file() fallback and the real unlink.  Only the
		 * transport underneath is replaced.
		 *
		 * That distinction is the point.  A test that drove only
		 * xml_curl_json_decode_response() would still pass if the fallback parse were
		 * deleted from the production dispatch, because that helper's contract is to
		 * return NULL and let the caller fall back -- so the sub-cases below are
		 * written to fail in exactly that situation: each one asserts which decoder
		 * produced the tree, from a user id that only one of the two response bodies
		 * contains.
		 */
		FST_TEST_BEGIN(http_dispatch_end_to_end)
		{
			/* the JSON body names user 1000; its XML counterpart names user 2000, so the
			   id in the returned tree says which decoder ran */
			static const char json_body[] = "{\"directory\":{\"user\":{\"@id\":\"1000\"}}}";
			static const char xml_body[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"2000\"></user></section></document>";
			xml_binding_t binding;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());

			/*
			 * 1. The binding asked for JSON and the gateway answered JSON.  The
			 *    translator decodes it, nothing falls back, and the request carried the
			 *    Accept header on the list curl was handed.
			 */
			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "http://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";

			xml = fst_xc_drive_fetch(&binding, "directory", json_body, "application/json; charset=utf-8", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_transport.perform_count, 1);
			fst_check_int_equals(fst_xc_transport.body_write_failures, 0);
			/* both header lists are released, so neither is leaked whichever branch ran */
			fst_check_int_equals(fst_xc_transport.released_list_count, 2);
			fst_xcheck(fst_xc_transport_header_in_list(FST_XC_HEADER_LIST_CONTENT_TYPE, "Content-Type: application/x-www-form-urlencoded"),
					   "the pre-existing request content type must still be sent");
			fst_xcheck(fst_xc_transport_header_in_list(FST_XC_HEADER_LIST_CONTENT_TYPE, "Accept: application/json"),
					   "a JSON binding must advertise Accept: application/json on the list curl receives");
			/* the 100-continue list is not built at all for this binding */
			fst_check_int_equals(fst_xc_transport.sent_header_count[FST_XC_HEADER_LIST_EXPECT], 0);
			/*
			 * The handoff, not just the build: removing the CURLOPT_HTTPHEADER call fails
			 * these, where a release-time observation on its own would not notice.
			 */
			fst_check_int_equals(fst_xc_transport.httpheader_set_count, 1);
			fst_xcheck(fst_xc_transport.setopt_count > fst_xc_transport.httpheader_set_count,
					   "the option observer must see the whole option stream, not only the header handoff");
			fst_xcheck(fst_xc_transport.httpheader_handle == fst_xc_transport.init_handle,
					   "the header list must be set on the handle this fetch created");
			fst_check_int_equals(fst_xc_transport_request_list(), FST_XC_HEADER_LIST_CONTENT_TYPE);
			fst_xcheck(fst_xc_transport_request_header("Content-Type: application/x-www-form-urlencoded"),
					   "the pre-existing request content type must be on the list curl was handed");
			fst_xcheck(fst_xc_transport_request_header("Accept: application/json"),
					   "the Accept entry must be on the list curl was handed, not merely on a list that was built");
			fst_xcheck(fst_xc_transport.last_httpheader_op < fst_xc_transport.perform_op,
					   "the header list must reach curl before the transfer is performed");
			/*
			 * The response metadata is read while the handle is still alive, and never
			 * afterwards: libcurl frees the Content-Type string with the handle, so a read
			 * past switch_curl_easy_cleanup() is a use-after-free.
			 */
			fst_xcheck(fst_xc_transport.response_code_op > 0, "the response code must be read");
			fst_xcheck(fst_xc_transport.content_type_op > 0, "the response content type must be read");
			fst_xcheck(fst_xc_transport.cleanup_op > 0, "the handle must be destroyed");
			fst_xcheck(fst_xc_transport.response_code_op < fst_xc_transport.cleanup_op,
					   "the response code must be read before the handle is destroyed");
			fst_xcheck(fst_xc_transport.content_type_op < fst_xc_transport.cleanup_op,
					   "the response content type must be read before the handle is destroyed");
			fst_check_int_equals(fst_xc_transport.getinfo_after_cleanup, 0);
			fst_check_int_equals(fst_xc_transport.getinfo_on_unknown_handle, 0);
			fst_check_int_equals(fst_xc_transport.content_type_alloc_failures, 0);
			fst_xcheck(fst_xc_transport.content_type_served_from_handle,
					   "the content type must be served from storage the handle owns, as libcurl serves it");
			fst_check_int_equals(fst_xc_transport.content_type_storage_freed, 1);
			fst_xcheck(xml != NULL, "a JSON response to a JSON binding must decode");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "1000");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 2. The binding asked for JSON and the gateway answered XML, announcing it
			 *    as XML.  This is the content-type mismatch: exactly one warning, and the
			 *    document still resolves -- through the untouched XML parse.  The id
			 *    proves it: 2000 exists only in the XML body, so this assertion fails if
			 *    the fallback parse is removed.
			 */
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "text/xml; charset=utf-8", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(xml != NULL, "a mismatched content type must fall back to the XML parse, not fail the fetch");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "2000");
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 3. The binding asked for JSON, the gateway announced JSON, and the body is
			 *    not JSON at all.  Same single warning, same fallback, same resolved
			 *    document -- the announcement is not trusted over the bytes.
			 */
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(xml != NULL, "a body that is not JSON must fall back to the XML parse");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "2000");
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 4. Nothing can decode an empty body, so both decoders report and the fetch
			 *    resolves to nothing.  This is the one path that emits the terminal parse
			 *    error as well as the fallback warning, and the two are distinct messages:
			 *    the warning explains why JSON was abandoned, the error reports that XML
			 *    failed too.
			 */
			xml = fst_xc_drive_fetch(&binding, "directory", "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "an empty body must resolve to nothing");
			fst_xc_unlink_preprocessed();

			/*
			 * 5. With no response-format the gateway's JSON is treated as XML: no Accept
			 *    header is negotiated, no decode is attempted, no fallback warning is
			 *    emitted, and switch_xml_parse_file() is handed the body and returns the
			 *    diagnostic-carrying tree it returns for any non-XML document.  That last
			 *    detail is what makes this a behaviour assertion rather than a smoke test.
			 */
			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "http://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;

			xml = fst_xc_drive_fetch(&binding, "directory", json_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_xcheck(!fst_xc_transport_sent_header("Accept: application/json"),
					   "a binding that did not ask for JSON must not advertise it");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(xml != NULL, "the XML-only path must return the parser's tree unchanged");
			if (xml) {
				fst_xcheck(*switch_xml_error(xml) != '\0', "the parser must report a JSON body as the malformed XML it is");
			}
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 6. A non-200 response.  The success gate is untouched, so no decoder runs
			 *    at all: no fallback warning, no parse error, and no document.
			 */
			binding.response_format = (char *) "json";
			xml = fst_xc_drive_fetch(&binding, "directory", json_body, "application/json", 500);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(xml == NULL, "a non-200 response must resolve to nothing whatever the format is");

			/*
			 * 7. The composition that the non-destructive append exists for: a binding
			 *    that both suppresses the 100-continue and asks for JSON.  That
			 *    configuration routes the Accept entry onto the same list the Expect
			 *    suppression builds, and the production code hands curl that list and
			 *    only that list -- so both entries have to be on it, and neither may
			 *    have landed on the list curl never receives.
			 */
			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "http://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";
			binding.disable100continue = 1;

			xml = fst_xc_drive_fetch(&binding, "directory", json_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_transport.released_list_count, 2);
			fst_xcheck(fst_xc_transport_header_in_list(FST_XC_HEADER_LIST_EXPECT, "Accept: application/json"),
					   "Accept must be on the list a 100-continue-suppressing binding hands curl");
			fst_xcheck(fst_xc_transport_header_in_list(FST_XC_HEADER_LIST_EXPECT, "Expect:"),
					   "the 100-continue suppression must be on that same list");
			fst_xcheck(!fst_xc_transport_header_in_list(FST_XC_HEADER_LIST_CONTENT_TYPE, "Accept: application/json"),
					   "Accept must not be appended to the list this binding makes curl discard");
			fst_check_int_equals(fst_xc_transport.httpheader_set_count, 2);
			fst_xcheck(fst_xc_transport.httpheader_list[0] != fst_xc_transport.httpheader_list[1],
					   "the second handoff must replace the list curl holds rather than repeat the first");
			fst_xcheck(fst_xc_transport.httpheader_op[0] < fst_xc_transport.httpheader_op[1],
					   "the replacing handoff must come second, so it is the one that takes effect");
			fst_xcheck(fst_xc_transport.last_httpheader_op < fst_xc_transport.perform_op,
					   "both handoffs must precede the transfer");
			fst_check_int_equals(fst_xc_transport_request_list(), FST_XC_HEADER_LIST_EXPECT);
			fst_xcheck(fst_xc_transport_request_header("Accept: application/json"),
					   "Accept must be on the list curl was actually left holding");
			fst_xcheck(fst_xc_transport_request_header("Expect:"),
					   "the 100-continue suppression must be on the list curl was actually left holding");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_xcheck(xml != NULL, "suppressing the 100-continue must not disturb the JSON decode");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "1000");
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 8. A mixed-case parameter value, on the HTTP path: the file: shortcut returns
			 *    before the format decision is reached, so a case assertion made there
			 *    proves nothing about negotiation or dispatch.  Here the value has to
			 *    survive the Accept negotiation, the decode dispatch and the decoded
			 *    document.
			 */
			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "http://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "JSON";

			xml = fst_xc_drive_fetch(&binding, "directory", json_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_transport_request_list(), FST_XC_HEADER_LIST_CONTENT_TYPE);
			fst_xcheck(fst_xc_transport_request_header("Accept: application/json"),
					   "a mixed-case response-format value must still negotiate JSON on the request curl carries");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(xml != NULL, "a mixed-case response-format value must still decode a JSON response");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "1000");
			switch_xml_free(xml);
			xml = NULL;


			/* asserted rather than called: the release is only safe once the unbind
			   has demonstrably removed this logger from the dispatcher */
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * Neither of the two lines the JSON path can write may carry any part of the
		 * gateway URL beyond its authority, and neither may carry the request form body.
		 *
		 * The path of a provisioning URL is not inert.  Deployments routinely address a
		 * tenant by putting its identifier - or an account key, or a bearer value - in a
		 * path segment, exactly as they put tokens in a query string, and a warning is
		 * written to whatever the operator's logger persists to.  Redacting the userinfo
		 * and the query while reproducing the path therefore leaves the same class of
		 * secret in the same place.  The form body is worse: it ends with the value that
		 * was looked up and carries every event variable the binding mapped into it.
		 *
		 * This case drives the REAL fetch, so it observes the bytes the module logs
		 * rather than the return value of the redaction helper: the bound counting logger
		 * sees every dispatched line, and the assertion is that the count of lines
		 * carrying the secret is zero while the count of the expected message is one.
		 * Asserting the helper alone would still pass if an edit logged binding->url or
		 * the form body directly somewhere else in the fetch.  Both the fallback warning
		 * and the terminal diagnostic are covered, because covering only the first leaves
		 * the leakiest line in the module unexecuted by the case that exists to police it.
		 */
		FST_TEST_BEGIN(log_hygiene_url_path_is_never_logged)
		{
			/* announced as XML against a JSON binding, so the decode is abandoned
			   and the one fallback warning is emitted */
			static const char xml_body[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"2000\"></user></section></document>";
			static const char secret_path_segment[] = "s3cr3t-tenant-token";
			static const char secret_password[] = "hunter2";
			static const char secret_query_value[] = "deadbeefapikey";
			static const char secret_subscriber[] = "s3cr3t-subscriber-9911";
			static const char gateway_url[] = "https://provisioner:hunter2@127.0.0.1:1"
				"/tenants/s3cr3t-tenant-token/directory?apikey=deadbeefapikey#frag";
			char rendered[256] = "";
			xml_binding_t binding;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());

			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) gateway_url;
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";

			/* 1. the secret path segment reaches no log line at all */
			fst_xc_log_watch_absent(secret_path_segment);
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry a path segment of the gateway URL");
			switch_xml_free(xml);
			xml = NULL;

			/* 2. neither does the userinfo password, which was already redacted */
			fst_xc_log_watch_absent(secret_password);
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry the gateway URL's userinfo");
			switch_xml_free(xml);
			xml = NULL;

			/* 3. nor the query value */
			fst_xc_log_watch_absent(secret_query_value);
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry the gateway URL's query string");
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * 4. and the warning is still useful: what survives is precisely the
			 *    scheme and the authority, which is what names the gateway that
			 *    degraded.  Asserted on the helper as well as on the log, because
			 *    "logs nothing" would satisfy the three checks above on its own.
			 */
			fst_xc_log_watch_absent("127.0.0.1:1");
			xml = fst_xc_drive_fetch(&binding, "directory", xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 1, "the warning must still identify the gateway by authority");
			switch_xml_free(xml);
			xml = NULL;

			fst_check_string_equals(xml_curl_json_redact_url(gateway_url, rendered, sizeof(rendered)),
									"https://[redacted]@127.0.0.1:1/[redacted]");

			/*
			 * The terminal "Error Parsing Result!" diagnostic, whose two operands are the
			 * gateway URL and the POST form body.  Everything above supplies a body the XML
			 * parse can read, so the fetch resolves and that line never runs; a
			 * confidentiality case that never executes it proves nothing about it.
			 *
			 * An empty body reaches it deterministically.  switch_xml_parse_str() never
			 * returns NULL - it returns a tree carrying an error string - so
			 * switch_xml_parse_file() reports failure only when the preprocessed file has
			 * no bytes at all.  Every sub-case below therefore answers with an empty
			 * payload announced as JSON: the decode is abandoned (one warning), the XML
			 * parse then fails (one error), and the fetch resolves to nothing.
			 *
			 * The watched strings are the two operands' secrets, one fetch each because
			 * only one absence pattern can be armed at a time: the URL's path segment, its
			 * userinfo password, its query value, the lookup's key_value, and the literal
			 * "key_value=" - which appears in the form body and nowhere else, so it catches
			 * the body being logged whatever values it happens to carry.
			 */
			fst_xc_log_watch_absent(secret_path_segment);
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "the terminal parse error must not carry a path segment of the gateway URL");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent(secret_password);
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "the terminal parse error must not carry the gateway URL's userinfo");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent(secret_query_value);
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "the terminal parse error must not carry the gateway URL's query string");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent(secret_subscriber);
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "the terminal parse error must not carry the value that was looked up");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent("key_value=");
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry the request form body");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			/*
			 * And the diagnostic is still worth writing.  "Logs nothing at all"
			 * would satisfy every absence check above, so the last two sub-cases
			 * assert what must survive: the gateway's authority, which says which
			 * gateway failed, and the lookup coordinates, which say what it was
			 * asked for.  Those coordinates are the safe half of the form body -
			 * section, tag_name and key_name are core-supplied selectors, never
			 * secrets - and they are what makes the line actionable without it.
			 */
			fst_xc_log_watch_absent("127.0.0.1:1");
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) >= 1, "the terminal parse error must still identify the gateway by authority");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent("section [directory] tag_name [user] key_name [id]");
			xml = fst_xc_drive_fetch_ex(&binding, "directory", secret_subscriber, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT), 1);
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent(NULL);
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * Log injection through the one operand of these lines that the CALLER chooses
		 * rather than the operator or the remote gateway.
		 *
		 * A section selector is not an internal constant.  mod_commands registers an
		 * `xml_locate' API command whose argument is passed straight into
		 * switch_xml_locate(), which is what dispatches to this module's binding - so the
		 * bytes that reach these lines are picked by whoever issued the lookup, over the
		 * console or over an event socket.  A selector carrying CR LF forges a second,
		 * wholly fabricated log entry, and one carrying an escape sequence reaches the
		 * terminal of whoever tails the log.  Neither is prevented by the URL and
		 * Content-Type renderings, which are the other two operands of the same warning.
		 *
		 * Both lines that render a section are covered.  The expected rendering is written
		 * out by hand rather than computed from the helper, so each assertion states
		 * independently what the sanitizer must produce.
		 */
		FST_TEST_BEGIN(log_hygiene_section_selector_is_neutralised)
		{
			/* announced as XML against a JSON binding, so the decode is abandoned
			   and the one fallback warning is emitted while the fetch still
			   resolves */
			static const char xml_body[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"2000\"></user></section></document>";
			/* a selector a caller can ask for: a bracket that closes the field
			   early, CR LF to begin a line of its own, a CSI clear-screen aimed at
			   the operator's terminal, a lone control byte, and a forged severity
			   to sit in the fabricated entry */
			static const char hostile_section[] = "directory]\r\n\033[2J\001[CRITICAL] forged";
			/* the same bytes with every one outside printable ASCII replaced by
			   '.', computed by hand: CR, LF and ESC are three consecutive
			   replacements - the escape sequence loses only its introducer, so the
			   "[2J" that followed it stays visible as the inert text it now is -
			   then SOH is the fourth, and nothing else may be altered */
			static const char neutralised[] = "directory]...[2J.[CRITICAL] forged";
			char oversized[256] = "";
			char rendered[64] = "";
			char needle[256] = "";
			xml_binding_t binding;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());

			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "https://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";

			/* the rendering on its own, against an independently written
			   expectation.  The buffer is the size the production line uses, so
			   this also states that a selector of this length survives whole. */
			fst_check_string_equals(xml_curl_json_sanitize_token(hostile_section, rendered, sizeof(rendered)), neutralised);

			fst_xc_log_watch_absent("\r\n");
			xml = fst_xc_drive_fetch(&binding, hostile_section, xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry a newline taken from the requested section");
			switch_xml_free(xml);
			xml = NULL;

			fst_xc_log_watch_absent("\033[2J");
			xml = fst_xc_drive_fetch(&binding, hostile_section, xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry a terminal control sequence from the requested section");
			switch_xml_free(xml);
			xml = NULL;

			fst_xc_log_watch_absent("\001");
			xml = fst_xc_drive_fetch(&binding, hostile_section, xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "no log line may carry a control byte from the requested section");
			switch_xml_free(xml);
			xml = NULL;

			/* the watched needle must be PRESENT here: "logs nothing" would satisfy the
			   three absence checks above on its own */
			switch_snprintf(needle, sizeof(needle), "JSON decode of the [%s] response", neutralised);
			fst_xc_log_watch_absent(needle);
			xml = fst_xc_drive_fetch(&binding, hostile_section, xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT), 1);
			switch_xml_free(xml);
			xml = NULL;

			/* a stable marker rather than an empty field, so a selector that never arrived
			   cannot be mistaken for one that was present and blank */
			switch_copy_string(needle, "JSON decode of the [(absent)] response", sizeof(needle));
			fst_xc_log_watch_absent(needle);
			xml = fst_xc_drive_fetch(&binding, "", xml_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT), 1);
			switch_xml_free(xml);
			xml = NULL;

			memset(oversized, 'A', 200);
			switch_snprintf(oversized + 200, sizeof(oversized) - 200, "%s", "TAILMARKER");
			fst_xc_log_watch_absent("TAILMARKER");
			xml = fst_xc_drive_fetch(&binding, oversized, xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0,
					   "a selector longer than the rendering buffer must be truncated rather than logged whole");
			switch_xml_free(xml);
			xml = NULL;

			/* an empty body announced as JSON is what reaches the terminal diagnostic */
			fst_xc_log_watch_absent("\r\n");
			xml = fst_xc_drive_fetch(&binding, hostile_section, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_xcheck(xml == NULL, "a body neither decoder can read must resolve to nothing");
			fst_xcheck(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT) == 0, "the terminal parse error must not carry a newline from the requested section");
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			switch_snprintf(needle, sizeof(needle), "section [%s] tag_name", neutralised);
			fst_xc_log_watch_absent(needle);
			xml = fst_xc_drive_fetch(&binding, hostile_section, "", "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_ABSENT), 1);
			switch_xml_free(xml);
			xml = NULL;
			fst_xc_unlink_preprocessed();

			fst_xc_log_watch_absent(NULL);
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * Declared at the tail of the suite, together with the binding-notice case
		 * below: these two are the only cases that mutate module-wide state, and each
		 * releases it again before it returns, so nothing that runs after them can be
		 * affected by what they leave behind.
		 */
		FST_TEST_BEGIN(response_format_configuration_parsing)
		{
			char dir_path[1024] = "";
			char dial_path[1024] = "";
			xml_binding_t *json_binding = NULL;
			xml_binding_t *xml_binding = NULL;
			switch_xml_t located = NULL;
			switch_xml_t located_node = NULL;
			switch_xml_t extension_tag = NULL;

			fst_xc_temp_path(FST_XC_TEMP_DIRECTORY_XML, dir_path, sizeof(dir_path));
			fst_xc_temp_path(FST_XC_TEMP_DIALPLAN_XML, dial_path, sizeof(dial_path));
			fst_requires(fst_xc_write_file(dir_path, fst_xc_directory_document, strlen(fst_xc_directory_document)));
			fst_requires(fst_xc_write_file(dial_path, fst_xc_dialplan_document, strlen(fst_xc_dialplan_document)));

			/*
			 * The configuration string outlives the do_config() call that reads it,
			 * because the registered provider re-parses it on every lookup.  Handing it
			 * to the suite-owned pointer means the teardown block frees it even if an
			 * assertion below never lets this case reach its own tail.
			 */
			fst_xc_module_conf = switch_mprintf(fst_xc_conf_template, dir_path, dial_path);
			fst_requires(fst_xc_module_conf != NULL);

			/*
			 * do_config() allocates every binding and duplicates every string member into
			 * the module pool, so it has to be given one -- and it must not be fst_pool.
			 * See the note above fst_xc_module_pool_create(): FST destroys the per-test
			 * pool before the teardown body runs, so a binding allocated out of it would
			 * be unbound through freed memory on any failing assertion.
			 */
			fst_requires(fst_xc_module_pool_create());

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
														   (void *) fst_xc_module_conf, NULL) == SWITCH_STATUS_SUCCESS,
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
			xml_binding = (xml_binding_t *) fst_xc_observed_bindings[1];

			/*
			 * Every assertion from here on is non-fatal.  This case has already lent the
			 * module a pool, registered a configuration provider and registered two fetch
			 * bindings with the core, and fst_requires() abandons the case body on the
			 * spot -- so a fatal assertion here would hand the rest of the run a core
			 * still carrying this case's bindings.  The teardown block does clean up
			 * unconditionally, but the case reads its own state below, so it guards
			 * instead of trusting.
			 */
			if (json_binding) {
				fst_xcheck(json_binding->response_format != NULL, "the opted-in binding must carry response-format");
				if (json_binding->response_format) {
					fst_check_string_equals(json_binding->response_format, "json");
				}

				/*
				 * And the eighteen parameters that were already there are unchanged by
				 * the nineteenth: a sample spanning a string, an integer, a long, a
				 * boolean-gated flag, a size and the post-variable hash is read back off
				 * the same binding.
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
			} else {
				fst_fail("do_config() did not build the binding that carries response-format");
			}

			/*
			 * The binding that omitted it is left at the NULL the wholesale memset()
			 * wrote, which is the whole mechanism by which a binding that does not ask
			 * for JSON stays XML-only.
			 */
			if (xml_binding) {
				fst_check(xml_binding->response_format == NULL);

				/* and it keeps the module's own defaults for everything else */
				fst_check_string_equals(xml_binding->bindings, "dialplan");
				fst_check(xml_binding->method == NULL);
				fst_check(xml_binding->cred == NULL);
				fst_check_int_equals(xml_binding->timeout, 0);
				fst_check_int_equals(xml_binding->disable100continue, 1);
				fst_check_int_equals((int) xml_binding->auth_scheme, (int) CURLAUTH_BASIC);
				fst_check_int_equals((int) xml_binding->curl_max_bytes, (int) XML_CURL_MAX_BYTES);
				fst_check(xml_binding->vars_map == NULL);
			} else {
				fst_fail("do_config() did not build the binding that omits response-format");
			}

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

			/*
			 * Shutdown is asserted rather than left implicit.  It releases the
			 * enable-post-var hash this configuration created and removes the fetch
			 * bindings do_config() registered, which is what makes the borrowed pool safe
			 * to destroy.  The whole sequence -- unbind the provider, shut the module
			 * down, drop globals.pool, destroy the pool, free the configuration string --
			 * lives in one idempotent helper that the teardown block also calls, so the
			 * only thing left to assert here is that the module reported success.
			 */
			fst_check(switch_xml_unbind_search_function_ptr(fst_xc_conf_search) == SWITCH_STATUS_SUCCESS);
			fst_check(mod_xml_curl_shutdown() == SWITCH_STATUS_SUCCESS);
			fst_xc_module_state_cleanup();
		}
		FST_TEST_END()

		/*
		 * The notice do_config() writes once per registered binding must not persist the
		 * gateway URL verbatim.
		 *
		 * A provisioning URL routinely carries a secret: an HTTP userinfo password, a
		 * tenant token in a path segment, an API key in the query.  This notice is written
		 * as the module loads and then stays in the log for as long as the log is kept, so
		 * an unredacted rendering is a durable disclosure of a live credential to everyone
		 * who can read the log -- and rotating the credential afterwards does not undo it.
		 *
		 * Two passes over the same one-binding configuration, because an absence assertion
		 * on its own would also be satisfied by a notice that said nothing at all.  The
		 * first pass asserts the exact redacted rendering is present, which proves the
		 * notice is still emitted and still names the gateway that was registered; the
		 * second asserts the planted token is absent from every line the same call
		 * produced.  Only the two together are the property being claimed.
		 *
		 * The recorded binding count is asserted in each pass as well.  It is incremented
		 * where do_config() hands the binding to the core, immediately above the notice, so
		 * a run that never got that far cannot be mistaken for a clean one.
		 *
		 * A barrier that did not complete makes the helper return -1, which fails the
		 * comparison that follows it, so an unread log queue cannot pass unnoticed either.
		 */
		FST_TEST_BEGIN(log_hygiene_binding_notice_redacts_gateway_url)
		{
			static const char canary[] = "n0t1ce-canary";
			static const char rendered[] = "http://[redacted]@127.0.0.1:19011/[redacted]";
			switch_status_t status = SWITCH_STATUS_FALSE;
			int hits = 0;

			fst_requires(fst_xc_log_capture_start());

			/*
			 * Neither pass is written as a loop iteration on purpose.  fst_requires()
			 * abandons a case body with break, so inside a loop it would leave the loop
			 * rather than the case and everything after it would still run.
			 *
			 * Pass one: the redacted rendering is present exactly once.
			 */
			fst_xc_module_conf = strdup(fst_xc_notice_conf);
			fst_requires(fst_xc_module_conf != NULL);
			fst_requires(fst_xc_module_pool_create());
			fst_xcheck(switch_xml_bind_search_function_ret(fst_xc_conf_search, switch_xml_parse_section_string("configuration"),
														   (void *) fst_xc_module_conf, NULL) == SWITCH_STATUS_SUCCESS,
					   "the configuration provider must register");

			/* every assertion from here to the release below is non-fatal: module-wide
			   state has been lent out, and the release has to be reached */
			fst_xc_observed_binding_count = 0;
			hits = fst_xc_count_config_log(rendered, &status);

			fst_xcheck(hits >= 0, "the log queue must drain before the counters are read");
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "the configuration carrying the canary URL must parse");
			fst_check_int_equals(fst_xc_observed_binding_count, 1);
			fst_xcheck(hits == 1, "the notice must still identify the gateway by scheme and authority, with the rest redacted");

			fst_xc_module_state_cleanup();

			/* Pass two: no part of the URL beyond the authority reaches any line. */
			fst_xc_module_conf = strdup(fst_xc_notice_conf);
			fst_requires(fst_xc_module_conf != NULL);
			fst_requires(fst_xc_module_pool_create());
			fst_xcheck(switch_xml_bind_search_function_ret(fst_xc_conf_search, switch_xml_parse_section_string("configuration"),
														   (void *) fst_xc_module_conf, NULL) == SWITCH_STATUS_SUCCESS,
					   "the configuration provider must register a second time");

			fst_xc_observed_binding_count = 0;
			hits = fst_xc_count_config_log(canary, &status);

			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "the configuration carrying the canary URL must parse again");
			fst_check_int_equals(fst_xc_observed_binding_count, 1);
			fst_xcheck(hits == 0, "the userinfo, the path and the query of a gateway URL must never reach a log line");

			fst_xc_module_state_cleanup();

			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * A cookie jar is opened for writing by libcurl and rewritten after every lookup,
		 * so the name the configuration supplies decides what gets overwritten.  The gate
		 * in front of it has to tell five shapes apart, and this case drives all five
		 * directly -- the two setopt calls it guards are interposed in this translation
		 * unit, so what a jar is has to be asserted at the gate rather than through it.
		 *
		 * The refusals are the security property: a symlink must not be written through, a
		 * directory or special file must not be opened at all, a file other users can write
		 * must not be trusted, and a file belonging to somebody else must not be touched.
		 * The acceptances are what keeps the property from being achieved by refusing
		 * everything: an ordinary private jar of ours still works, and one that does not
		 * exist yet is still created -- for this user alone, under a permissive umask, which
		 * is the condition under which a jar created with the process default would be
		 * world readable.
		 *
		 * Every refusal is also asserted to be diagnosable and to disclose nothing: the
		 * warning is counted, the URL it names is checked for the planted token, and then
		 * checked for the authority so that "logged nothing" cannot satisfy the absence.
		 *
		 * The umask is changed around one call only, and restored immediately: it is
		 * process-wide, and the point of setting it is that the mode asserted afterwards
		 * can then only have come from the gate's own open().
		 */
		FST_TEST_BEGIN(cookie_jar_path_is_validated)
		{
			static const char url[] = "http://provisioner:j4r-canary@127.0.0.1:19012/tenants/j4r-canary/directory?apikey=j4r-canary";
			static const char refusal[] = "Refusing cookie file";
			static const char reason_type[] = "it is not a regular file";
			static const char reason_owner[] = "it belongs to another user";
			static const char reason_mode[] = "other users can write it";
			static const char reason_dir[] = "the directory holding it is writable by other users and not sticky";
			char jar[1024] = "";
			char canary[1024] = "";
			char shared[1024] = "";
			struct stat st;
			mode_t saved_umask = 0;
			int accepted = -1;
			int hits = 0;
			int fd = -1;

			fst_requires(fst_xc_log_capture_start());

			/* a jar that does not exist yet is created, and created for this user only */
			fst_xc_temp_path(FST_XC_TEMP_COOKIE_NEW, jar, sizeof(jar));
			saved_umask = umask(0022);
			hits = fst_xc_count_jar_log(refusal, jar, url, &accepted);
			umask(saved_umask);

			fst_xcheck(hits >= 0, "the log queue must drain before the counters are read");
			fst_xcheck(accepted == 1, "a jar that does not exist yet must be usable");
			fst_check_int_equals(hits, 0);

			/*
			 * Every observation of a jar below is guarded rather than required.  A gate that
			 * accepted a name without creating it would leave the stat buffer unset, so the
			 * checks cannot simply run; but abandoning the case here would also hide the
			 * refusals further down, and a single run should name every property that broke.
			 */
			if (lstat(jar, &st) == 0) {
				fst_xcheck(S_ISREG(st.st_mode), "the created jar must be a regular file");
				fst_check_int_equals((int) (st.st_mode & 0777), (int) (S_IRUSR | S_IWUSR));
			} else {
				fst_fail("a jar the gate reported usable must exist afterwards");
			}

			/* an ordinary private jar of ours is accepted, and left exactly as it was */
			fst_xc_temp_path(FST_XC_TEMP_COOKIE_KEEP, jar, sizeof(jar));
			fst_requires(fst_xc_write_file(jar, "jar", 3));
			hits = fst_xc_count_jar_log(refusal, jar, url, &accepted);

			fst_xcheck(accepted == 1, "an existing private jar of ours must be usable");
			fst_check_int_equals(hits, 0);

			if (lstat(jar, &st) == 0) {
				fst_check_int_equals((int) st.st_size, 3);
				fst_check_int_equals((int) (st.st_mode & 0777), (int) (S_IRUSR | S_IWUSR));
			} else {
				fst_fail("an existing jar must not be removed by the check that accepted it");
			}

			/* a symlink is refused, it is left in place, and its target keeps its contents */
			fst_xc_temp_path(FST_XC_TEMP_CANARY, canary, sizeof(canary));
			fst_requires(fst_xc_write_file(canary, "untouched", 9));
			fst_xc_temp_path(FST_XC_TEMP_COOKIE_LINK, jar, sizeof(jar));
			fst_requires(symlink(canary, jar) == 0);
			hits = fst_xc_count_jar_log(reason_type, jar, url, &accepted);

			fst_xcheck(accepted == 0, "a jar that is a symlink must be refused");
			fst_check_int_equals(hits, 1);

			if (lstat(jar, &st) == 0) {
				fst_xcheck(S_ISLNK(st.st_mode), "the planted symlink must be left as it was, not replaced by a regular file");
			} else {
				fst_fail("the planted symlink must still be there");
			}

			if ((fd = open(canary, O_RDONLY, 0)) > -1) {
				char buf[64] = "";
				switch_ssize_t got = read(fd, buf, sizeof(buf) - 1);

				close(fd);
				fst_check_int_equals((int) got, 9);
				fst_xcheck(!strcmp(buf, "untouched"), "the symlink target must be untouched");
			} else {
				fst_fail("the symlink target must still be readable");
			}

			/*
			 * The same refusal, twice more: the warning must not carry the token planted in
			 * the URL's userinfo, path and query, and it must still carry the authority --
			 * otherwise a warning that named nothing would satisfy the absence on its own.
			 */
			hits = fst_xc_count_jar_log("j4r-canary", jar, url, &accepted);
			fst_check_int_equals(accepted, 0);
			fst_xcheck(hits == 0, "a refusal must not disclose any part of the gateway URL beyond its authority");

			hits = fst_xc_count_jar_log("127.0.0.1:19012", jar, url, &accepted);
			fst_check_int_equals(accepted, 0);
			fst_xcheck(hits == 1, "a refusal must still say which binding it applies to");

			/*
			 * A directory is refused as an entry in its own right.  It is created private and
			 * inside this case's own directory so that the rule about the directory *holding*
			 * a jar cannot fire first and make this sub-case pass for the wrong reason -- which
			 * is why each refusal below names the rule it expects rather than merely counting
			 * warnings.
			 */
			fst_xc_temp_path(FST_XC_TEMP_COOKIE_DIR, shared, sizeof(shared));
			fst_requires(switch_dir_make(shared, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE | SWITCH_FPROT_UEXECUTE, NULL)
						 == SWITCH_STATUS_SUCCESS);
			hits = fst_xc_count_jar_log(reason_type, shared, url, &accepted);

			fst_xcheck(accepted == 0, "a jar that is a directory must be refused");
			fst_check_int_equals(hits, 1);
			fst_xcheck(rmdir(shared) == 0, "the directory fixture must be removable");

			/* a jar other users can write is refused, however it came to be that way */
			fst_xc_temp_path(FST_XC_TEMP_COOKIE_LOOSE, jar, sizeof(jar));
			fst_requires(fst_xc_write_file(jar, "jar", 3));
			fst_requires(chmod(jar, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) == 0);
			hits = fst_xc_count_jar_log(reason_mode, jar, url, &accepted);

			fst_xcheck(accepted == 0, "a jar other users can write must be refused");
			fst_check_int_equals(hits, 1);

			/*
			 * A jar belonging to somebody else is refused.  Only a run that can create one
			 * can observe this, which is to say a run with an effective uid of zero; the
			 * branch is skipped rather than faked otherwise, because a fake owner would
			 * assert the test's own arithmetic instead of the gate's.
			 */
			if (geteuid() == 0) {
				fst_xc_temp_path(FST_XC_TEMP_COOKIE_ALIEN, jar, sizeof(jar));
				fst_xcheck(fst_xc_write_file(jar, "jar", 3) == 1, "the foreign-owner fixture must be created");
				fst_xcheck(chown(jar, (uid_t) 65534, (gid_t) 65534) == 0, "the foreign-owner fixture must change hands");
				hits = fst_xc_count_jar_log(reason_owner, jar, url, &accepted);

				fst_xcheck(accepted == 0, "a jar belonging to another user must be refused");
				fst_check_int_equals(hits, 1);
			}

			/*
			 * A jar in a directory other users can write without the sticky bit is refused
			 * whatever the entry itself looks like, because there the name can be renamed
			 * away and re-created between the check and libcurl's open.  The directory is
			 * created inside this case's private one, so no other user can actually reach
			 * it; the mode bits alone are what the gate reads, which is what makes the
			 * outcome deterministic.  It is removed here rather than by the teardown sweep,
			 * which removes files and would report a directory as residue -- and would
			 * report it for real if the gate had created the jar inside it after all.
			 */
			fst_requires(switch_dir_make(shared, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE | SWITCH_FPROT_UEXECUTE, NULL)
						 == SWITCH_STATUS_SUCCESS);
			fst_requires(chmod(shared, 0777) == 0);
			switch_snprintf(jar, sizeof(jar), "%s%s%s", shared, SWITCH_PATH_SEPARATOR, FST_XC_TEMP_COOKIE_NEW);
			hits = fst_xc_count_jar_log(reason_dir, jar, url, &accepted);

			fst_xcheck(accepted == 0, "a jar whose directory would let the name be replaced must be refused");
			fst_check_int_equals(hits, 1);
			fst_xcheck(lstat(jar, &st) != 0, "a refused jar must not have been created");
			fst_xcheck(rmdir(shared) == 0, "the shared-directory fixture must be removable, so nothing was created inside it");

			/* the same directory with the sticky bit set is usable again, which is what keeps
			   the rule above from being an outright ban on shared directories */
			fst_requires(switch_dir_make(shared, SWITCH_FPROT_UREAD | SWITCH_FPROT_UWRITE | SWITCH_FPROT_UEXECUTE, NULL)
						 == SWITCH_STATUS_SUCCESS);
			fst_requires(chmod(shared, 01777) == 0);
			hits = fst_xc_count_jar_log(refusal, jar, url, &accepted);

			fst_xcheck(accepted == 1, "a sticky shared directory must not defeat an otherwise private jar");
			fst_check_int_equals(hits, 0);
			fst_xcheck(unlink(jar) == 0, "the jar created in the sticky fixture must be removable");
			fst_xcheck(rmdir(shared) == 0, "the sticky-directory fixture must be removable");

			/*
			 * An empty path is refused without a diagnostic.  The fetch path cannot reach
			 * the gate with one -- the member is only set from a parameter that carried a
			 * value -- so a warning here would be noise about a configuration that does not
			 * exist, and the caller's own guard is what makes the answer observable.
			 */
			hits = fst_xc_count_jar_log(refusal, "", url, &accepted);
			fst_xcheck(accepted == 0, "an empty jar path must be refused");
			fst_check_int_equals(hits, 0);

			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * Declared last: every preceding case's teardown has run by the time this body
		 * executes, so this is where their cumulative result becomes an assertion.
		 *
		 * A teardown block runs outside any test's assertion scope, which is why a failure
		 * there is latched rather than reported in place.  Without this case the latch
		 * would be read only by the setup of a following case, and the last case in the
		 * suite has no follower - so an undeleted file or a logger that would not unbind at
		 * the very end of the run would pass silently.  It touches no module state and
		 * allocates nothing, so it cannot disturb what the case above left behind.
		 */
		FST_TEST_BEGIN(teardown_is_observable)
		{
			char preserved[1024] = "";

			fst_xcheck(fst_xc_teardown_failed == 0, "every case's teardown must have completed: an earlier one reported unfinished cleanup");
			fst_check_int_equals(fst_xc_log_bound, 0);

			/*
			 * And the checked removal itself, exercised directly rather than only through
			 * the teardown block that cannot assert on it.
			 */
			fst_xcheck(*fst_xc_temp_dir != '\0', "the setup block must have created this case's private directory");
			switch_copy_string(preserved, fst_xc_temp_dir, sizeof(preserved));

			fst_xcheck(fst_xc_temp_dir_destroy() == 1, "removing an untouched private directory must report success");
			fst_check_string_equals(fst_xc_temp_dir, "");
			fst_xcheck(switch_directory_exists(preserved, NULL) != SWITCH_STATUS_SUCCESS, "the private directory must actually be gone");
			fst_xcheck(fst_xc_temp_dir_destroy() == 1, "a repeated removal must be a harmless success");
			fst_xcheck(fst_xc_teardown_failed == 0, "a clean removal must not latch a teardown failure");
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 8 -- the fallback event
		 * -------------------------------------------------------------------
		 * The WARNING tells an operator that provisioning fidelity degraded; the
		 * event tells their alerting.  These four cases assert the machine-facing
		 * half: that it fires exactly once per fallback with the right reason on
		 * each edge, that it identifies the binding and the gateway without
		 * carrying a credential, that a binding which never asked for JSON fires
		 * nothing at all, and that an event which could not be built completely is
		 * abandoned rather than delivered with a header missing.
		 *
		 * Every assertion after the consumer is bound is deliberately non-fatal, so
		 * no failure can abandon a case body with the consumer still bound to the
		 * core; each case unbinds it on the way out and the stop is asserted.
		 */

		FST_TEST_BEGIN(fallback_event_content_type_mismatch)
		{
			/* announced as XML against a JSON binding: the decode is abandoned on the
			   Content-Type edge and the document still resolves through the untouched
			   XML parse, which is what makes this a degradation rather than a failure */
			static const char xml_body[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"2000\"></user></section></document>";
			static const char gateway_url[] = "https://provisioner:hunter2@127.0.0.1:1"
				"/tenants/s3cr3t-tenant-token/directory?apikey=deadbeefapikey#frag";
			xml_binding_t binding;
			fst_xc_evt_record_t captured;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());
			fst_requires(fst_xc_evt_capture_start());

			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) gateway_url;
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";
			binding.name = (char *) "provisioning_binding";

			xml = fst_xc_evt_drive_fetch(&binding, "directory", xml_body, "text/xml; charset=utf-8", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_xcheck(fst_xc_evt_fetch_barrier_ok, "the event queue must drain before the event counters are read");

			/* exactly one event, and it pairs one for one with the one WARNING */
			fst_check_int_equals(fst_xc_evt_captured(), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);

			captured = fst_xc_evt_record(0);
			fst_check_string_equals(captured.reason, "content-type-mismatch");
			fst_check_string_equals(captured.binding, "provisioning_binding");

			/*
			 * The gateway is identified by scheme and authority and by nothing else.
			 * Asserted the way the log-hygiene cases assert it: the userinfo password,
			 * the credential-bearing path segment and the query token are all ABSENT
			 * from the header value, and what remains is the authority that names which
			 * gateway degraded -- so "carries nothing" cannot satisfy these checks.
			 */
			fst_check_string_equals(captured.gateway, "https://[redacted]@127.0.0.1:1/[redacted]");
			fst_xcheck(strstr(captured.gateway, "hunter2") == NULL, "the Gateway header must not carry the URL's userinfo password");
			fst_xcheck(strstr(captured.gateway, "s3cr3t-tenant-token") == NULL, "the Gateway header must not carry a URL path segment");
			fst_xcheck(strstr(captured.gateway, "deadbeefapikey") == NULL, "the Gateway header must not carry the URL's query token");
			fst_xcheck(strstr(captured.gateway, "127.0.0.1:1") != NULL, "the Gateway header must still identify the gateway by authority");

			/* and the fallback itself is unchanged: the XML parse ran and resolved the
			   document, so the event observes a degradation rather than causing one */
			fst_xcheck(xml != NULL, "a mismatched content type must still fall back to the XML parse");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "2000");
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * The same edge from a binding the configuration never named.  The header is
			 * the module's single deterministic rendering for that case, so a consumer
			 * never has to tell an absent header from an empty one.
			 */
			binding.name = NULL;

			xml = fst_xc_evt_drive_fetch(&binding, "directory", xml_body, "text/xml", 200);
			fst_xcheck(fst_xc_evt_fetch_barrier_ok, "the event queue must drain before the event counters are read");
			fst_check_int_equals(fst_xc_evt_captured(), 1);
			captured = fst_xc_evt_record(0);
			fst_check_string_equals(captured.binding, "(unnamed)");
			fst_check_string_equals(captured.reason, "content-type-mismatch");
			switch_xml_free(xml);
			xml = NULL;

			fst_xcheck(fst_xc_evt_capture_stop(), "the fallback event consumer must unbind and release cleanly");
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fallback_event_malformed_json)
		{
			/* announced as JSON, and truncated mid-document: the announcement is not
			   trusted over the bytes, so this is the other edge of the taxonomy */
			static const char malformed_body[] = "{\"directory\":{\"user\":{\"@id\":\"1000\"";
			static const char gateway_url[] = "http://provisioner:hunter2@127.0.0.1:1/tenants/s3cr3t-tenant-token/directory";
			xml_binding_t binding;
			fst_xc_evt_record_t captured;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());
			fst_requires(fst_xc_evt_capture_start());

			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) gateway_url;
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.response_format = (char *) "json";
			binding.name = (char *) "provisioning_binding";

			xml = fst_xc_evt_drive_fetch(&binding, "directory", malformed_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_xcheck(fst_xc_evt_fetch_barrier_ok, "the event queue must drain before the event counters are read");

			fst_check_int_equals(fst_xc_evt_captured(), 1);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 1);

			captured = fst_xc_evt_record(0);
			fst_check_string_equals(captured.reason, "malformed-json");
			fst_check_string_equals(captured.binding, "provisioning_binding");
			fst_check_string_equals(captured.gateway, "http://[redacted]@127.0.0.1:1/[redacted]");
			fst_xcheck(strstr(captured.gateway, "hunter2") == NULL, "the Gateway header must not carry the URL's userinfo password");
			fst_xcheck(strstr(captured.gateway, "s3cr3t-tenant-token") == NULL, "the Gateway header must not carry a URL path segment");

			/*
			 * And the body reaches the untouched XML parse, which reports it as the
			 * malformed XML it is rather than failing the fetch: switch_xml_parse_str()
			 * returns a tree carrying an error string instead of NULL, so no terminal
			 * parse error is logged either.
			 */
			fst_xcheck(xml != NULL, "a body that is not JSON must fall back to the XML parse");
			if (xml) {
				fst_xcheck(*switch_xml_error(xml) != '\0', "the parser must report a truncated JSON body as the malformed XML it is");
			}
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			switch_xml_free(xml);
			xml = NULL;

			fst_xcheck(fst_xc_evt_capture_stop(), "the fallback event consumer must unbind and release cleanly");
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fallback_event_absent_on_xml_default)
		{
			/*
			 * The absent-parameter path, end to end through the same fake transport, and
			 * the assertion is a negative one: with no response-format the module fires
			 * NOTHING.  Both sub-cases matter -- an XML answer is the ordinary case, and
			 * a JSON answer proves the silence comes from the binding never having asked
			 * rather than from the response happening to be XML.
			 */
			static const char xml_body[] = "<document type=\"freeswitch/xml\"><section name=\"directory\">"
				"<user id=\"2000\"></user></section></document>";
			static const char json_body[] = "{\"directory\":{\"user\":{\"@id\":\"1000\"}}}";
			xml_binding_t binding;
			switch_xml_t xml = NULL;

			fst_requires(fst_xc_log_capture_start());
			fst_requires(fst_xc_evt_capture_start());

			memset(&binding, 0, sizeof(binding));
			binding.url = (char *) "http://127.0.0.1:1/provision";
			binding.curl_max_bytes = XML_CURL_MAX_BYTES;
			binding.name = (char *) "xml_default_binding";

			/* 1. an XML answer to an XML-default binding: the ordinary run */
			xml = fst_xc_evt_drive_fetch(&binding, "directory", xml_body, "text/xml; charset=utf-8", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_xcheck(fst_xc_evt_fetch_barrier_ok, "the event queue must drain before the event counters are read");
			fst_check_int_equals(fst_xc_evt_captured(), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_PARSE_ERROR), 0);
			fst_xcheck(!fst_xc_transport_sent_header("Accept: application/json"),
					   "a binding that did not ask for JSON must not advertise it");
			fst_xcheck(xml != NULL, "the XML-only path must return the parser's tree unchanged");
			fst_check_string_equals(fst_xc_fetched_user_id(xml), "2000");
			switch_xml_free(xml);
			xml = NULL;

			/* 2. a JSON answer to the same binding: still no decode, so still no event */
			xml = fst_xc_evt_drive_fetch(&binding, "directory", json_body, "application/json", 200);
			fst_xcheck(fst_xc_fetch_barrier_ok, "the log queue must drain before the warning counters are read");
			fst_xcheck(fst_xc_evt_fetch_barrier_ok, "the event queue must drain before the event counters are read");
			fst_check_int_equals(fst_xc_evt_captured(), 0);
			fst_check_int_equals(fst_xc_log_count(FST_XC_LOG_IDX_FALLBACK), 0);
			fst_xcheck(xml != NULL, "the XML-only path must return the parser's tree unchanged");
			if (xml) {
				fst_xcheck(*switch_xml_error(xml) != '\0', "the parser must report a JSON body as the malformed XML it is");
			}
			switch_xml_free(xml);
			xml = NULL;

			/*
			 * The consumer really was listening throughout: the barrier events it saw
			 * prove the binding was live for both fetches, so the two zero counts above
			 * are silence rather than a consumer that was never attached.
			 */
			fst_check_int_equals(fst_xc_evt_bound, 1);

			fst_xcheck(fst_xc_evt_capture_stop(), "the fallback event consumer must unbind and release cleanly");
			fst_xcheck(fst_xc_log_capture_stop(), "the counting logger must unbind and release cleanly");
		}
		FST_TEST_END()

		/*
		 * The construction guard, which is what makes the three headers a contract
		 * rather than an expectation.  switch_event_add_header_string() returns a
		 * status and refuses a value it cannot store, so an unchecked add would let
		 * a two-of-three event reach a consumer that has no way to tell a refused
		 * header from one the module chose not to send.
		 *
		 * The emitter is called directly because no gateway can produce that edge:
		 * the reason token is an internal literal and the other two operands pass
		 * through bounding helpers that always yield a string, so nothing a remote
		 * peer answers with can reach the header API with a value it will refuse.
		 * Driving it from here is the only way the guard is observable at all.
		 *
		 * Two controls sit either side of the negative assertion.  A complete call
		 * must fire exactly one whole event, so "no event" cannot be satisfied by a
		 * consumer that was never listening; and a call whose binding and gateway are
		 * both absent -- rendered, not refused -- must still fire, so the guard is a
		 * refusal of what cannot be built rather than a refusal of anything unusual.
		 */
		FST_TEST_BEGIN(fallback_event_abandoned_when_incomplete)
		{
			static const char gateway_url[] = "https://provisioner:hunter2@127.0.0.1:1"
				"/tenants/s3cr3t-tenant-token/directory?apikey=deadbeefapikey";
			fst_xc_evt_record_t captured;

			fst_requires(fst_xc_evt_capture_start());

			/*
			 * 1. Control: everything the emitter needs is present, so one event
			 *    arrives carrying all three headers -- the same rendering the
			 *    end-to-end cases above assert, reached through the emitter alone.
			 */
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are armed");
			fst_xc_evt_arm();
			xml_curl_json_fire_fallback_event("provisioning_binding", XML_CURL_JSON_FALLBACK_REASON_MALFORMED, gateway_url);
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are read");
			fst_xc_evt_disarm();

			fst_check_int_equals(fst_xc_evt_captured(), 1);
			captured = fst_xc_evt_record(0);
			fst_check_string_equals(captured.binding, "provisioning_binding");
			fst_check_string_equals(captured.reason, "malformed-json");
			fst_check_string_equals(captured.gateway, "https://[redacted]@127.0.0.1:1/[redacted]");

			/*
			 * 2. The guard itself: the reason is the one header value with no
			 *    rendering helper behind it, so a NULL one is refused by the header
			 *    API -- and the answer is silence, not an event carrying Binding and
			 *    Gateway with the reason missing.
			 */
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are armed");
			fst_xc_evt_arm();
			xml_curl_json_fire_fallback_event("provisioning_binding", NULL, gateway_url);
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are read");
			fst_xc_evt_disarm();

			fst_check_int_equals(fst_xc_evt_captured(), 0);

			/*
			 * 3. And the guard is precise.  An unnamed binding and an absent gateway
			 *    URL are both rendered to their deterministic markers rather than
			 *    refused, so that event is complete and must still be delivered.  A
			 *    blanket "abandon anything with an empty operand" would fail here.
			 */
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are armed");
			fst_xc_evt_arm();
			xml_curl_json_fire_fallback_event(NULL, XML_CURL_JSON_FALLBACK_REASON_CONTENT_TYPE, NULL);
			fst_xcheck(fst_xc_evt_barrier(), "the event queue must drain before the counters are read");
			fst_xc_evt_disarm();

			fst_check_int_equals(fst_xc_evt_captured(), 1);
			captured = fst_xc_evt_record(0);
			fst_check_string_equals(captured.binding, "(unnamed)");
			fst_check_string_equals(captured.reason, "content-type-mismatch");
			fst_check_string_equals(captured.gateway, "(none)");

			fst_xcheck(fst_xc_evt_capture_stop(), "the fallback event consumer must unbind and release cleanly");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_configuration_load_width_ceiling)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				cJSON *loads = NULL;

				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_load_width_ceiling.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);

				/*
				 * The width this pair stands on, asserted on the parsed document
				 * before it is released.  Parity alone cannot see it: two twins
				 * that had both shrunk to a handful of children would still
				 * serialise identically, and the pair would have stopped being a
				 * boundary case without failing.  The array is required to be
				 * exactly as wide as the ceiling the module enforces, and its two
				 * endpoints are named so a truncated or re-based run is caught as
				 * well as a short one.
				 */
				loads = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(fixture, "configuration"),
																				   "configuration"),
															   "modules"),
											"load");
				fst_requires(loads != NULL);
				fst_check(cJSON_IsArray(loads));
				fst_check_int_equals(cJSON_GetArraySize(loads), XML_CURL_JSON_MAX_ARRAY_ELEMENTS);
				fst_check_string_equals(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(loads, 0), "@module")),
										"mod_load_000");
				fst_check_string_equals(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(loads,
																								   XML_CURL_JSON_MAX_ARRAY_ELEMENTS - 1),
																				"@module")),
										"mod_load_255");

				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("configuration_load_width_ceiling", "configuration", &from_json, &from_xml,
											xml_error, sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* the declared width ceiling boundary: 256 repeated children under one parent */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * And the width survives the translation, which is the half of the
			 * claim the document above cannot make: every one of the 256 array
			 * elements has to become its own element under the one parent, in
			 * order, with none coalesced and none dropped.
			 */
			fst_check_int_equals(fst_xc_count_occurrences(from_json, "<load module="), XML_CURL_JSON_MAX_ARRAY_ELEMENTS);
			fst_check_int_equals(fst_xc_count_occurrences(from_xml, "<load module="), XML_CURL_JSON_MAX_ARRAY_ELEMENTS);
			fst_check_string_has(from_json, "<load module=\"mod_load_000\"></load>");
			fst_check_string_has(from_json, "<load module=\"mod_load_255\"></load>");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_configuration_attribute_only)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char *raw = NULL;
			char xml_error[256] = "";

			/*
			 * "Attribute-only" is a claim about the whole document, so it is made
			 * about the whole document: the bytes on disk carry no text member at
			 * all.  A "$" member is spelled exactly that way in JSON, and no value
			 * anywhere in this fixture contains that sequence, so its absence is
			 * an exact reading rather than an approximation.
			 */
			raw = xml_curl_json_read_file(FST_XC_FIXTURE_DIR "configuration_attribute_only.json", XML_CURL_MAX_BYTES);
			fst_requires(raw != NULL);
			fst_check_string_does_not_have(raw, "\"$\"");
			switch_safe_free(raw);

			{
				cJSON *layout = NULL;

				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_attribute_only.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);

				/*
				 * And the construct itself, on the element the pair was written
				 * around: every member an "@" carrying a string, so the element has
				 * neither text nor a child.  Parity cannot see this - two twins that
				 * had both grown a text member would still compare equal - so the
				 * structure is read directly, before the document is released.
				 */
				layout = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(fixture,
																																			   "configuration"),
																													   "configuration"),
																			   "layout-settings"),
													   "layouts"),
											   "layout"),
											0);
				fst_requires(layout != NULL);
				fst_xcheck(fst_xc_json_attribute_only(cJSON_GetObjectItem(layout, "image")),
						   "the image element must carry nothing but @-prefixed string members");
				fst_check_int_equals(cJSON_GetArraySize(cJSON_GetObjectItem(layout, "image")), 4);

				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("configuration_attribute_only", "configuration", &from_json, &from_xml,
											xml_error, sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* attribute-only elements: every member an @, no $ and no children, in non-alphabetical order */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * What the translation has to preserve: each "@" member becomes an
			 * attribute in the order it was written - not alphabetised - and an
			 * element with no text member serialises with empty content rather than
			 * borrowing text from anywhere.
			 */
			fst_check_string_has(from_json, "<image x=\"0\" y=\"0\" scale=\"360\" floor=\"true\"></image>");
			fst_check_string_has(from_json, "<layout name=\"2x1\" auto-3d-position=\"true\">");
			fst_check_int_equals(fst_xc_count_occurrences(from_json, "<image "), 5);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_configuration_unicode_escapes)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char *raw = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "configuration_unicode_escapes.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			/*
			 * The JSON twin has to be the side that carries ESCAPES rather than
			 * decoded characters, or the decoded-value assertions below would prove
			 * nothing about the escape path.  The production reader hands back
			 * exactly the bytes on disk, so a plain escape, a three-byte one and the
			 * surrogate pair are all checked for there first.
			 */
			raw = xml_curl_json_read_file(FST_XC_FIXTURE_DIR "configuration_unicode_escapes.json", XML_CURL_MAX_BYTES);
			fst_requires(raw != NULL);
			fst_check_string_has(raw, "\\u00ea");
			fst_check_string_has(raw, "\\u2122");
			fst_check_string_has(raw, "\\u89c6\\u9891\\u4f1a\\u8bae");
			fst_check_string_has(raw, "\\ud83d\\ude42");
			switch_safe_free(raw);

			fst_requires(fst_xc_render_pair("configuration_unicode_escapes", "configuration", &from_json, &from_xml,
											xml_error, sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* \uXXXX escapes, one of them a surrogate pair, surviving as the decoded character */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * Equality alone would also hold if both sides had passed an escape
			 * through verbatim, so the decoded values are asserted directly.  Each
			 * non-ASCII code point comes back as the numeric character reference
			 * switch_xml_toxml() emits for it, the surrogate pair comes back as ONE
			 * reference for U+1F642 rather than as its two halves, an escaped
			 * ampersand comes back as the entity and not as a raw "&", and no
			 * literal escape survives anywhere in the output.
			 */
			fst_check_string_has(from_json, "Confer&#xEA;ncia FreeSWITCH&#x2122; &#x1F642;");
			fst_check_string_has(from_json, "&#x89C6;&#x9891;&#x4F1A;&#x8BAE;");
			fst_check_string_has(from_json, "Atenci&#xF3;n &amp; Ventas");
			fst_check_string_does_not_have(from_json, "\\u");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * GROUP 1b -- three further paired directory fixtures
		 * -------------------------------------------------------------------
		 * The same acceptance test as GROUP 1, applied to three constructs the
		 * original nine do not reach: a node carrying attributes and text at
		 * once beside sibling elements, nesting one level inside the depth
		 * ceiling the module enforces, and the empty-element form.  Each case
		 * proves its construct explicitly as well as comparing the two
		 * serialisations, so an equal comparison of two identically wrong
		 * strings cannot pass for parity.
		 */

		FST_TEST_BEGIN(fixture_parity_directory_mixed_text_and_siblings)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_mixed_text_and_siblings.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_mixed_text_and_siblings", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* a node carrying both @ attributes and $ text, standing beside sibling elements */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * The construct itself: <name> keeps its declared attribute order
			 * and its text, its parent <contact> carries an attribute of its
			 * own alongside children, and both siblings survive in document
			 * order.  The mix is attributes plus text on the leaf and
			 * attributes plus children on the parent -- never text AND children
			 * on one element, which duplicate_members_and_mixed_content_rejected
			 * pins as refused and which no fixture may therefore contain.
			 */
			fst_check_string_has(from_json, "<name source=\"crm\" locale=\"en-US\">Ana Fernandez</name>");
			fst_check_string_has(from_json, "<contact type=\"voice\">");
			fst_check_string_has(from_json, "<city>Tulsa</city>");
			fst_check_string_has(from_json, "<state>OK</state>");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_directory_nesting_boundary)
		{
			/*
			 * Interpretation I-3: the requested "CJSON_NESTING_LIMIT minus
			 * one" does not describe this tree, so the depth this pair stands
			 * on cannot be reconstructed from the request alone.
			 * CJSON_NESTING_LIMIT bounds cJSON's own parser recursion rather
			 * than this decoder, and it is not even a single value here:
			 * configure.ac defines it as 64 for the autotools build, while
			 * switch_cJSON.h falls back to 1000 for any build that leaves it
			 * undefined.  Neither number ever decides anything, because the
			 * module refuses first at its own XML_CURL_JSON_MAX_DEPTH -- 32 --
			 * which it enforces itself on every platform precisely so that the
			 * parser's configuration cannot move the boundary.  32 is therefore
			 * the enforceable ceiling, and this fixture pair uses that boundary
			 * minus one: depth 31.  The assertions below pin that choice from
			 * both sides.
			 *
			 * The depth in this pair is only meaningful against the accounting
			 * that produces it, so both are stated here.
			 *
			 * Two gates charge nesting.  The lexical gate counts every '{' and
			 * every '[' it walks and refuses the one that would exceed
			 * XML_CURL_JSON_MAX_DEPTH; the translator charges one level per
			 * recursive call, with <section> as level 1.  For a chain of
			 * single-child objects -- no array, so no bracket that is not also
			 * an element -- the lexical count is the tighter of the two by
			 * exactly one, and it coincides with the XML element depth of the
			 * twin counting <document> as 1 and <section> as 2.
			 *
			 * XML_CURL_JSON_MAX_DEPTH is therefore the last element depth the
			 * decoder accepts, and one more is refused; both are asserted below
			 * so the number this pair stands on is a boundary rather than an
			 * arbitrary depth.  The pair itself sits one level inside it.
			 */
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";
			switch_stream_handle_t stream = { 0 };
			const char *p = NULL;
			int depth = 0;
			int deepest = 0;
			int i;

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_nesting_boundary.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_nesting_boundary", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* nesting one level inside the depth ceiling the module enforces */
			fst_check_string_equals(from_json, from_xml);

			/*
			 * The achieved depth, measured rather than assumed.  Every element
			 * serialises as <name></name> and never as <name/>, and both
			 * character data and attribute values are ampersand-encoded, so an
			 * unescaped '<' is always a tag: "</" closes one and anything else
			 * opens one.  Counting them is therefore an exact element depth.
			 */
			for (p = from_json; *p; p++) {
				if (*p != '<') {
					continue;
				}
				if (p[1] == '/') {
					depth--;
					continue;
				}
				if (++depth > deepest) {
					deepest = depth;
				}
			}
			fst_check_int_equals(deepest, XML_CURL_JSON_MAX_DEPTH - 1);
			/* and the deepest element is the marker the fixture places there */
			fst_check_string_has(from_json, "<nest depth=\"30\">");
			fst_check_string_has(from_json, "<variable name=\"deepest\" value=\"depth-31\"></variable>");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);

			/*
			 * The ceiling from both sides.  A chain whose deepest element sits
			 * exactly at XML_CURL_JSON_MAX_DEPTH is still accepted, and one
			 * level deeper is refused -- so the pair above really is at the
			 * boundary minus one.  The chain length is two short of the depth
			 * because the payload root and the <section> object account for the
			 * first two levels.
			 */
			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH - 2; i++) {
				stream.write_function(&stream, "%s", "{\"nest\":");
			}
			stream.write_function(&stream, "%s", "{}");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH - 2; i++) {
				stream.write_function(&stream, "%s", "}");
			}
			stream.write_function(&stream, "%s", "}");
			fst_xcheck(fst_xc_accepts((const char *) stream.data, "directory"),
					   "a document whose deepest element sits at the ceiling must still be accepted");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			stream.write_function(&stream, "%s", "{\"directory\":");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH - 1; i++) {
				stream.write_function(&stream, "%s", "{\"nest\":");
			}
			stream.write_function(&stream, "%s", "{}");
			for (i = 0; i < XML_CURL_JSON_MAX_DEPTH - 1; i++) {
				stream.write_function(&stream, "%s", "}");
			}
			stream.write_function(&stream, "%s", "}");
			fst_xcheck(fst_xc_rejects((const char *) stream.data, "directory"),
					   "one level deeper than the ceiling must be refused");
			switch_safe_free(stream.data);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_directory_empty_elements)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "directory_empty_elements.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("directory_empty_elements", "directory", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* empty elements, written self-closing in the XML twin and as {} in the JSON twin */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * Both sides normalise to <name></name>.  The XML twin's <params/>
			 * is parsed into a child carrying an empty but non-NULL text
			 * pointer and the JSON twin's {} is built into one, and the
			 * serializer decides on those pointers alone -- so neither
			 * serialisation can carry a self-closing tag anywhere, with
			 * attributes or without.
			 */
			fst_check_string_has(from_json, "<params></params>");
			fst_check_string_has(from_json, "<users></users>");
			fst_check_string_has(from_json, "<user id=\"1005\" type=\"pointer\"></user>");
			fst_check_string_does_not_have(from_json, "/>");
			fst_check_string_does_not_have(from_xml, "/>");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		/*
		 * Three further paired fixtures for the dialplan section, held to exactly the
		 * acceptance test GROUP 1 applies: the JSON document goes through the
		 * production reader and translator, its XML twin through the simple parser,
		 * and the two switch_xml_toxml(x, SWITCH_FALSE) serialisations must be
		 * byte-identical.  What they add is construct combinations the original nine
		 * do not reach -- repeated-children arrays nested three levels deep, an
		 * attribute-only document whose values arrive as \uXXXX escapes, and empty
		 * elements standing beside a node that carries attributes and text at once.
		 * Widths and depths stay ordinary here on purpose: the width and nesting
		 * ceilings are boundary cases of the configuration and directory pairs, and a
		 * pair that sat on a boundary would stop being a test of the construct.
		 */

		FST_TEST_BEGIN(fixture_parity_dialplan_nested_arrays)
		{
			/* the shape this pair is written around: three extensions, each with
			   three conditions, each with three actions */
			static const int wide = 3;
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				cJSON *extensions = NULL;
				cJSON *extension = NULL;
				cJSON *conditions = NULL;
				cJSON *condition = NULL;
				int outer = 0;
				int inner = 0;

				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_nested_arrays.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);

				/*
				 * Arrays at all three levels, asserted on the parsed document.
				 * Parity cannot distinguish an array of one from a lone object -
				 * both translate to a single child - so a fixture that had lost its
				 * nesting would compare equal to a twin that had lost it too.  Every
				 * level is walked rather than sampled, because the construct is
				 * arrays INSIDE arrays and one conforming branch says nothing about
				 * the others.
				 */
				extensions = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(fixture, "dialplan"), "context"),
												 "extension");
				fst_requires(extensions != NULL);
				fst_check(cJSON_IsArray(extensions));
				fst_check_int_equals(cJSON_GetArraySize(extensions), wide);

				for (outer = 0; outer < cJSON_GetArraySize(extensions); outer++) {
					extension = cJSON_GetArrayItem(extensions, outer);
					conditions = cJSON_GetObjectItem(extension, "condition");
					fst_xcheck(cJSON_IsArray(conditions), "every extension must carry an ARRAY of conditions");
					fst_check_int_equals(cJSON_GetArraySize(conditions), wide);

					for (inner = 0; inner < cJSON_GetArraySize(conditions); inner++) {
						condition = cJSON_GetArrayItem(conditions, inner);
						fst_xcheck(cJSON_IsArray(cJSON_GetObjectItem(condition, "action")),
								   "every condition must carry an ARRAY of actions");
						fst_check_int_equals(cJSON_GetArraySize(cJSON_GetObjectItem(condition, "action")), wide);
					}
				}

				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("dialplan_nested_arrays", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* three levels of repeated-children arrays nested inside one another: an array
			   of extensions, each holding an array of conditions, each holding an array of
			   actions */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * And the product of those widths in the output: 3 extensions, 3 x 3
			 * conditions and 3 x 3 x 3 actions, which is what proves each array was
			 * expanded in its own parent rather than flattened into one run.
			 */
			fst_check_int_equals(fst_xc_count_occurrences(from_json, "<extension "), wide);
			fst_check_int_equals(fst_xc_count_occurrences(from_json, "<condition "), wide * wide);
			fst_check_int_equals(fst_xc_count_occurrences(from_json, "<action "), wide * wide * wide);
			fst_check_int_equals(fst_xc_count_occurrences(from_xml, "<action "), wide * wide * wide);

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_dialplan_attribute_only_unicode)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char *raw = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_attribute_only_unicode.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			/*
			 * The JSON twin has to be the side carrying escapes rather than decoded
			 * characters, or the decoded-value assertions below would prove nothing about
			 * the escape path.  The production reader hands back exactly the bytes on
			 * disk, so both a plain escape and the surrogate pair are checked for there
			 * first.
			 */
			raw = xml_curl_json_read_file(FST_XC_FIXTURE_DIR "dialplan_attribute_only_unicode.json", XML_CURL_MAX_BYTES);
			fst_requires(raw != NULL);
			fst_check_string_has(raw, "\\u00f3");
			fst_check_string_has(raw, "\\ud842\\udfb7");
			switch_safe_free(raw);

			fst_requires(fst_xc_render_pair("dialplan_attribute_only_unicode", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* attribute-only elements throughout -- not one "$" member in the document --
			   whose values arrive as \uXXXX escapes on the JSON side and as the decoded
			   characters on the XML side */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * Equality alone would also hold if both sides had passed an escape through
			 * verbatim, so the decoded values are asserted directly.  Each one comes back
			 * as the numeric character reference switch_xml_toxml() emits for a non-ASCII
			 * code point, and the surrogate pair comes back as ONE reference for U+20BB7
			 * rather than as its two halves -- which is the round trip this pair exists to
			 * prove.  No literal escape survives anywhere in the output.
			 */
			fst_check_string_has(from_json, "Atenci&#xF3;n al Cliente");
			fst_check_string_has(from_json, "Kundendienst M&#xFC;ller");
			fst_check_string_has(from_json, "call_rate_currency=&#x20AC;");
			fst_check_string_has(from_json, "&#x20BB7;&#x7530; &#x592A;&#x90CE;");
			fst_check_string_does_not_have(from_json, "\\u");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(fixture_parity_dialplan_empty_and_mixed)
		{
			char *from_json = NULL;
			char *from_xml = NULL;
			char xml_error[256] = "";

			{
				fst_parse_json_file(fixture, FST_XC_FIXTURE_DIR "dialplan_empty_and_mixed.json");
				fst_check(cJSON_IsObject(fixture));
				fst_check_int_equals(cJSON_GetArraySize(fixture), 1);
				cJSON_Delete(fixture);
			}

			fst_requires(fst_xc_render_pair("dialplan_empty_and_mixed", "dialplan", &from_json, &from_xml, xml_error,
											sizeof(xml_error)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(xml_error, "");
			/* empty elements with attributes and without any -- including one written
			   self-closing and expressed as an empty object in an array -- beside a node
			   carrying attributes and text at once among its siblings */
			fst_check_string_equals(from_json, from_xml);
			/*
			 * What the pair pins down beyond equality: the self-closing form survives
			 * nowhere.  An empty element normalises to <name></name> whichever way its
			 * tree was built and whether or not it carries attributes, and the mixed node
			 * keeps its attributes and its text side by side while its childless siblings
			 * keep theirs.
			 */
			fst_check_string_has(from_json, "<action application=\"answer\"></action>");
			fst_check_string_has(from_json, "<condition></condition>");
			fst_check_string_has(from_json, "<anti-action application=\"start_dtmf\"></anti-action>");
			fst_check_string_has(from_json, "<action application=\"playback\">tone_stream://%(2000,4000,440,480)</action>");
			fst_check_string_has(from_json, "<action application=\"export\">sip_secure_media=true</action>");
			fst_check_string_does_not_have(from_json, "/>");

			switch_safe_free(from_json);
			switch_safe_free(from_xml);
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * THE CORPUS CARRIES NO PREPROCESSOR TOKEN
		 * -------------------------------------------------------------------
		 * A `$${var}' token is expanded by the XML decode path and NOT by this
		 * one: switch_xml_parse_file() runs the configuration preprocessor over
		 * the body it parses, while the translator builds the tree straight
		 * through the builder API and copies every value verbatim.  A fixture
		 * pair carrying such a token therefore claims an identity it does not
		 * have: the two sides agree here, where the XML twin is read with
		 * switch_xml_parse_file_simple() and nothing is expanded, and disagree in
		 * a live switch whose global is set -- or, just as bad, whose global is
		 * UNSET, since the preprocessor then writes nothing at all where the
		 * translator writes the literal token.
		 *
		 * The corpus is the reference a gateway author reads, so the rule is kept
		 * rather than merely stated in the runbook: both halves of every shipped
		 * pair are scanned, and the scan is required to have covered them all so
		 * that an unreadable directory cannot pass for a clean corpus.  The
		 * badgerfish_invalid_* counter-examples are excluded deliberately -- they
		 * exist to be refused, and what they contain past the rule they violate
		 * is not part of the contract.
		 */
		FST_TEST_BEGIN(fixture_corpus_carries_no_preprocessor_tokens)
		{
			switch_dir_t *dir = NULL;
			char entry[512] = "";
			char path[1024] = "";
			const char *found = NULL;
			char *bytes = NULL;
			int scanned = 0;
			int offenders = 0;

			fst_requires(switch_dir_open(&dir, FST_XC_FIXTURE_DIR, fst_pool) == SWITCH_STATUS_SUCCESS);

			while ((found = switch_dir_next_file(dir, entry, sizeof(entry)))) {
				if (!fst_xc_val_is_corpus_fixture(found)) {
					continue;
				}

				switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, found);

				/* the production reader, so an unreadable fixture fails here exactly as it
				   would fail a fetch rather than being silently skipped */
				if (!(bytes = xml_curl_json_read_file(path, XML_CURL_MAX_BYTES))) {
					offenders++;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "test_mod_xml_curl: corpus fixture [%s] could not be read\n", found);
					continue;
				}

				scanned++;

				if (strstr(bytes, "$${")) {
					offenders++;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "test_mod_xml_curl: corpus fixture [%s] carries a $${...} preprocessor token, which only the XML "
									  "decode path expands\n", found);
				}

				switch_safe_free(bytes);
			}

			switch_dir_close(dir);

			fst_xcheck(offenders == 0,
					   "no fixture of the conformant corpus may carry a $${...} token, because the JSON path copies it verbatim while the XML path expands it");
			fst_xcheck(scanned == 2 * FST_XC_VAL_MIN_PARITY_JSON,
					   "both halves of all eighteen shipped parity pairs must have been scanned");
		}
		FST_TEST_END()

		/*
		 * -------------------------------------------------------------------
		 * THE STANDALONE CONTRACT VALIDATOR, EXERCISED AS A BLACK BOX
		 * -------------------------------------------------------------------
		 * tools/validate_badgerfish.py is what a backend team runs against its
		 * own gateway's output to prove BadgerFish conformance before
		 * deployment.  It is not linked into anything and shares no code with
		 * the module, so the only honest way to assert it is the way that team
		 * will use it: run the script through a shell and read its exit status.
		 *
		 * Two halves, one per direction of the contract.  The conformant half
		 * sweeps every parity fixture actually present and requires a clean
		 * verdict across all of them at once.  The non-conformant half runs the
		 * validator once per committed counter-example and requires each to be
		 * refused individually, so a validator that had stopped detecting one
		 * rule could not hide behind another rule still firing.
		 *
		 * Being the last declared case, its own teardown has no follower to
		 * observe it.  That is contained rather than ignored: the case writes
		 * nothing into the private per-case directory and binds no logger, so
		 * its teardown is the removal of an untouched directory - exactly the
		 * operation the case above it has just proved reports success - and the
		 * cumulative latch from every earlier case is asserted below before
		 * anything else happens, so nothing that ran before this point goes
		 * unchecked.
		 */
		FST_TEST_BEGIN(validator_contract_matrix)
		{
			switch_stream_handle_t stream = { 0 };
			switch_memory_pool_t *pool = NULL;
			switch_dir_t *dir = NULL;
			char entry[512] = "";
			char path[1024] = "";
			const char *found = NULL;
			int parity_seen[switch_arraylen(fst_xc_val_required_parity_json)];
			int parity_files = 0;
			int invalid_files = 0;
			int quoting_failures = 0;
			int missing_parity = 0;
			int missing_samples = 0;
			int slot = -1;
			int i = 0;
			int code = -1;

			memset(parity_seen, 0, sizeof(parity_seen));

			/* Everything before this case is accounted for, including the
			   teardown of the case that asserted the cumulative latch. */
			fst_xcheck(fst_xc_teardown_failed == 0, "every earlier case's teardown must have completed before the validator case runs");

			/*
			 * A shell has to be available before system() means anything.
			 * system(NULL) is the sanctioned probe for that and returns
			 * non-zero when a command interpreter is present.
			 */
			fst_xcheck(system(NULL) != 0, "no command interpreter is available, so the validator cannot be executed at all");

			/*
			 * The validator has to be where the build says it is.  Checked
			 * before anything is run so that a missing script is reported as a
			 * missing script rather than as a shell exit code of 127.
			 */
			fst_xcheck(switch_file_exists(FST_XC_VAL_TOOL, NULL) == SWITCH_STATUS_SUCCESS,
					   "the contract validator is missing from " FST_XC_VAL_TOOL);

			/*
			 * python3 has to be available.  This is asserted rather than used
			 * as a reason to skip: a run in which the validator was never
			 * executed must not be indistinguishable from a run in which it
			 * passed.
			 */
			code = fst_xc_val_run(FST_XC_VAL_PYTHON " --version >/dev/null 2>&1");
			fst_xcheck(code == 0, "python3 is not available on this host, so the contract validator cannot be exercised");

			/*
			 * The quoting every command below is built with, asserted directly
			 * rather than trusted.  This is the one property that cannot be
			 * observed from an exit code - a mis-quoted word and an unterminated
			 * quote can both come back as the same status - so the produced word
			 * is compared with the POSIX form byte for byte: an apostrophe closes
			 * the quoted run, is emitted escaped, and a new run opens.
			 */
			SWITCH_STANDARD_STREAM(stream);
			fst_xcheck(fst_xc_val_append_word(&stream, "a'b") == 1, "a word carrying an apostrophe must still be quotable");
			fst_xcheck(stream.data && !strcmp((const char *) stream.data, " 'a'\\''b'"),
					   "a word carrying an apostrophe must be quoted as the POSIX '\\'' form");
			switch_safe_free(stream.data);

			SWITCH_STANDARD_STREAM(stream);
			fst_xcheck(fst_xc_val_append_word(&stream, "") == 0, "an empty word must be refused rather than emitted as an empty argument");
			switch_safe_free(stream.data);

			/*
			 * And the validator itself has to be runnable and self-describing.
			 * --help exits 0, which also proves the interpreter accepts the
			 * script - a syntax error would surface here rather than as a
			 * confusing conformance verdict below.
			 *
			 * Every command from here on is BUILT rather than written as a
			 * literal, because the interpreter and the tool path are words the
			 * shell has to receive whole just as much as the arguments are.
			 */
			SWITCH_STANDARD_STREAM(stream);

			if (fst_xc_val_begin_command(&stream)) {
				stream.write_function(&stream, "%s", " --help >/dev/null 2>&1");
				code = fst_xc_val_run((const char *) stream.data);
			} else {
				quoting_failures++;
				code = -1;
			}

			switch_safe_free(stream.data);

			fst_xcheck(quoting_failures == 0, "the validator command could not be built as quoted shell words");
			fst_xcheck(code == 0, "the contract validator must document its own usage with --help and exit 0");

			/*
			 * A path that names nothing must be reported as a usage or I/O
			 * error, NOT as a conformance failure - otherwise a mistyped path
			 * in a deployment gate would read as a refused payload.
			 */
			SWITCH_STANDARD_STREAM(stream);

			if (fst_xc_val_begin_command(&stream)
				&& fst_xc_val_append_word(&stream, FST_XC_FIXTURE_DIR "no_such_response.json")) {
				stream.write_function(&stream, "%s", " >/dev/null 2>&1");
				code = fst_xc_val_run((const char *) stream.data);
			} else {
				quoting_failures++;
				code = -1;
			}

			switch_safe_free(stream.data);

			fst_xcheck(quoting_failures == 0, "the validator command could not be built as quoted shell words");
			fst_xcheck(code == 2, "a missing input must exit 2, so it is never mistaken for a conformance verdict");

			/*
			 * The response ceiling the module enforces before it parses anything
			 * (XML_CURL_MAX_BYTES, per-binding response-max-bytes) is part of the
			 * contract the validator certifies, so its override is exercised
			 * against a real fixture: a ceiling of one byte cannot be met by any
			 * document, so a conformant fixture must be REFUSED under it.  This
			 * needs no oversized fixture of its own, which is the point - the
			 * boundary is asserted without committing a megabyte of padding.
			 */
			SWITCH_STANDARD_STREAM(stream);

			if (fst_xc_val_begin_command(&stream)
				&& fst_xc_val_append_word(&stream, "--max-response-bytes")
				&& fst_xc_val_append_word(&stream, "1")
				&& fst_xc_val_append_word(&stream, FST_XC_FIXTURE_DIR "configuration_acl.json")) {
				stream.write_function(&stream, "%s", " >/dev/null 2>&1");
				code = fst_xc_val_run((const char *) stream.data);
			} else {
				quoting_failures++;
				code = -1;
			}

			switch_safe_free(stream.data);

			fst_xcheck(quoting_failures == 0, "the validator command could not be built as quoted shell words");
			fst_xcheck(code == 1, "a document larger than the response ceiling must be refused with exit 1, as mod_xml_curl refuses it before parsing");

			/*
			 * And the same fixture has to PASS at the default ceiling, or the
			 * assertion above would be satisfied by a validator that refuses
			 * everything.
			 */
			SWITCH_STANDARD_STREAM(stream);

			if (fst_xc_val_begin_command(&stream)
				&& fst_xc_val_append_word(&stream, FST_XC_FIXTURE_DIR "configuration_acl.json")) {
				stream.write_function(&stream, "%s", " >/dev/null 2>&1");
				code = fst_xc_val_run((const char *) stream.data);
			} else {
				quoting_failures++;
				code = -1;
			}

			switch_safe_free(stream.data);

			fst_xcheck(quoting_failures == 0, "the validator command could not be built as quoted shell words");
			fst_xcheck(code == 0, "the same fixture must conform at the default response ceiling");

			/*
			 * One pool for both halves' directory enumerations.  Nothing has
			 * been allocated yet, so this is the one place a fatal requirement
			 * can be used without putting a release out of reach: from here on
			 * every assertion is non-fatal, so control always reaches the
			 * matching release below however the case turns out.
			 */
			fst_requires(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS);

			/*
			 * ---------------------------------------------------------------
			 * HALF ONE: every conformant parity fixture, in a single run
			 * ---------------------------------------------------------------
			 * The corpus is enumerated AND named.  Enumerating it is what runs
			 * the validator over whatever is actually present, so an added pair
			 * is covered the moment it lands; naming it is what makes the
			 * eighteen shipped pairs a requirement rather than a coincidence,
			 * so a deleted pair is reported by name instead of being absorbed
			 * by a count that some other file happened to satisfy.
			 */
			if (switch_dir_open(&dir, FST_XC_FIXTURE_DIR, pool) == SWITCH_STATUS_SUCCESS) {
				SWITCH_STANDARD_STREAM(stream);

				if (!fst_xc_val_begin_command(&stream)) {
					quoting_failures++;
				}

				while ((found = switch_dir_next_file(dir, entry, sizeof(entry)))) {
					if (!fst_xc_val_is_parity_json(found)) {
						continue;
					}

					parity_files++;

					slot = fst_xc_val_index_of(fst_xc_val_required_parity_json,
											   switch_arraylen(fst_xc_val_required_parity_json), found);

					if (slot >= 0) {
						parity_seen[slot] = 1;
					}

					switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, found);

					if (!fst_xc_val_append_word(&stream, path)) {
						quoting_failures++;
					}
				}

				switch_dir_close(dir);
				dir = NULL;

				/* stdout carries only the summary; the verdict is the exit
				   status, and stderr is deliberately left attached so that an
				   unexpected violation names itself in the test log. */
				stream.write_function(&stream, "%s", " >/dev/null");
				code = fst_xc_val_run((const char *) stream.data);
				switch_safe_free(stream.data);

				/* Which of the required pairs were not in the run, named one by
				   one: "the corpus is short by two" is not an actionable
				   diagnostic and this is. */
				for (i = 0; i < (int) switch_arraylen(fst_xc_val_required_parity_json); i++) {
					if (!parity_seen[i]) {
						missing_parity++;
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
										  "test_mod_xml_curl: required conformant fixture [%s] is missing from %s\n",
										  fst_xc_val_required_parity_json[i], FST_XC_FIXTURE_DIR);
					}
				}

				fst_xcheck(quoting_failures == 0, "a fixture path could not be passed to the validator as a single shell word");
				fst_xcheck(missing_parity == 0, "every named conformant parity fixture must be present and included in the validator run");
				fst_xcheck(parity_files >= FST_XC_VAL_MIN_PARITY_JSON,
						   "the conformant corpus is smaller than the eighteen parity fixtures this suite ships");
				fst_xcheck(code == 0, "the validator must exit 0 across every conformant parity JSON fixture present");
			} else {
				fst_fail("the fixtures directory could not be enumerated for the conformant sweep");
			}

			/*
			 * ---------------------------------------------------------------
			 * HALF TWO: each counter-example the contract names, one run apiece
			 * ---------------------------------------------------------------
			 * One run per sample is the point: a single run over all five would
			 * exit 1 even if only one of them were still being detected.
			 *
			 * The five are driven from the named list rather than from whatever
			 * the directory happens to hold, so each of the five rules is
			 * required to still be detected individually.  Its absence is
			 * reported as an absence - checked before the run, so a deleted
			 * sample reads as "missing" rather than as the validator's exit 2
			 * for an unopenable input.
			 */
			for (i = 0; i < (int) switch_arraylen(fst_xc_val_required_invalid_json); i++) {
				switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, fst_xc_val_required_invalid_json[i]);

				if (switch_file_exists(path, NULL) != SWITCH_STATUS_SUCCESS) {
					missing_samples++;
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "test_mod_xml_curl: required non-conformant sample [%s] is missing from %s\n",
									  fst_xc_val_required_invalid_json[i], FST_XC_FIXTURE_DIR);
					continue;
				}

				invalid_files++;

				/*
				 * Both streams are discarded here, unlike the sweep above: these
				 * files are MEANT to be refused, and a passing run must not
				 * spray the test log with the named violations of deliberate
				 * counter-examples.  An unexpected verdict is logged explicitly
				 * instead, naming the file and the code.
				 */
				code = fst_xc_val_run_one(path, 1);

				if (code == FST_XC_VAL_UNQUOTABLE) {
					quoting_failures++;
					continue;
				}

				if (code != 1) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "test_mod_xml_curl: validator returned %d for non-conformant sample [%s]\n", code,
									  fst_xc_val_required_invalid_json[i]);
				}

				fst_xcheck(code == 1, "each non-conformant sample the contract names must be refused with exit 1, on its own");
			}

			fst_xcheck(missing_samples == 0, "every non-conformant sample the contract names must be present in the fixtures directory");
			fst_xcheck(invalid_files == (int) switch_arraylen(fst_xc_val_required_invalid_json),
					   "all five named non-conformant samples must have been exercised");

			/*
			 * Anything else committed under the same prefix is exercised too, so
			 * that a sixth counter-example added later is covered without this
			 * case having to change - the named five above are a floor, not a
			 * ceiling.
			 */
			if (switch_dir_open(&dir, FST_XC_FIXTURE_DIR, pool) == SWITCH_STATUS_SUCCESS) {
				while ((found = switch_dir_next_file(dir, entry, sizeof(entry)))) {
					if (!fst_xc_val_is_invalid_sample(found)) {
						continue;
					}

					if (fst_xc_val_index_of(fst_xc_val_required_invalid_json,
											switch_arraylen(fst_xc_val_required_invalid_json), found) >= 0) {
						/* already run, by name, above */
						continue;
					}

					invalid_files++;
					switch_snprintf(path, sizeof(path), "%s%s", FST_XC_FIXTURE_DIR, found);

					code = fst_xc_val_run_one(path, 1);

					if (code == FST_XC_VAL_UNQUOTABLE) {
						quoting_failures++;
						continue;
					}

					if (code != 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
										  "test_mod_xml_curl: validator returned %d for non-conformant sample [%s]\n", code, found);
					}

					fst_xcheck(code == 1, "every committed non-conformant sample must be refused with exit 1");
				}

				switch_dir_close(dir);
				dir = NULL;

				fst_xcheck(quoting_failures == 0, "a sample path could not be passed to the validator as a single shell word");
			} else {
				fst_fail("the fixtures directory could not be enumerated for the non-conformant samples");
			}

			switch_core_destroy_memory_pool(&pool);
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
