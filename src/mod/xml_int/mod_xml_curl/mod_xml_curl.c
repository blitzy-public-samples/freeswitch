/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
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
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 * Justin Cassidy <xachenant@hotmail.com>
 *
 * mod_xml_curl.c -- CURL XML Gateway
 *
 */
#include <switch.h>
#include <switch_curl.h>


SWITCH_MODULE_LOAD_FUNCTION(mod_xml_curl_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_xml_curl_shutdown);
SWITCH_MODULE_DEFINITION(mod_xml_curl, mod_xml_curl_load, mod_xml_curl_shutdown, NULL);


struct xml_binding {
	char *method;
	char *url;
	char *bindings;
	char *cred;
	char *bind_local;
	int disable100continue;
	int use_get_style;
	uint32_t enable_cacert_check;
	char *ssl_cert_file;
	char *ssl_key_file;
	char *ssl_key_password;
	char *ssl_version;
	char *ssl_cacert_file;
	uint32_t enable_ssl_verifyhost;
	char *cookie_file;
	switch_hash_t *vars_map;
	int use_dynamic_url;
	long auth_scheme;
	int timeout;
	switch_size_t curl_max_bytes;
	/* JSON: opt-in response representation for this binding. Left NULL by the wholesale
	   memset() in do_config() whenever the response-format parameter is absent, which is
	   exactly what preserves the legacy XML-only behaviour. Appended last so that no
	   existing member offset moves. */
	char *response_format;
};

static int keep_files_around = 0;

typedef struct xml_binding xml_binding_t;

#define XML_CURL_MAX_BYTES 1024 * 1024

struct config_data {
	char *name;
	int fd;
	switch_size_t bytes;
	switch_size_t max_bytes;
	int err;
};

typedef struct hash_node {
	switch_hash_t *hash;
	struct hash_node *next;
} hash_node_t;

static struct {
	switch_memory_pool_t *pool;
	hash_node_t *hash_root;
	hash_node_t *hash_tail;
} globals;

#define XML_CURL_SYNTAX "[debug_on|debug_off]"
SWITCH_STANDARD_API(xml_curl_function)
{
	if (session) {
		return SWITCH_STATUS_FALSE;
	}

	if (zstr(cmd)) {
		goto usage;
	}

	if (!strcasecmp(cmd, "debug_on")) {
		keep_files_around = 1;
	} else if (!strcasecmp(cmd, "debug_off")) {
		keep_files_around = 0;
	} else {
		goto usage;
	}

	stream->write_function(stream, "OK\n");
	return SWITCH_STATUS_SUCCESS;

  usage:
	stream->write_function(stream, "USAGE: %s\n", XML_CURL_SYNTAX);
	return SWITCH_STATUS_SUCCESS;
}

static size_t file_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
	register unsigned int realsize = (unsigned int) (size * nmemb);
	struct config_data *config_data = data;
	int x;

	config_data->bytes += realsize;

	if (config_data->bytes > config_data->max_bytes) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Oversized file detected [%d bytes]\n", (int) config_data->bytes);
		config_data->err = 1;
		return 0;
	}

	x = write(config_data->fd, ptr, realsize);
	if (x != (int) realsize) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Short write! %d out of %d\n", x, realsize);
	}
	return x;
}

/*
 * JSON: everything from here down to xml_url_fetch() is new, JSON-specific logic, kept in
 * separate file-static functions so that the original XML fetch path below reads exactly as
 * it did before. None of it executes unless a binding opts in with response-format=json.
 * Every function reports failure through a falsy return so that the single dispatch point in
 * xml_url_fetch() can fall back to the untouched switch_xml_parse_file() call.
 *
 * The translation implements the BadgerFish convention: attributes are members prefixed with
 * '@', text content lives under '$', child elements are nested keys, repeated children of the
 * same name collapse into an array, and the single top-level key is the requested provisioning
 * section name. cJSON is reached through <switch.h> alone, which already pulls in switch_json.h
 * and therefore switch_cJSON.h.
 */

