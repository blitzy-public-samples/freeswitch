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
	/* JSON: opt-in response representation for this binding. NULL selects the XML decoder;
	   keep this append-only field last so no other member offset moves. */
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
 * JSON: everything from here down to xml_url_fetch() is file-local and decodes a response only
 * when the binding carries response-format=json. Every helper reports failure through a falsy
 * return, so the single dispatch point in xml_url_fetch() falls back to the switch_xml_parse_file()
 * call it shares with the XML path.
 *
 * The translation implements the BadgerFish convention: attributes are members prefixed with
 * '@', text content lives under '$', child elements are nested keys, repeated children of the
 * same name collapse into an array, and the single top-level key is the requested provisioning
 * section name. cJSON is reached through <switch.h> alone, which already pulls in switch_json.h
 * and therefore switch_cJSON.h.
 *
 * SECURITY POSTURE. A provisioning response is remote, untrusted input that goes on to decide
 * directory, dialplan and configuration policy, so this module owns the canonical JSON contract
 * and the payload must conform to it rather than the reverse. The decoder is therefore a
 * whitelist, not a best-effort parser, and it is layered - each layer documented at its own
 * definition:
 *
 *   1. xml_curl_json_read_file()     - exact read of the capped body
 *   2. xml_curl_json_validate_text() - lexical gate on the raw bytes, before cJSON is called
 *   3. xml_curl_json_to_xml()        - one top-level key, and it must name the requested section
 *   4. xml_curl_json_to_xml_node()   - XML name, UTF-8 and budget validation per member
 */

/*
 * JSON: hard resource ceilings for the decoder. They are compile-time constants enforced by this
 * module rather than parser configuration, because CJSON_NESTING_LIMIT is set by the autotools
 * build only and a build that does not define it inherits a far looser default. Bounding the
 * input here keeps the behaviour identical on every platform and every build.
 *
 * XML_CURL_JSON_MAX_CHILDREN_PER_PARENT bounds the COST of building the tree rather than its
 * size, which is what stops a merely wide response becoming a denial of service. Because this
 * translator passes a constant offset of zero so that insertion order becomes document order,
 * every switch_xml_insert() append walks the parent's ordered and same-name chains to their tail,
 * so n children under one parent cost O(n^2) comparisons synchronously on the fetch path. Capping
 * any one parent at w bounds the whole-document total at XML_CURL_JSON_MAX_NODES * w - linear in
 * the node ceiling instead of quadratic. 256 is two orders of magnitude below the node ceiling
 * and an order of magnitude above the widest parent in any shipped provisioning document.
 *
 * XML_CURL_JSON_MAX_ARRAY_ELEMENTS is no larger than that per-parent ceiling, so a single array
 * cannot be wider than a parent may be. The two checks stay distinct because the array ceiling
 * refuses before anything is built, while the per-parent ceiling catches width accumulated
 * across several member names.
 */
#define XML_CURL_JSON_MAX_DEPTH 32			/* nesting levels in the JSON document */
#define XML_CURL_JSON_MAX_VALUES 50000		/* objects + arrays + strings in the document */
#define XML_CURL_JSON_MAX_OBJECT_MEMBERS 256	/* members of one JSON object */
#define XML_CURL_JSON_MAX_ARRAY_ELEMENTS 256	/* repeated children under one name */
#define XML_CURL_JSON_MAX_CHILDREN_PER_PARENT 256	/* child elements built under one parent */
#define XML_CURL_JSON_MAX_NODES 20000		/* elements + attributes built in total */
#define XML_CURL_JSON_MAX_STRING_BYTES 8192	/* bytes in one name or one value */
#define XML_CURL_JSON_MAX_NAME_BYTES 128	/* bytes in one element or attribute name */

/*
 * JSON: ceiling for the cumulative NAME bytes the translation writes into the tree. It is a
 * ceiling of its own rather than the payload length, because a repeated child key is written once
 * per array element: {"directory":{"extension":[{},{},{}]}} legitimately transforms into more name
 * bytes than it occupies as input, and charging that expansion against the input size would refuse
 * the very shape the array mapping exists to express. The value follows from the two ceilings that
 * already bound the expansion - at most XML_CURL_JSON_MAX_NODES names, each at most
 * XML_CURL_JSON_MAX_NAME_BYTES plus the byte a BadgerFish '@' prefix adds.
 */
#define XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES (XML_CURL_JSON_MAX_NODES * (XML_CURL_JSON_MAX_NAME_BYTES + 1))

/*
 * JSON: the budget threaded through the recursive translation. One caller-owned structure is what
 * makes the ceilings above document-wide rather than per-node, so a wide-and-shallow document
 * cannot amplify past a deep-and-narrow one.
 *
 * The two volume counters are separate because they are bounded by different things. Every decoded
 * value appears exactly once in the payload and escape sequences only ever shrink, so text_bytes is
 * soundly bounded by the length of the payload the binding already size-capped. Names are not: an
 * array collapses n repeated children onto one key, so that key is written n times into the tree
 * from a single occurrence in the input. name_bytes therefore carries its own expanded-output
 * ceiling instead of being charged against the input length.
 */
struct xml_curl_json_budget {
	int depth;						/* nesting depth of the node being translated */
	switch_size_t nodes;			/* elements and attributes built so far */
	switch_size_t name_bytes;		/* cumulative element and attribute name bytes written */
	switch_size_t text_bytes;		/* cumulative decoded attribute-value and text bytes */
	switch_size_t max_text_bytes;	/* ceiling for text_bytes: the payload length */
};

typedef struct xml_curl_json_budget xml_curl_json_budget_t;

/*
 * JSON: charges `add` against `*counter` unless the charge would breach `limit`. The test is
 * written against the remaining headroom rather than on the sum, so it is correct for every value
 * of `add` and cannot itself overflow - which is the whole point of routing every charge through
 * one place. Returns 1 when the charge was accepted, and leaves the counter untouched otherwise.
 */
static int xml_curl_json_budget_charge(switch_size_t *counter, switch_size_t add, switch_size_t limit)
{
	if (!counter || *counter > limit || add > limit - *counter) {
		return 0;
	}

	*counter += add;

	return 1;
}

/* JSON: length of the UTF-8 sequence starting at p, or 0 when the bytes are not a well-formed
   sequence. Strict by design: overlong encodings, surrogate code points and anything above
   U+10FFFF are all rejected, because cJSON does not validate UTF-8 and the XML serializer would
   otherwise emit a document no conforming parser can read back. */