/* JSON: true when an HTTP response Content-Type names the JSON media type. Accepts
   "application/json" bare or carrying parameters such as "; charset=utf-8"; rejects an absent
   or empty header, text/xml, application/xml and every other media type. */
static int xml_curl_json_is_json_content_type(const char *content_type)
{
	char media_type[64] = "";
	const char *p = content_type;
	switch_size_t len = 0;

	if (zstr(p)) {
		return 0;
	}

	/* Tolerate leading linear whitespace ahead of the media type. */
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	/* The media type ends at the first parameter separator or whitespace. */
	while (p[len] && p[len] != ';' && p[len] != ' ' && p[len] != '\t' && p[len] != '\r' && p[len] != '\n') {
		len++;
	}

	if (len == 0 || len >= sizeof(media_type)) {
		return 0;
	}

	memcpy(media_type, p, len);
	media_type[len] = '\0';

	/* Whole-token comparison: type and subtype must both match, case-insensitively. */
	return !strcasecmp(media_type, "application/json") ? 1 : 0;
}

/* JSON: reads the already size-capped response body that file_callback() streamed to the
   temporary file into a NUL-terminated heap buffer. The binding's response ceiling is
   re-checked here rather than re-implemented, so the 1 MiB default and any operator override
   are inherited for free. The caller owns the result and releases it with switch_safe_free().
   Returns NULL on any error, which converges on the XML fallback. */
static char *xml_curl_json_read_file(const char *filename, switch_size_t max_bytes)
{
	struct stat st;
	char *buf = NULL;
	switch_ssize_t bytes_read = 0;
	int fd = -1;

	if (zstr(filename)) {
		return NULL;
	}

	if ((fd = open(filename, O_RDONLY, 0)) < 0) {
		return NULL;
	}

	if (fstat(fd, &st) != 0 || st.st_size <= 0 || (switch_size_t) st.st_size > max_bytes) {
		close(fd);
		return NULL;
	}

	/* Plain malloc with an explicit check rather than switch_must_malloc(): an allocation
	   failure has to degrade to the XML fallback, never abort the process. */
	if (!(buf = malloc((size_t) st.st_size + 1))) {
		close(fd);
		return NULL;
	}

	bytes_read = read(fd, buf, (size_t) st.st_size);
	close(fd);

	if (bytes_read <= 0) {
		switch_safe_free(buf);
		return NULL;
	}

	buf[bytes_read] = '\0';

	return buf;
}

/* JSON: recursive BadgerFish visitor. Populates an existing switch_xml_t node from a cJSON
   object by dispatching on the kind of each member. Returns 1 on success and 0 on any
   translation error; a partially built subtree stays attached to the caller's tree, which the
   root entry point releases with a single switch_xml_free(). */