static int xml_curl_json_utf8_len(const unsigned char *p, switch_size_t remaining)
{
	unsigned char c;
	int len = 0;
	int i;

	if (!p || !remaining) {
		return 0;
	}

	c = p[0];

	if (c < 0x80) {
		return 1;
	} else if (c >= 0xc2 && c <= 0xdf) {
		len = 2;
	} else if (c >= 0xe0 && c <= 0xef) {
		len = 3;
	} else if (c >= 0xf0 && c <= 0xf4) {
		len = 4;
	} else {
		/* 0x80-0xc1 and 0xf5-0xff never start a valid sequence */
		return 0;
	}

	if (remaining < (switch_size_t) len) {
		return 0;
	}

	for (i = 1; i < len; i++) {
		if (p[i] < 0x80 || p[i] > 0xbf) {
			return 0;
		}
	}

	/* Reject the overlong, surrogate and out-of-range second bytes */
	if (len == 3) {
		if (c == 0xe0 && p[1] < 0xa0) {
			return 0;
		}
		if (c == 0xed && p[1] > 0x9f) {
			return 0;
		}
	} else if (len == 4) {
		if (c == 0xf0 && p[1] < 0x90) {
			return 0;
		}
		if (c == 0xf4 && p[1] > 0x8f) {
			return 0;
		}
	}

	return len;
}

/* JSON: true when a code point may appear in XML 1.0 character data. Tab, newline and carriage
   return are the only control characters XML permits; NUL, backspace, form feed and the rest of
   the C0 range are forbidden, as are the two permanently invalid code points. */
static int xml_curl_json_is_xml_char(unsigned long cp)
{
	if (cp == 0x09 || cp == 0x0a || cp == 0x0d) {
		return 1;
	}

	if (cp < 0x20) {
		return 0;
	}

	if (cp >= 0xd800 && cp <= 0xdfff) {
		return 0;
	}

	if (cp == 0xfffe || cp == 0xffff) {
		return 0;
	}

	if (cp > 0x10ffff) {
		return 0;
	}

	return 1;
}

/*
 * JSON: validates a decoded string that is about to be handed to a duplicating builder. The
 * builders measure with strlen(), so a string carrying an embedded NUL would be silently
 * truncated - "alice\u0000admin" becoming the identifier "alice". This validator measures the
 * same way, with its own strlen(), and so cannot see past such a NUL either: an embedded NUL is
 * refused upstream instead, by xml_curl_json_validate_unicode_escape() rejecting a \u0000 escape
 * before cJSON can decode it and by xml_curl_json_read_file() refusing a literal NUL anywhere
 * inside the recorded body length. What is established here is everything else - the buffer is
 * walked byte by byte rather than trusted, and 1 is returned only when the string is bounded,
 * well-formed UTF-8 and legal XML character data throughout.
 *
 * The two-character sequence "<!" is refused outright, and that rule is the reason this one
 * validator guards every builder call site rather than each one guarding itself. The serializer
 * escapes '<' as &lt; in the ordinary case, but switch_xml_ampencode() special-cases a '<' whose
 * next byte is '!': it emits the '<' raw and switches into an immune mode that copies every
 * remaining byte of the string verbatim. A value of <![CDATA[..]]> - or of "<!-- " followed by a
 * quote - would therefore leave the escaping regime altogether and let a response forge attribute
 * and element syntax in the serialised document. Refusing the sequence before it reaches a
 * builder is what keeps the serialisation faithful. A lone '<' is still accepted and still
 * escaped, because it never reaches the immune branch.
 */
static int xml_curl_json_is_valid_text(const char *text)
{
	const unsigned char *p = (const unsigned char *) text;
	switch_size_t len = 0;
	switch_size_t i = 0;
	int seq;
	unsigned long cp;

	if (!text) {
		return 0;
	}

	len = strlen(text);

	if (len > XML_CURL_JSON_MAX_STRING_BYTES) {
		return 0;
	}

	while (i < len) {
		if (!(seq = xml_curl_json_utf8_len(p + i, len - i))) {
			return 0;
		}

		if (seq == 1) {
			cp = p[i];
		} else if (seq == 2) {
			cp = ((unsigned long) (p[i] & 0x1f) << 6) | (p[i + 1] & 0x3f);
		} else if (seq == 3) {
			cp = ((unsigned long) (p[i] & 0x0f) << 12) | ((unsigned long) (p[i + 1] & 0x3f) << 6) | (p[i + 2] & 0x3f);
		} else {
			cp = ((unsigned long) (p[i] & 0x07) << 18) | ((unsigned long) (p[i + 1] & 0x3f) << 12) |
				((unsigned long) (p[i + 2] & 0x3f) << 6) | (p[i + 3] & 0x3f);
		}

		if (!xml_curl_json_is_xml_char(cp)) {
			return 0;
		}

		/* the serializer's immune-mode trigger; see the note above this function */
		if (cp == '<' && i + 1 < len && p[i + 1] == '!') {
			return 0;
		}

		i += (switch_size_t) seq;
	}

	return 1;
}

/* JSON: one character of the provisioning-safe element and attribute name alphabet. This is a
   deliberate subset of the XML Name production - ASCII letters, digits, underscore, hyphen and
   period, with a letter or underscore leading - because a JSON member name reaches
   switch_xml_add_child_d() and switch_xml_set_attr_d() verbatim and the serializer writes names
   without escaping. A name such as x></x><evil would otherwise forge document structure, and
   a name carrying a quote would forge an attribute. Every tag and attribute FreeSWITCH
   provisioning actually uses is inside this subset. */
static int xml_curl_json_is_name_char(char c, int first)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
		return 1;
	}

	if (!first && ((c >= '0' && c <= '9') || c == '-' || c == '.')) {
		return 1;
	}

	return 0;
}

/* JSON: validates an element or attribute name. A single colon is accepted as a namespace
   separator, and both halves must then be well-formed names in their own right, so ":x", "x:"
   and "a:b:c" are all refused rather than passed through as malformed namespace syntax. */