static int xml_curl_json_to_xml_node(switch_xml_t node, cJSON *object)
{
	cJSON *member = NULL;
	cJSON *element = NULL;
	switch_xml_t child = NULL;

	if (!node || !object) {
		return 0;
	}

	cJSON_ArrayForEach(member, object) {
		/* Every member of a JSON object carries its own key in ->string. A missing or empty
		   key can name neither an element, nor an attribute, nor the text marker. */
		if (zstr(member->string)) {
			return 0;
		}

		if (member->string[0] == '@') {
			/* BadgerFish attribute. The name must survive stripping the '@' and the value
			   must be a string, because switch_xml_set_attr_d() duplicates both unguarded.
			   Attributes are set in JSON member order, which the serializer preserves. */
			if (zstr(member->string + 1) || !cJSON_IsString(member) || !member->valuestring) {
				return 0;
			}
			switch_xml_set_attr_d(node, member->string + 1, member->valuestring);
		} else if (!strcmp(member->string, "$")) {
			/* BadgerFish text content. */
			if (!cJSON_IsString(member) || !member->valuestring) {
				return 0;
			}
			switch_xml_set_txt_d(node, member->valuestring);
		} else if (cJSON_IsArray(member)) {
			/* Repeated children of one name collapse into a JSON array. A constant offset of
			   zero makes switch_xml_insert() append, so array order becomes document order. */
			cJSON_ArrayForEach(element, member) {
				if (!cJSON_IsObject(element)) {
					return 0;
				}
				if (!(child = switch_xml_add_child_d(node, member->string, 0))) {
					return 0;
				}
				if (!xml_curl_json_to_xml_node(child, element)) {
					return 0;
				}
			}
		} else if (cJSON_IsObject(member)) {
			/* A single child element of this name. */
			if (!(child = switch_xml_add_child_d(node, member->string, 0))) {
				return 0;
			}
			if (!xml_curl_json_to_xml_node(child, member)) {
				return 0;
			}
		} else {
			/* Numbers, booleans, nulls and raw values have no guaranteed lexical round trip
			   through the const char * builders, so they are a translation error rather than
			   an implicit coercion. */
			return 0;
		}
	}

	return 1;
}

/* JSON: BadgerFish adapter and root entry point. Translates a JSON provisioning response into
   the same kind of switch_xml_t the XML path produces, or returns NULL on any error. The single
   top-level key names the provisioning section, and the <document type="freeswitch/xml">
   <section name="..."> envelope that switch_xml_locate() searches for is synthesised here: a
   tree without it could not be resolved by the core. Only the duplicating builder variants are
   used, because the cJSON tree owning every name and value is released before the result is
   returned. */
static switch_xml_t xml_curl_json_to_xml(const char *json_text)
{
	cJSON *json = NULL;
	cJSON *root_member = NULL;
	switch_xml_t document = NULL;
	switch_xml_t section = NULL;

	if (zstr(json_text)) {
		return NULL;
	}

	if (!(json = cJSON_Parse(json_text))) {
		return NULL;
	}

	/* The payload must be an object holding exactly one member: the section name mapped to
	   the object to translate. */
	if (!cJSON_IsObject(json) || cJSON_GetArraySize(json) != 1) {
		cJSON_Delete(json);
		return NULL;
	}

	root_member = json->child;

	if (!root_member || zstr(root_member->string) || !cJSON_IsObject(root_member)) {
		cJSON_Delete(json);
		return NULL;
	}

	if (!(document = switch_xml_new("document"))) {
		cJSON_Delete(json);
		return NULL;
	}

	switch_xml_set_attr_d(document, "type", "freeswitch/xml");

	if (!(section = switch_xml_add_child_d(document, "section", 0))) {
		switch_xml_free(document);
		cJSON_Delete(json);
		return NULL;
	}

	/* Set the section name before translating the member so that it is the first attribute,
	   matching the attribute order of the equivalent XML document. */
	switch_xml_set_attr_d(section, "name", root_member->string);

	if (!xml_curl_json_to_xml_node(section, root_member)) {
		switch_xml_free(document);
		cJSON_Delete(json);
		return NULL;
	}

	cJSON_Delete(json);

	return document;
}

/* JSON: the response decode step reached from the single format dispatch point in
   xml_url_fetch(). Validates the response Content-Type, reads the capped temporary file and
   runs the BadgerFish translation. Every failure edge - and only a failure edge - emits one
   SWITCH_LOG_WARNING and returns NULL, so the caller falls through to the unmodified XML
   parse; a fallback is a degradation worth surfacing rather than a failure. cJSON_GetErrorPtr()
   is deliberately not consulted: it is process global while this code runs concurrently on
   many fetch threads, so it could report an unrelated thread's error. */
static switch_xml_t xml_curl_json_decode_response(const char *filename, const char *content_type, switch_size_t max_bytes, const char *url)
{
	switch_xml_t xml = NULL;
	char *json_text = NULL;
	const char *reason = "unknown translation error";

	if (zstr(filename)) {
		reason = "no response body was captured";
	} else if (!xml_curl_json_is_json_content_type(content_type)) {
		reason = "response Content-Type is not application/json";
	} else if (!(json_text = xml_curl_json_read_file(filename, max_bytes))) {
		reason = "response body could not be read";
	} else if (!(xml = xml_curl_json_to_xml(json_text))) {
		reason = "response body is not a well-formed BadgerFish JSON document";
	}

	switch_safe_free(json_text);

	if (!xml) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "JSON decode of the response from [%s] failed (%s) [Content-Type: %s]; falling back to XML parsing\n",
						  switch_str_nil(url), reason, zstr(content_type) ? "(absent)" : content_type);
	}

	return xml;
}




static switch_xml_t xml_url_fetch(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params,
								  void *user_data)
{
	switch_event_t *my_params = NULL;
	char filename[512] = "";
	switch_CURL *curl_handle = NULL;
	switch_CURLcode cc;
	struct config_data config_data;
	switch_xml_t xml = NULL;
	char *data = NULL;
	switch_uuid_t uuid;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	xml_binding_t *binding = (xml_binding_t *) user_data;
	char *file_url;
	switch_curl_slist_t *slist = NULL;
	long httpRes = 0;
	switch_curl_slist_t *headers = NULL;
	char hostname[256] = "";
	char basic_data[512];
	char *uri = NULL;
	char *dynamic_url = NULL;
	char content_type[256] = "";	/* JSON: copy of the response Content-Type, taken while the curl handle is alive */
	char *curl_content_type = NULL;	/* JSON: libcurl-owned; never freed here */

    strncpy(hostname, switch_core_get_switchname(), sizeof(hostname) - 1);

	if (!binding) {
		return NULL;
	}

	if ((file_url = strstr(binding->url, "file:"))) {
		file_url += 5;

		if (!(xml = switch_xml_parse_file(file_url))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Parsing Result!\n");
		}

		return xml;
	}

	switch_snprintf(basic_data, sizeof(basic_data), "hostname=%s&section=%s&tag_name=%s&key_name=%s&key_value=%s",
					hostname, section, switch_str_nil(tag_name), switch_str_nil(key_name), switch_str_nil(key_value));

	data = switch_event_build_param_string(params, basic_data, binding->vars_map);
	switch_assert(data);

	if (binding->use_dynamic_url) {
		if (!params) {
			switch_event_create(&params, SWITCH_EVENT_REQUEST_PARAMS);
			switch_assert(params);
			my_params = params;
		}

		switch_event_add_header_string(params, SWITCH_STACK_TOP, "hostname", hostname);
		switch_event_add_header_string(params, SWITCH_STACK_TOP, "section", switch_str_nil(section));
		switch_event_add_header_string(params, SWITCH_STACK_TOP, "tag_name", switch_str_nil(tag_name));
		switch_event_add_header_string(params, SWITCH_STACK_TOP, "key_name", switch_str_nil(key_name));
		switch_event_add_header_string(params, SWITCH_STACK_TOP, "key_value", switch_str_nil(key_value));
		dynamic_url = switch_event_expand_headers(params, binding->url);
		switch_assert(dynamic_url);
	} else {
		dynamic_url = binding->url;
	}

	if (binding->use_get_style == 1) {
		uri = malloc(strlen(data) + strlen(dynamic_url) + 16);
		switch_assert(uri);
		sprintf(uri, "%s%c%s", dynamic_url, strchr(dynamic_url, '?') != NULL ? '&' : '?', data);
	}

	switch_uuid_get(&uuid);
	switch_uuid_format(uuid_str, &uuid);

	switch_snprintf(filename, sizeof(filename), "%s%s%s.tmp.xml", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, uuid_str);

	memset(&config_data, 0, sizeof(config_data));

	config_data.name = filename;
	config_data.max_bytes = binding->curl_max_bytes;

	if ((config_data.fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR)) > -1) {
		curl_handle = switch_curl_easy_init();
		headers = switch_curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

		/* JSON: when the binding opted in, advertise that a BadgerFish JSON response is
		   acceptable. Purely additive - the request Content-Type above, the form body and the
		   rest of the XML request path are untouched, and switch_curl_slist_free_all(headers)
		   below already releases this entry. */
		if (binding->response_format && !strcasecmp(binding->response_format, "json")) {
			headers = switch_curl_slist_append(headers, "Accept: application/json");
		}

		if (!strncasecmp(binding->url, "https", 5)) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0);
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0);
		}

		if (!zstr(binding->cred)) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, binding->auth_scheme);
			switch_curl_easy_setopt(curl_handle, CURLOPT_USERPWD, binding->cred);
		}
		switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
		if (binding->method != NULL)
			switch_curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, binding->method);
		switch_curl_easy_setopt(curl_handle, CURLOPT_POST, !binding->use_get_style);
		switch_curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);
		switch_curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 10);
		if (!binding->use_get_style)
			switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, data);
		switch_curl_easy_setopt(curl_handle, CURLOPT_URL, binding->use_get_style ? uri : dynamic_url);
		switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, file_callback);
		switch_curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) &config_data);
		switch_curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "freeswitch-xml/1.0");
		switch_curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1);

		if (binding->timeout) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, binding->timeout);
		}

		if (binding->disable100continue) {
			slist = switch_curl_slist_append(slist, "Expect:");
			/* JSON: this pre-existing branch REPLACES the header list assigned above rather
			   than adding to it, which is long-standing behaviour that must not change. The
			   Accept header therefore has to be restated on the list curl will actually use,
			   or it would silently never reach the gateway - disable100continue defaults to
			   1, so this is the common path. Guarded identically to the append above, so a
			   binding without response-format still sends byte-for-byte what it sends today. */
			if (binding->response_format && !strcasecmp(binding->response_format, "json")) {
				slist = switch_curl_slist_append(slist, "Accept: application/json");
			}
			switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, slist);
		}

		if (binding->enable_cacert_check) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, TRUE);
		}

		if (binding->ssl_cert_file) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSLCERT, binding->ssl_cert_file);
		}

		if (binding->ssl_key_file) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSLKEY, binding->ssl_key_file);
		}

		if (binding->ssl_key_password) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSLKEYPASSWD, binding->ssl_key_password);
		}

		if (binding->ssl_version) {
			if (!strcasecmp(binding->ssl_version, "SSLv3")) {
				switch_curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_SSLv3);
			} else if (!strcasecmp(binding->ssl_version, "TLSv1")) {
				switch_curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1);
			}
		}

		if (binding->ssl_cacert_file) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_CAINFO, binding->ssl_cacert_file);
		}

		if (binding->enable_ssl_verifyhost) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2);
		}

		if (binding->cookie_file) {
			switch_curl_easy_setopt(curl_handle, CURLOPT_COOKIEJAR, binding->cookie_file);
			switch_curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, binding->cookie_file);
		}

		if (binding->bind_local) {
			curl_easy_setopt(curl_handle, CURLOPT_INTERFACE, binding->bind_local);
		}

		cc = switch_curl_easy_perform(curl_handle);
		if (cc && cc != CURLE_WRITE_ERROR) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "CURL returned error:[%d] %s\n", cc, switch_curl_easy_strerror(cc));
		}

		switch_curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpRes);
		/* JSON: the response Content-Type has to be read here, adjacent to the response code
		   and before switch_curl_easy_cleanup() below, because libcurl owns that string and
		   frees it together with the handle. Reading it at the decode site would be a
		   use-after-free. libcurl returns NULL when the response carried no Content-Type. */
		switch_curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &curl_content_type);
		if (curl_content_type) {
			switch_copy_string(content_type, curl_content_type, sizeof(content_type));
		}
		switch_curl_easy_cleanup(curl_handle);
		switch_curl_slist_free_all(headers);
		switch_curl_slist_free_all(slist);
		close(config_data.fd);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening temp file!\n");
	}

	if (config_data.err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error encountered! [%s]\ndata: [%s]\n", binding->url, data);
		xml = NULL;
	} else {
		if (httpRes == 200) {
			/* JSON: the single response-format dispatch point. When this binding opted in with
			   response-format=json, attempt the BadgerFish decode first. The helper emits one
			   WARNING on every failure edge and returns NULL, so a non-JSON Content-Type, an
			   unreadable body, unparseable JSON or a wrongly shaped document all converge on
			   the original XML parse below. */
			if (binding->response_format && !strcasecmp(binding->response_format, "json")) {
				xml = xml_curl_json_decode_response(filename, content_type, binding->curl_max_bytes, binding->url);
			}

			/* JSON: this guard is the only change made to the XML path. The two statements it
			   wraps are the pre-existing parse call and its error report, kept byte for byte -
			   including their original indentation - so the fallback is provably unchanged. */
			if (!xml) {
			if (!(xml = switch_xml_parse_file(filename))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Parsing Result! [%s]\ndata: [%s]\n", binding->url, data);
			}
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Received HTTP error %ld trying to fetch %s\ndata: [%s]\n", httpRes, binding->url,
							  data);
			xml = NULL;
		}
	}

	/* Debug by leaving the file behind for review */
	if (keep_files_around) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE, "XML response is in %s\n", filename);
	} else {
		if (unlink(filename) != 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "XML response file [%s] delete failed\n", filename);
		}
	}

	switch_safe_free(data);
	if (binding->use_get_style == 1)
		switch_safe_free(uri);
	if (binding->use_dynamic_url && dynamic_url != binding->url)
		switch_safe_free(dynamic_url);

	if (my_params) {
		switch_event_destroy(&my_params);
	}

	return xml;
}