static int xml_curl_json_is_valid_xml_name(const char *name)
{
	switch_size_t len = 0;
	switch_size_t i = 0;
	int colons = 0;
	int first = 1;

	if (zstr(name)) {
		return 0;
	}

	len = strlen(name);

	if (len > XML_CURL_JSON_MAX_NAME_BYTES) {
		return 0;
	}

	for (i = 0; i < len; i++) {
		if (name[i] == ':') {
			if (++colons > 1 || first) {
				/* a leading colon, or a second one */
				return 0;
			}
			/* the local part after the colon must start a name of its own */
			first = 1;
			continue;
		}

		if (!xml_curl_json_is_name_char(name[i], first)) {
			return 0;
		}

		first = 0;
	}

	/* first is still set only when the name ended on a colon */
	return first ? 0 : 1;
}

/* JSON: validates one \uXXXX escape and, when it opens a surrogate pair, its partner. Returns
   the number of bytes consumed from p (which points at the 'u') or 0 when the escape is
   malformed, names a code point XML forbids, or is an unpaired surrogate half. Rejecting
   \u0000 here is what stops cJSON from decoding it into an embedded NUL. */
static switch_size_t xml_curl_json_validate_unicode_escape(const char *p, switch_size_t remaining)
{
	unsigned long cp = 0;
	switch_size_t i = 0;

	/* 'u' plus four hex digits */
	if (remaining < 5) {
		return 0;
	}

	for (i = 1; i <= 4; i++) {
		char c = p[i];

		cp <<= 4;

		if (c >= '0' && c <= '9') {
			cp |= (unsigned long) (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			cp |= (unsigned long) (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			cp |= (unsigned long) (c - 'A' + 10);
		} else {
			return 0;
		}
	}

	if (cp >= 0xd800 && cp <= 0xdbff) {
		unsigned long low = 0;

		/* high surrogate: the low half must follow immediately as another escape */
		if (remaining < 11 || p[5] != '\\' || p[6] != 'u') {
			return 0;
		}

		for (i = 7; i <= 10; i++) {
			char c = p[i];

			low <<= 4;

			if (c >= '0' && c <= '9') {
				low |= (unsigned long) (c - '0');
			} else if (c >= 'a' && c <= 'f') {
				low |= (unsigned long) (c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				low |= (unsigned long) (c - 'A' + 10);
			} else {
				return 0;
			}
		}

		if (low < 0xdc00 || low > 0xdfff) {
			return 0;
		}

		return 11;
	}

	if (!xml_curl_json_is_xml_char(cp)) {
		/* covers \u0000, \u0008, \u000c, the rest of C0, lone low surrogates and U+FFFE/FFFF */
		return 0;
	}

	return 5;
}

/*
 * JSON: the lexical gate. Walks the raw payload once, before cJSON sees it, and accepts only the
 * restricted profile this module documents: every value is an object, an array or a string.
 *
 * Refusing number, boolean and null tokens here is the point. They have no representation in the
 * translation - the builders take const char * and there is no guaranteed lexical round trip for
 * a JSON number - so they were already a translation error, but only after the parser had
 * consumed them. Rejecting the token before the parser runs keeps untrusted input away from the
 * number scanner altogether.
 *
 * The same pass bounds nesting depth, total value count and individual string length, and
 * validates every escape sequence and every raw UTF-8 sequence. Returns 1 when the payload is
 * inside the profile.
 */
static int xml_curl_json_validate_text(const char *json_text)
{
	char stack[XML_CURL_JSON_MAX_DEPTH];
	const char *p = json_text;
	switch_size_t len = 0;
	switch_size_t i = 0;
	switch_size_t values = 0;
	int depth = 0;
	int seq = 0;

	if (zstr(json_text)) {
		return 0;
	}

	len = strlen(json_text);

	/* Skip one UTF-8 byte order mark, which the parser tolerates as well */
	if (len >= 3 && (unsigned char) p[0] == 0xef && (unsigned char) p[1] == 0xbb && (unsigned char) p[2] == 0xbf) {
		i = 3;
	}

	for (; i < len; i++) {
		char c = p[i];

		if (c == '{' || c == '[') {
			if (depth >= XML_CURL_JSON_MAX_DEPTH) {
				return 0;
			}
			if (++values > XML_CURL_JSON_MAX_VALUES) {
				return 0;
			}
			stack[depth++] = c;
			continue;
		}

		if (c == '}' || c == ']') {
			if (!depth || stack[depth - 1] != (c == '}' ? '{' : '[')) {
				return 0;
			}
			depth--;
			continue;
		}

		if (c == ':' || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			continue;
		}

		if (c == '"') {
			switch_size_t start = ++i;

			if (++values > XML_CURL_JSON_MAX_VALUES) {
				return 0;
			}

			while (i < len && p[i] != '"') {
				if (p[i] == '\\') {
					switch_size_t consumed = 0;

					if (i + 1 >= len) {
						return 0;
					}

					switch (p[i + 1]) {
					case '"':
					case '\\':
					case '/':
					case 'n':
					case 'r':
					case 't':
						/* legal in JSON and legal in XML character data */
						i += 2;
						continue;
					case 'u':
						if (!(consumed = xml_curl_json_validate_unicode_escape(p + i + 1, len - (i + 1)))) {
							return 0;
						}
						i += 1 + consumed;
						continue;
					default:
						/* \b and \f name characters XML forbids; anything else is not an
						   escape at all */
						return 0;
					}
				}

				if ((unsigned char) p[i] < 0x20) {
					/* a raw control byte is invalid inside a JSON string */
					return 0;
				}

				if (!(seq = xml_curl_json_utf8_len((const unsigned char *) p + i, len - i))) {
					return 0;
				}

				i += (switch_size_t) seq;
			}

			if (i >= len) {
				/* unterminated string */
				return 0;
			}

			if (i - start > XML_CURL_JSON_MAX_STRING_BYTES) {
				return 0;
			}

			continue;
		}

		/* Anything else - a digit, a sign, a period, or the first letter of true/false/null -
		   is outside the profile and is refused before the parser can interpret it. */
		return 0;
	}

	return depth == 0 && values > 0;
}

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

/*
 * JSON: reads the already size-capped response body that file_callback() streamed to the
 * temporary file into a NUL-terminated heap buffer. The binding's response ceiling is re-checked
 * here rather than re-implemented, so the 1 MiB default and any operator override are inherited
 * for free. The caller owns the result and releases it with switch_safe_free(). Returns NULL on
 * any error, which converges on the XML fallback.
 *
 * The body is read in a loop until the exact size fstat() reported has been consumed. A single
 * read() is allowed to return fewer bytes than asked for, and accepting a short count would hand
 * the decoder a truncated document whose accidental prefix might still parse - so a short read,
 * a premature EOF and a read error are all treated as failures rather than as a smaller payload.
 * An interrupted read is retried, because EINTR is not a data error.
 *
 * A NUL byte anywhere inside the recorded length is likewise refused. Everything downstream is
 * NUL-terminated C strings, so a payload of "{...}\0<more bytes>" would otherwise be silently
 * split, letting a caller-visible document hide arbitrary trailing content.
 */
static char *xml_curl_json_read_file(const char *filename, switch_size_t max_bytes)
{
	struct stat st;
	char *buf = NULL;
	switch_size_t want = 0;
	switch_size_t total = 0;
	switch_ssize_t bytes_read;
	int failed = 0;
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

	want = (switch_size_t) st.st_size;

	/* Plain malloc with an explicit check rather than switch_must_malloc(): an allocation
	   failure has to degrade to the XML fallback, never abort the process. */
	if (!(buf = malloc(want + 1))) {
		close(fd);
		return NULL;
	}

	while (total < want) {
		bytes_read = read(fd, buf + total, want - total);

		if (bytes_read < 0) {
			if (errno == EINTR) {
				continue;
			}
			failed = 1;
			break;
		}

		if (bytes_read == 0) {
			/* the file shrank under us, or was never as long as fstat() claimed */
			failed = 1;
			break;
		}

		total += (switch_size_t) bytes_read;
	}

	close(fd);

	if (failed || total != want || memchr(buf, '\0', total)) {
		switch_safe_free(buf);
		return NULL;
	}

	buf[total] = '\0';

	return buf;
}

/*
 * JSON: checks one JSON object against the structural half of the canonical contract before any
 * of it is translated, so that a document which cannot be represented faithfully is refused
 * rather than half-built.
 *
 * Three properties are enforced, all of them cases where cJSON is happy but the mapping is not
 * well defined:
 *
 *   - Bounded width. An object may not carry more members than the ceiling allows.
 *   - No duplicate member names. cJSON preserves duplicates, so "@id" twice would silently be
 *     last-wins on an attribute while a repeated child key would become two elements. Either way
 *     the document a producer validated and the tree this module builds could disagree, so the
 *     duplicate is refused and an array remains the one and only way to express repetition.
 *   - No mixed content. switch_xml serialization emits an element's text only when the element
 *     has no children, so a node carrying both "$" and child keys could not round trip; the
 *     BadgerFish convention cannot recover the separate text runs of mixed content either.
 *
 * Returns 1 when the object is acceptable.
 */
static int xml_curl_json_check_object(cJSON *object)
{
	cJSON *member = NULL;
	cJSON *other = NULL;
	int members = 0;
	int has_text = 0;
	int has_children = 0;

	if (!object) {
		return 0;
	}

	for (member = object->child; member; member = member->next) {
		/* Every member of a JSON object carries its own key in ->string. A missing or empty
		   key can name neither an element, nor an attribute, nor the text marker. */
		if (zstr(member->string)) {
			return 0;
		}

		if (++members > XML_CURL_JSON_MAX_OBJECT_MEMBERS) {
			return 0;
		}

		if (!strcmp(member->string, "$")) {
			has_text = 1;
		} else if (member->string[0] != '@') {
			has_children = 1;
		}
	}

	if (has_text && has_children) {
		return 0;
	}

	/* Width is bounded above, so this pairwise scan is bounded too */
	for (member = object->child; member; member = member->next) {
		for (other = member->next; other; other = other->next) {
			if (!strcmp(member->string, other->string)) {
				return 0;
			}
		}
	}

	return 1;
}

/*
 * JSON: recursive BadgerFish visitor. Populates an existing switch_xml_t node from a cJSON
 * object by dispatching on the kind of each member. Returns 1 on success and 0 on any
 * translation error; a partially built subtree stays attached to the caller's tree, which the
 * root entry point releases with a single switch_xml_free().
 *
 * Every name and every value is validated before it reaches a builder. The lexical gate proves the
 * payload is well-formed JSON of the right shape; these checks prove the strings inside it are
 * usable as XML. The builders duplicate whatever they are given with no inspection and the
 * serializer writes names without escaping, so this is the last place an unusable name or value
 * can be stopped.
 *
 * The budget is shared across the whole document, so breadth is bounded as tightly as depth. Width
 * is additionally bounded per parent by the local "children" counter, which keeps the cost of
 * switch_xml_insert()'s tail walk linear in the node ceiling; it is a local rather than a budget
 * member because the limit is per parent, so each recursive call gets its own.
 */
static int xml_curl_json_to_xml_node(switch_xml_t node, cJSON *object, xml_curl_json_budget_t *budget)
{
	cJSON *member = NULL;
	cJSON *element = NULL;
	switch_xml_t child = NULL;
	int elements;
	int children = 0;

	if (!node || !object || !budget) {
		return 0;
	}

	if (++budget->depth > XML_CURL_JSON_MAX_DEPTH) {
		return 0;
	}

	if (!xml_curl_json_check_object(object)) {
		return 0;
	}

	cJSON_ArrayForEach(member, object) {
		if (member->string[0] == '@') {
			/* BadgerFish attribute: the name must be a legal XML name once the '@' is stripped
			   and the value must be a string. switch_xml_set_attr_d() duplicates the name as
			   given and the value through switch_str_nil(), so neither is validated there and
			   both are detached from cJSON lifetime here. Attributes are set in JSON member
			   order, which the serializer preserves. */
			if (!xml_curl_json_is_valid_xml_name(member->string + 1)) {
				return 0;
			}
			if (!cJSON_IsString(member) || !member->valuestring || !xml_curl_json_is_valid_text(member->valuestring)) {
				return 0;
			}
			if (++budget->nodes > XML_CURL_JSON_MAX_NODES) {
				return 0;
			}
			/* The name is charged against the expanded-output ceiling and the value against the
			   payload length: the two are bounded by different things. */
			if (!xml_curl_json_budget_charge(&budget->name_bytes, strlen(member->string), XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES)) {
				return 0;
			}
			if (!xml_curl_json_budget_charge(&budget->text_bytes, strlen(member->valuestring), budget->max_text_bytes)) {
				return 0;
			}
			switch_xml_set_attr_d(node, member->string + 1, member->valuestring);
		} else if (!strcmp(member->string, "$")) {
			/* BadgerFish text content. */
			if (!cJSON_IsString(member) || !member->valuestring || !xml_curl_json_is_valid_text(member->valuestring)) {
				return 0;
			}
			if (!xml_curl_json_budget_charge(&budget->text_bytes, strlen(member->valuestring), budget->max_text_bytes)) {
				return 0;
			}
			switch_xml_set_txt_d(node, member->valuestring);
		} else if (cJSON_IsArray(member)) {
			/* Repeated children of one name collapse into a JSON array. A constant offset of
			   zero makes switch_xml_insert() append, so array order becomes document order.
			   That append walks the parent's existing ordered and same-name lists to their
			   tail, so the per-parent width ceiling below - not the array ceiling, and not
			   the node ceiling - is what bounds the quadratic term. */
			if (!xml_curl_json_is_valid_xml_name(member->string)) {
				return 0;
			}
			if (!(elements = cJSON_GetArraySize(member)) || elements > XML_CURL_JSON_MAX_ARRAY_ELEMENTS) {
				/* An empty array expresses nothing that omitting the key would not express,
				   and is not part of the canonical contract. */
				return 0;
			}
			cJSON_ArrayForEach(element, member) {
				if (!cJSON_IsObject(element)) {
					return 0;
				}
				if (++budget->nodes > XML_CURL_JSON_MAX_NODES) {
					return 0;
				}
				/* One charge per element: this is exactly the repeated-key expansion the
				   expanded-output ceiling exists to bound and the payload length must not. */
				if (!xml_curl_json_budget_charge(&budget->name_bytes, strlen(member->string), XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES)) {
					return 0;
				}
				if (++children > XML_CURL_JSON_MAX_CHILDREN_PER_PARENT) {
					return 0;
				}
				if (!(child = switch_xml_add_child_d(node, member->string, 0))) {
					return 0;
				}
				if (!xml_curl_json_to_xml_node(child, element, budget)) {
					return 0;
				}
			}
		} else if (cJSON_IsObject(member)) {
			/* A single child element of this name. */
			if (!xml_curl_json_is_valid_xml_name(member->string)) {
				return 0;
			}
			if (++budget->nodes > XML_CURL_JSON_MAX_NODES) {
				return 0;
			}
			if (!xml_curl_json_budget_charge(&budget->name_bytes, strlen(member->string), XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES)) {
				return 0;
			}
			if (++children > XML_CURL_JSON_MAX_CHILDREN_PER_PARENT) {
				return 0;
			}
			if (!(child = switch_xml_add_child_d(node, member->string, 0))) {
				return 0;
			}
			if (!xml_curl_json_to_xml_node(child, member, budget)) {
				return 0;
			}
		} else {
			/* Numbers, booleans, nulls and raw values have no guaranteed lexical round trip
			   through the const char * builders, so they are a translation error rather than
			   an implicit coercion. The lexical gate already refuses these tokens; this arm
			   keeps the invariant local to the translator as well. */
			return 0;
		}
	}

	/* Only the success path unwinds the depth counter. Every failure abandons the whole
	   translation, so there is no sibling left to account for. */
	budget->depth--;

	return 1;
}

/*
 * JSON: BadgerFish adapter and root entry point. Translates a JSON provisioning response into the
 * same kind of switch_xml_t the XML path produces, or returns NULL on any error. The
 * <document type="freeswitch/xml"><section name="..."> envelope that switch_xml_locate() searches
 * for is synthesised here: a tree without it could not be resolved by the core. Only the
 * duplicating builder variants are used, because the cJSON tree owning every name and value is
 * released before the result is returned.
 *
 * requested_section is the section the core asked this binding for, and the single top-level key
 * of the payload must be exactly that. Accepting whatever key the response happened to carry, and
 * naming the section after it, would let a gateway answer a directory lookup with a configuration
 * document: the decode would report success, the XML path would be skipped, and the core would
 * see a section it never requested - or, worse, resolve nothing and quietly fall back to static
 * local configuration. Requiring the two to agree removes that whole class of substitution.
 */
static switch_xml_t xml_curl_json_to_xml(const char *json_text, const char *requested_section)
{
	cJSON *json = NULL;
	cJSON *root_member = NULL;
	cJSON *envelope_member = NULL;
	const char *parse_end = NULL;
	switch_xml_t document = NULL;
	switch_xml_t section = NULL;
	xml_curl_json_budget_t budget;

	if (zstr(json_text) || zstr(requested_section)) {
		return NULL;
	}

	/* The section name is core-supplied rather than remote, but it still ends up in a builder,
	   so it is held to the same alphabet as every other name in the document. */
	if (!xml_curl_json_is_valid_xml_name(requested_section)) {
		return NULL;
	}

	/* Lexical gate first: the parser never sees a payload outside the documented profile. */
	if (!xml_curl_json_validate_text(json_text)) {
		return NULL;
	}

	/* cJSON_Parse() stops at the end of the first complete value and reports success even when
	   the buffer continues, so a document of "{...}GARBAGE" would be accepted. Requiring the
	   parse to land on the terminator refuses the trailing bytes instead of ignoring them, and
	   the returned end pointer is checked as well rather than trusted. */
	if (!(json = cJSON_ParseWithOpts(json_text, &parse_end, 1))) {
		return NULL;
	}

	if (!parse_end || *parse_end != '\0') {
		cJSON_Delete(json);
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

	/* The sole top-level key must name the section that was actually requested. */
	if (strcmp(root_member->string, requested_section)) {
		cJSON_Delete(json);
		return NULL;
	}

	/*
	 * JSON: the <document>/<section> envelope belongs to this adapter, not to the response, so
	 * the payload may not carry attributes for it. The root member is translated INTO the
	 * <section> element, and switch_xml_set_attr() replaces an existing attribute in place
	 * rather than appending a second one - so a root-level "@name" would silently rewrite the
	 * section name that was set from requested_section a few lines below. A response could
	 * otherwise answer a directory lookup with section name="result", which switch_xml_locate()
	 * reads as a deliberate "not found" and satisfies from static local configuration instead:
	 * the decode would report success, no fallback warning would be emitted, and the XML parse
	 * would be skipped. Refusing every root-level attribute member removes that whole class of
	 * substitution, costs nothing the canonical contract uses - the envelope's only attribute is
	 * the section name, and no provisioning document places attributes at this level - and
	 * converges on the ordinary fallback like every other translation error.
	 */
	cJSON_ArrayForEach(envelope_member, root_member) {
		if (envelope_member->string && envelope_member->string[0] == '@') {
			cJSON_Delete(json);
			return NULL;
		}
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
	switch_xml_set_attr_d(section, "name", requested_section);

	/* JSON: the payload length bounds the DECODED VALUE volume only, since every value appears
	   exactly once in the payload and escape sequences only shrink. Names are accounted separately
	   against XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES because an array writes one key once per
	   element. */
	memset(&budget, 0, sizeof(budget));
	budget.max_text_bytes = strlen(json_text);

	if (!xml_curl_json_to_xml_node(section, root_member, &budget)) {
		switch_xml_free(document);
		cJSON_Delete(json);
		return NULL;
	}

	/*
	 * JSON: re-assert the envelope's own attribute once the translation has finished. The loop
	 * above already refuses the only input that could have disturbed it, so this is an invariant
	 * guard rather than a repair: the section a caller receives is the section that was
	 * requested, unconditionally and however the translator evolves. Re-setting is leak free -
	 * switch_xml_set_attr() releases the previous duplicate when it replaces a value it had
	 * duplicated itself - and it does not reorder the attribute, so the serialisation is
	 * unchanged.
	 */
	switch_xml_set_attr_d(section, "name", requested_section);

	cJSON_Delete(json);

	return document;
}

/* JSON: renders untrusted metadata for a log line. Copies at most buflen-1 bytes and replaces
   every byte outside printable ASCII with '.', so a response header cannot inject a newline to
   forge a second log entry, cannot smuggle terminal control sequences, and cannot grow the line
   without bound. Always returns a NUL-terminated buf. */
static const char *xml_curl_json_sanitize_token(const char *token, char *buf, switch_size_t buflen)
{
	switch_size_t i = 0;

	if (!buf || buflen < 2) {
		return "";
	}

	if (zstr(token)) {
		switch_copy_string(buf, "(absent)", buflen);
		return buf;
	}

	while (i < buflen - 1 && token[i]) {
		buf[i] = (token[i] >= 0x20 && token[i] <= 0x7e) ? token[i] : '.';
		i++;
	}

	buf[i] = '\0';

	return buf;
}

/* JSON: renders a gateway URL for a log line with its secrets removed. Only the scheme and the
   authority survive: a configured gateway-url legitimately carries userinfo, query strings
   routinely carry tokens, and a provisioning path is just as capable of carrying a credential as
   either of those - a tenant identifier, an account key or a bearer value is commonly one of its
   segments. So the userinfo is replaced with a marker, and everything from the first '/', '?' or
   '#' onwards is collapsed into one marker that records only that something followed the
   authority. The marker is deliberately not a reconstruction of what was there: it reads the same
   whether the original carried a path, a query, a fragment or all three. What is left - scheme,
   host and port - is what an operator needs to identify which gateway degraded, and it is the
   most that can be logged without persisting a secret. The result is sanitized and bounded like
   any other logged metadata. */
static const char *xml_curl_json_redact_url(const char *url, char *buf, switch_size_t buflen)
{
	char scratch[256] = "";
	const char *host = NULL;
	const char *at = NULL;
	const char *p = NULL;
	switch_size_t len = 0;

	if (!buf || buflen < 2) {
		return "";
	}

	if (zstr(url)) {
		switch_copy_string(buf, "(none)", buflen);
		return buf;
	}

	/* Everything up to and including "://" is the scheme and is safe to keep verbatim. */
	if ((host = strstr(url, "://"))) {
		host += 3;
	} else {
		host = url;
	}

	/* Userinfo, when present, ends at the last '@' before the start of the path. */
	for (p = host; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
		if (*p == '@') {
			at = p;
		}
	}

	len = (switch_size_t) (host - url);

	if (len >= sizeof(scratch)) {
		len = sizeof(scratch) - 1;
	}

	memcpy(scratch, url, len);
	scratch[len] = '\0';

	if (at) {
		switch_snprintf(scratch + len, sizeof(scratch) - len, "%s", "[redacted]@");
		host = at + 1;
	}

	len = strlen(scratch);

	/* Copy the authority only - host and port - stopping at the path, the query or the
	   fragment. This loop is bounded by scratch, so a pathological URL truncates rather than
	   overflowing. */
	for (p = host; *p && *p != '/' && *p != '?' && *p != '#' && len < sizeof(scratch) - 1; p++) {
		scratch[len++] = *p;
	}

	scratch[len] = '\0';

	/* Anything at all after the authority is replaced wholesale, path included. Testing *p
	   rather than the three delimiters also covers the truncation case above: if the authority
	   itself did not fit, the marker records that the rendering is incomplete. */
	if (*p) {
		switch_snprintf(scratch + len, sizeof(scratch) - len, "%s", "/[redacted]");
	}

	return xml_curl_json_sanitize_token(scratch, buf, buflen);
}

/*
 * JSON: appends one header to a curl slist without ever losing the list it was given.
 * switch_curl_slist_append() returns NULL when it cannot allocate, and libcurl leaves the
 * original list untouched in that case - so assigning the result straight back to the variable
 * would drop every header appended so far, leak them, and send a request whose headers depend on
 * an allocation outcome. Appending through a temporary keeps the caller's list intact and lets
 * the caller decide what to do about the failure. Returns 1 when the header was added.
 */
static int xml_curl_json_append_header(switch_curl_slist_t **list, const char *header)
{
	switch_curl_slist_t *appended = NULL;

	if (!list || zstr(header)) {
		return 0;
	}

	if (!(appended = switch_curl_slist_append(*list, header))) {
		return 0;
	}

	*list = appended;

	return 1;
}

/* JSON: the response decode step reached from the single format dispatch point in
   xml_url_fetch(). Validates the response Content-Type, reads the capped temporary file and runs
   the BadgerFish translation for the section that was requested. Every failure edge - and only a
   failure edge - emits one SWITCH_LOG_WARNING and returns NULL, so the caller falls through to the
   XML parse; a fallback is a degradation worth surfacing rather than a failure.

   None of the warning's operands is trustworthy, so each is rendered through a bounding helper
   first: the URL is credential-bearing, the Content-Type comes from the remote gateway, and the
   section comes from whoever asked for the lookup, since the xml_locate API passes its argument
   straight into switch_xml_locate(). Logging any of them verbatim would let a newline forge a
   second log entry or a control sequence reach an operator's terminal. cJSON_GetErrorPtr() is not
   consulted: it is process global while this code runs on many fetch threads at once, so it could
   report an unrelated thread's error. */
static switch_xml_t xml_curl_json_decode_response(const char *filename, const char *content_type, switch_size_t max_bytes, const char *url,
												  const char *section)
{
	switch_xml_t xml = NULL;
	char *json_text = NULL;
	char safe_url[256] = "";
	char safe_content_type[96] = "";
	char safe_section[64] = "";
	const char *reason = "unknown translation error";

	if (zstr(filename)) {
		reason = "no response body was captured";
	} else if (zstr(section)) {
		reason = "the requested provisioning section is unknown";
	} else if (!xml_curl_json_is_json_content_type(content_type)) {
		reason = "response Content-Type is not application/json";
	} else if (!(json_text = xml_curl_json_read_file(filename, max_bytes))) {
		reason = "response body could not be read in full";
	} else if (!(xml = xml_curl_json_to_xml(json_text, section))) {
		reason = "response body is not a well-formed BadgerFish JSON document for the requested section";
	}

	switch_safe_free(json_text);

	if (!xml) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "JSON decode of the [%s] response from [%s] failed (%s) [Content-Type: %s]; falling back to XML parsing\n",
						  xml_curl_json_sanitize_token(section, safe_section, sizeof(safe_section)),
						  xml_curl_json_redact_url(url, safe_url, sizeof(safe_url)), reason,
						  xml_curl_json_sanitize_token(content_type, safe_content_type, sizeof(safe_content_type)));
	}

	return xml;
}

/*
 * SECURITY: decides whether a configured cookie jar path may be handed to libcurl.
 *
 * libcurl opens the jar for writing at the end of every transfer. That open follows symbolic links
 * and creates a missing file with whatever the process umask happens to be, so an unvalidated path
 * lets any local user who can create that name choose which file FreeSWITCH truncates, and leaves a
 * freshly created jar readable by everyone even though it holds session cookies for the provisioning
 * gateway. Neither outcome is acceptable for a file the module writes as a side effect of a lookup,
 * and the shipped sample historically pointed at a predictable name in the shared temporary
 * directory, which is exactly the precondition such an attack needs.
 *
 * The gate is conservative and says nothing at all on success:
 *   - the directory holding the jar must not be writable by other users unless it carries the sticky
 *     bit, because otherwise the name can be renamed away and re-created between this check and
 *     libcurl's open, which would make every check below unreliable rather than merely incomplete;
 *   - an existing jar that is a regular file owned by this process and writable by nobody else is
 *     accepted untouched, so a working deployment keeps working and its mode is never altered;
 *   - a symbolic link, a directory, a device, a FIFO, a file owned by another user, or a file any
 *     other user can write is refused;
 *   - a missing jar is created here, with O_EXCL and no-follow semantics at mode 0600, so libcurl
 *     later truncates a file that is already private instead of creating a public one;
 *   - a path that cannot be classified at all is refused.
 *
 * The directory rule is deliberately not "owner only". A jar belongs in a service directory such as
 * the one $${db_dir} names, and those are conventionally readable and traversable by others; demanding
 * mode 0700 there would refuse the location the shipped sample recommends and turn a hardening into an
 * outage. Requiring the sticky bit where the directory is shared is the weakest rule that still makes
 * the entry itself un-swappable, and it is what makes a correctly configured shared directory usable
 * while the historical sample's predictable name in a world-writable one is not.
 *
 * A refusal costs cookie persistence for this binding and nothing else - the caller still performs
 * the fetch - which is the same graceful degradation the response decode path uses.
 *
 * Windows has neither the link semantics nor the ownership model this check is written against, and
 * its per-user temporary directory is not shared the way /tmp is, so the check is a no-op there and
 * behaviour on that platform is byte-for-byte what it was.
 */
static int xml_curl_cookie_jar_is_usable(const char *path, const char *url)
{
#ifdef WIN32
	return !zstr(path);
#else
	char safe_path[256] = "";
	char safe_url[256] = "";
	char dir[1024] = "";
	const char *reason = NULL;
	char *slash = NULL;
	struct stat st;

	if (zstr(path)) {
		return 0;
	}

	/* The directory first, because if the entry can be swapped nothing learned about it afterwards
	   means anything. stat() and not lstat() here: a symbolic link to a safe directory is safe. */
	if (strlen(path) >= sizeof(dir)) {
		reason = "its path is too long to be validated";
	} else {
		switch_copy_string(dir, path, sizeof(dir));

		if ((slash = strrchr(dir, '/'))) {
			/* keep the root slash itself, otherwise the parent name would become empty */
			if (slash == dir) {
				dir[1] = '\0';
			} else {
				*slash = '\0';
			}
		} else {
			switch_copy_string(dir, ".", sizeof(dir));
		}

		if (stat(dir, &st) != 0) {
			reason = "the directory holding it could not be read";
		} else if (!S_ISDIR(st.st_mode)) {
			reason = "the path holding it is not a directory";
		} else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !(st.st_mode & S_ISVTX)) {
			reason = "the directory holding it is writable by other users and not sticky, so the name could be replaced after it is checked";
		}
	}

	/* Then the entry itself. lstat() rather than stat(): the question is what the name is, not what
	   it points at. */
	if (!reason) {
		int fd;

		if (lstat(path, &st) == 0) {
			if (!S_ISREG(st.st_mode)) {
				reason = "it is not a regular file, so a symbolic link, directory or special file would be written through";
			} else if (st.st_uid != geteuid()) {
				reason = "it belongs to another user";
			} else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
				reason = "other users can write it, so its contents cannot be trusted";
			} else {
				return 1;
			}
		} else if (errno != ENOENT) {
			reason = "its status could not be read";
		} else if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)) < 0) {
			reason = "it does not exist and could not be created privately";
		} else {
			close(fd);
			return 1;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					  "Refusing cookie file [%s] for the binding at [%s] because %s; cookies will not be persisted for this binding\n",
					  xml_curl_json_sanitize_token(path, safe_path, sizeof(safe_path)),
					  xml_curl_json_redact_url(url, safe_url, sizeof(safe_url)), reason);

	return 0;
#endif
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
	char safe_url[256] = "";		/* JSON: redacted rendering of the gateway URL, for logging only */
	char safe_section[64] = "";
	char safe_tag[64] = "";
	char safe_key[64] = "";
	char safe_type[96] = "";
	int json_response = 0;			/* JSON: 1 once this fetch has actually negotiated JSON */
	switch_curl_slist_t **request_headers = NULL;	/* JSON: the list curl is finally handed */

    strncpy(hostname, switch_core_get_switchname(), sizeof(hostname) - 1);

	if (!binding) {
		return NULL;
	}

	/* JSON: the format decision is taken once, here, and then only consulted. Keeping it in a
	   local is what lets the request path lower it again if the Accept header cannot be sent,
	   so the decode step can never expect a representation this fetch did not ask for. */
	json_response = (binding->response_format && !strcasecmp(binding->response_format, "json")) ? 1 : 0;

	/* JSON: Accept has to land on the list curl is finally handed, which is not always the list the
	   other request headers are built on - when disable100continue is set, the Expect: branch below
	   hands curl its own list. Selecting the target once here keeps the format decision single. */
	request_headers = binding->disable100continue ? &slist : &headers;

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

		/* JSON: the only place the request path acts on the format decision, and it only appends -
		   every other request option keeps its value. switch_curl_slist_free_all() below releases
		   whichever list carries this entry. When the header cannot be added the request is still
		   made, and the format decision is lowered so the response is decoded as XML. */
		if (json_response && !xml_curl_json_append_header(request_headers, "Accept: application/json")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "Could not add the Accept: application/json request header for [%s]; continuing without JSON "
							  "negotiation, so the response will be decoded as XML\n",
							  xml_curl_json_redact_url(binding->url, safe_url, sizeof(safe_url)));
			json_response = 0;
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
			if (json_response) {
				/* JSON: this list already carries the Accept entry, so the append goes through the
				   non-destructive helper. Assigning the result straight back would, on an
				   allocation failure, replace a list carrying Accept with NULL, leak it and hand
				   curl an empty header set while the fetch still believed it had negotiated JSON.
				   Keeping the list means the worst case is only that suppression is not applied. */
				if (!xml_curl_json_append_header(&slist, "Expect:")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
									  "Could not add the Expect: request header for [%s]; 100-continue suppression is not applied\n",
									  xml_curl_json_redact_url(binding->url, safe_url, sizeof(safe_url)));
				}
			} else {
				/* An XML-only binding has nothing on this list that an allocation failure could
				   discard, so it appends directly and stays silent. */
				slist = switch_curl_slist_append(slist, "Expect:");
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

		/* SECURITY: validate the jar path before libcurl is allowed to open it. The two setopt calls
		   below are unchanged; the only difference is that they are now reached exclusively for a
		   path that is safe to truncate and to create. The check lives here, at fetch time, rather
		   than in do_config(): the cookie-file parameter keeps its existing name, default and
		   semantics, and a jar that is replaced by a symbolic link after the module loaded is caught
		   just the same. */
		if (binding->cookie_file && xml_curl_cookie_jar_is_usable(binding->cookie_file, binding->url)) {
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
		   use-after-free. libcurl returns NULL when the response carried no Content-Type.
		   The probe is unconditional, so it also runs for a binding that never asked for JSON;
		   there it only fills a local buffer that nothing goes on to consult. */
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
			/* JSON: the single response-format dispatch point. When this fetch negotiated JSON,
			   attempt the BadgerFish decode first. The helper emits one WARNING on every failure
			   edge and returns NULL, so a non-JSON Content-Type, a body that could not be read
			   in full, unparseable JSON, a document naming a section other than the one that was
			   requested, and a document outside the canonical contract all converge on the XML
			   parse below. */
			if (json_response) {
				xml = xml_curl_json_decode_response(filename, content_type, binding->curl_max_bytes, binding->url, section);
			}

			/* JSON: parse the response as XML whenever the JSON decode did not produce a document.
			   The guard is always taken for a binding that never asked for JSON, so every JSON
			   failure edge and the XML path share one place where a decode failure is reported. */
			if (!xml) {
				if (!(xml = switch_xml_parse_file(filename))) {
					if (json_response) {
						/* JSON: an opted-in binding reports the same failure without persisting a
						   credential. A gateway-url legitimately carries userinfo and query
						   tokens, and `data' is the POST form body, which ends with the lookup's
						   key_value and carries every event variable the binding mapped into it.
						   Neither is diagnostically necessary, so the URL is redacted to its
						   authority, the lookup is summarised by its coordinates only - never
						   key_value - and the response is described by its Content-Type and size.
						   Every rendering is sanitized and bounded, so a remote Content-Type
						   cannot forge a second log line. */
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
										  "Error Parsing Result! [%s]\nrequest: section [%s] tag_name [%s] key_name [%s]; response: content-type [%s], %ld bytes\n",
										  xml_curl_json_redact_url(binding->url, safe_url, sizeof(safe_url)),
										  xml_curl_json_sanitize_token(section, safe_section, sizeof(safe_section)),
										  xml_curl_json_sanitize_token(tag_name, safe_tag, sizeof(safe_tag)),
										  xml_curl_json_sanitize_token(key_name, safe_key, sizeof(safe_key)),
										  xml_curl_json_sanitize_token(content_type, safe_type, sizeof(safe_type)),
										  (long) config_data.bytes);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Parsing Result! [%s]\ndata: [%s]\n", binding->url, data);
					}
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
		char safe_url[256] = "";		/* SECURITY: redacted rendering of the gateway URL, for logging only */
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
				/* JSON: opt-in response representation. This arm sits at the tail of the chain
				   so every other parameter keeps its evaluation order; absence selects XML. */
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

		/* SECURITY: a gateway URL legitimately carries HTTP userinfo and query parameters, and both
		   are routinely credential bearing. This registration notice is written once per binding at
		   module load, so an unredacted rendering would persist a provisioning secret to the log file
		   for the lifetime of that log. Render it through the same helper the response diagnostics
		   use - the helper only needs a URL and is not JSON specific despite its name - so that the
		   scheme and authority remain diagnosable while userinfo, path, query and fragment do not
		   reach the log. Everything else about the message, including the binding name and the
		   section list, is unchanged. */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Binding [%s] XML Fetch Function [%s] [%s]\n",
						  zstr(bname) ? "N/A" : bname, xml_curl_json_redact_url(binding->url, safe_url, sizeof(safe_url)),
						  binding->bindings ? binding->bindings : "all");
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