#define ENABLE_PARAM_VALUE "enabled"
static switch_status_t do_config(void)
{
	char *cf = "xml_curl.conf";
	switch_xml_t cfg, xml, bindings_tag, binding_tag, param;
	xml_binding_t *binding = NULL;
	int x = 0;
	int need_vars_map = 0;
	switch_hash_t *vars_map = NULL;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", cf);
		return SWITCH_STATUS_TERM;
	}

	if (!(bindings_tag = switch_xml_child(cfg, "bindings"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing <bindings> tag!\n");
		goto done;
	}

	for (binding_tag = switch_xml_child(bindings_tag, "binding"); binding_tag; binding_tag = binding_tag->next) {
		char *bname = (char *) switch_xml_attr_soft(binding_tag, "name");
		char *url = NULL;
		char *bind_local = NULL;
		char *bind_cred = NULL;
		char *bind_mask = NULL;
		char *method = NULL;
		int disable100continue = 1;
		int use_dynamic_url = 0, timeout = 0;
		switch_size_t curl_max_bytes = XML_CURL_MAX_BYTES;
		char *response_format = NULL;	/* JSON: opt-in response representation; NULL means XML */
		uint32_t enable_cacert_check = 0;
		char *ssl_cert_file = NULL;
		char *ssl_key_file = NULL;
		char *ssl_key_password = NULL;
		char *ssl_version = NULL;
		char *ssl_cacert_file = NULL;
		uint32_t enable_ssl_verifyhost = 0;
		char *cookie_file = NULL;
		hash_node_t *hash_node;
		long auth_scheme = CURLAUTH_BASIC;
		need_vars_map = 0;
		vars_map = NULL;


		for (param = switch_xml_child(binding_tag, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!strcasecmp(var, "gateway-url")) {
				bind_mask = (char *) switch_xml_attr_soft(param, "bindings");
				if (val) {
					url = val;
				}
			} else if (!strcasecmp(var, "gateway-credentials")) {
				bind_cred = val;
			} else if (!strcasecmp(var, "auth-scheme")) {
				if (*val == '=') {
					auth_scheme = 0;
					val++;
				}

				if (!strcasecmp(val, "basic")) {
					auth_scheme |= CURLAUTH_BASIC;
				} else if (!strcasecmp(val, "digest")) {
					auth_scheme |= CURLAUTH_DIGEST;
				} else if (!strcasecmp(val, "NTLM")) {
					auth_scheme |= CURLAUTH_NTLM;
				} else if (!strcasecmp(val, "GSS-NEGOTIATE")) {
					auth_scheme |= CURLAUTH_GSSNEGOTIATE;
				} else if (!strcasecmp(val, "any")) {
					auth_scheme = (long)CURLAUTH_ANY;
				}
			} else if (!strcasecmp(var, "disable-100-continue") && !switch_true(val)) {
				disable100continue = 0;
			} else if (!strcasecmp(var, "method")) {
				method = val;
			} else if (!strcasecmp(var, "timeout")) {
				int tmp = atoi(val);
				if (tmp >= 0) {
					timeout = tmp;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set a negative timeout!\n");
				}
			} else if (!strcasecmp(var, "enable-cacert-check") && switch_true(val)) {
				enable_cacert_check = 1;
			} else if (!strcasecmp(var, "ssl-cert-path")) {
				ssl_cert_file = val;
			} else if (!strcasecmp(var, "ssl-key-path")) {
				ssl_key_file = val;
			} else if (!strcasecmp(var, "ssl-key-password")) {
				ssl_key_password = val;
			} else if (!strcasecmp(var, "ssl-version")) {
				ssl_version = val;
			} else if (!strcasecmp(var, "ssl-cacert-file")) {
				ssl_cacert_file = val;
			} else if (!strcasecmp(var, "enable-ssl-verifyhost") && switch_true(val)) {
				enable_ssl_verifyhost = 1;
			} else if (!strcasecmp(var, "cookie-file")) {
				cookie_file = val;
			} else if (!strcasecmp(var, "use-dynamic-url") && switch_true(val)) {
				use_dynamic_url = 1;
			} else if (!strcasecmp(var, "enable-post-var")) {
				if (!vars_map && need_vars_map == 0) {
					if (switch_core_hash_init(&vars_map) != SWITCH_STATUS_SUCCESS) {
						need_vars_map = -1;
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Can't init params hash!\n");
						continue;
					}
					need_vars_map = 1;
				}

				if (vars_map && val) {
					if (switch_core_hash_insert(vars_map, val, ENABLE_PARAM_VALUE) != SWITCH_STATUS_SUCCESS) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Can't add %s to params hash!\n", val);
					}
				}
			} else if (!strcasecmp(var, "bind-local")) {
				bind_local = val;
			} else if (!strcasecmp(var, "response-max-bytes")) {
				int tmp = atoi(val);
				if (tmp >= 0) {
					curl_max_bytes = tmp;
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set a negative maximum response bytes!\n");
				}
			} else if (!strcasecmp(var, "response-format")) {
				/* JSON: opt-in response representation. Appended at the tail of the chain so
				   that every pre-existing comparison keeps its current evaluation order. When
				   this parameter is absent the module behaves exactly as it did before. */
				response_format = val;
			}
		}

		if (!url) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Binding has no url!\n");
			if (vars_map)
				switch_core_hash_destroy(&vars_map);
			continue;
		}

		if (!(binding = switch_core_alloc(globals.pool, sizeof(*binding)))) {
			if (vars_map)
				switch_core_hash_destroy(&vars_map);
			goto done;
		}
		memset(binding, 0, sizeof(*binding));

		binding->auth_scheme = auth_scheme;
		binding->timeout = timeout;
		binding->url = switch_core_strdup(globals.pool, url);
		switch_assert(binding->url);

		if (bind_local != NULL) {
			binding->bind_local = switch_core_strdup(globals.pool, bind_local);
		}
		if (method != NULL) {
			binding->method = switch_core_strdup(globals.pool, method);
		} else {
			binding->method = NULL;
		}
		if (bind_mask) {
			binding->bindings = switch_core_strdup(globals.pool, bind_mask);
		}

		if (bind_cred) {
			binding->cred = switch_core_strdup(globals.pool, bind_cred);
		}

		binding->disable100continue = disable100continue;
		binding->use_get_style = method != NULL && strcasecmp(method, "post") != 0;
		binding->use_dynamic_url = use_dynamic_url;
		binding->enable_cacert_check = enable_cacert_check;

		if (ssl_cert_file) {
			binding->ssl_cert_file = switch_core_strdup(globals.pool, ssl_cert_file);
		}

		if (ssl_key_file) {
			binding->ssl_key_file = switch_core_strdup(globals.pool, ssl_key_file);
		}

		if (ssl_key_password) {
			binding->ssl_key_password = switch_core_strdup(globals.pool, ssl_key_password);
		}

		if (ssl_version) {
			binding->ssl_version = switch_core_strdup(globals.pool, ssl_version);
		}

		if (ssl_cacert_file) {
			binding->ssl_cacert_file = switch_core_strdup(globals.pool, ssl_cacert_file);
		}

		binding->enable_ssl_verifyhost = enable_ssl_verifyhost;

		if (cookie_file) {
			binding->cookie_file = switch_core_strdup(globals.pool, cookie_file);
		}

		binding->vars_map = vars_map;

		if (vars_map) {
			switch_zmalloc(hash_node, sizeof(hash_node_t));
			hash_node->hash = vars_map;
			hash_node->next = NULL;

			if (!globals.hash_root) {
				globals.hash_root = hash_node;
				globals.hash_tail = globals.hash_root;
			}

			else {
				globals.hash_tail->next = hash_node;
				globals.hash_tail = globals.hash_tail->next;
			}

		}

		binding->curl_max_bytes = curl_max_bytes;

		/* JSON: optional per-binding response format. Guarded exactly like its siblings above,
		   so an absent parameter leaves the member at the NULL the memset() already wrote. The
		   string lives in the module pool and is never freed individually. */
		if (response_format != NULL) {
			binding->response_format = switch_core_strdup(globals.pool, response_format);
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Binding [%s] XML Fetch Function [%s] [%s]\n",
						  zstr(bname) ? "N/A" : bname, binding->url, binding->bindings ? binding->bindings : "all");
		switch_xml_bind_search_function(xml_url_fetch, switch_xml_parse_section_string(binding->bindings), binding);
		x++;
		binding = NULL;
	}

  done:
	switch_xml_free(xml);

	return x ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_xml_curl_load)
{
	switch_api_interface_t *xml_curl_api_interface;

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	globals.hash_root = NULL;
	globals.hash_tail = NULL;

	if (do_config() != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	SWITCH_ADD_API(xml_curl_api_interface, "xml_curl", "XML Curl", xml_curl_function, XML_CURL_SYNTAX);
	switch_console_set_complete("add xml_curl debug_on");
	switch_console_set_complete("add xml_curl debug_off");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_xml_curl_shutdown)
{
	hash_node_t *ptr = NULL;

	while (globals.hash_root) {
		ptr = globals.hash_root;
		switch_core_hash_destroy(&ptr->hash);
		globals.hash_root = ptr->next;
		switch_safe_free(ptr);
	}

	switch_xml_unbind_search_function_ptr(xml_url_fetch);

	return SWITCH_STATUS_SUCCESS;
}

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
