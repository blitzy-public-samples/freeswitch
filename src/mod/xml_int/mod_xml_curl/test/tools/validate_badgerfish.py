#!/usr/bin/env python3
#
# FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
# Copyright (C) 2005-2024, Anthony Minessale II <anthm@freeswitch.org>
#
# Version: MPL 1.1
#
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
#
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
#
# The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
#
# The Initial Developer of the Original Code is
# Anthony Minessale II <anthm@freeswitch.org>
# Portions created by the Initial Developer are Copyright (C)
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
# Blitzy Agent <agent@blitzy.com>
#
# validate_badgerfish.py -- standalone mod_xml_curl BadgerFish contract validator
#
"""Prove that a provisioning gateway's JSON output conforms to the BadgerFish
contract mod_xml_curl accepts, BEFORE the gateway is deployed.

WHO THIS IS FOR
---------------
The backend team that owns a provisioning gateway. mod_xml_curl's JSON decoder
accepts a deliberately restricted profile, and anything outside it is silently
degraded: the module logs a warning and parses the response as XML instead, so a
non-conformant gateway does not fail loudly -- it just never gets used. Running
this validator against real gateway output turns that silent degradation into a
verdict you can gate a deployment on.

USAGE
-----
    # one response captured from your gateway
    python3 validate_badgerfish.py /path/to/response.json

    # a whole directory of captured responses
    python3 validate_badgerfish.py /path/to/captures/

    # your gateway serves a section name of its own
    python3 validate_badgerfish.py --sections configuration,directory /tmp/r.json

    # only the exit code matters (CI gate)
    python3 validate_badgerfish.py --quiet captures/ && echo conformant

The conformance corpus that ships with FreeSWITCH is the parity fixture set one
directory up, in ../fixtures/: every *.json file whose name starts with
"configuration_", "directory_" or "dialplan_" is a conformant reference document
that this validator accepts, and each has an .xml twin that mod_xml_curl's own
test suite proves it translates into byte for byte. The five
badgerfish_invalid_*.json files in the same directory are deliberate
counter-examples, one per contract rule, and this validator rejects each of them
with a named violation. Start from a fixture that resembles your own payload.

EXIT STATUS
-----------
    0   every input conforms
    1   at least one input violates the contract
    2   usage or I/O error -- distinct on purpose, so a mistyped path can never be
        mistaken for a verdict. This covers a bad option, a path that names
        nothing, a file that cannot be read, and three refusals this tool makes
        deliberately because its input is untrusted: a symbolic link (pass what it
        resolves to if that is really the intent), anything that is not a regular
        file (a FIFO, a socket, a device -- none has a bounded body), and a
        directory whose entries cannot be listed.

SAFETY WITH UNTRUSTED INPUT
---------------------------
This tool is pointed at output captured from a gateway, so it treats that output
as hostile rather than merely malformed. Three properties follow, and each is a
deliberate refusal rather than a best effort:

  * A body larger than mod_xml_curl's own 1 MiB response ceiling is refused from
    fstat() before anything is allocated, and again as a bounded read in case the
    file grew in between.
  * The decoder's depth, value-count and string-length ceilings are applied to the
    RAW TEXT in a single pass, BEFORE json.loads() is called, so a document that
    breaches them is never materialised as an object graph. This mirrors what the
    module itself does and for the same reason.
  * Every input is opened exactly once, with O_NOFOLLOW, and judged by fstat() on
    that same descriptor. Nothing is checked and then reopened, and a directory's
    entries are opened relative to the descriptor the directory was listed
    through, so there is no window in which a name could be swapped.

Paths are escaped before they are printed, because a filename may legally contain
a newline or an ANSI escape and these lines are what a CI report is built from.

OUTPUT
------
Violations go to STDERR, one line per violation, in this exact form:

    <path>: <VIOLATION_NAME>: <human explanation>

VIOLATION_NAME is a stable identifier suitable for grep and for a CI report; the
explanation names the offending JSON path. Every violation in a file is reported,
not just the first, so a gateway author can fix a payload in one pass -- with one
deliberate exception: a document that is STRUCTURALLY broken (an unterminated
string, a mismatched bracket, a scalar where the profile allows none, nesting past
the ceiling) is reported and then abandoned, because past such a point the pass no
longer knows where it is in the document and every further finding would describe a
shape it has lost track of.
STDOUT carries only the per-file "OK" lines that --verbose adds and the final
summary that --quiet removes, so redirecting stdout to /dev/null still leaves
every violation visible.

REQUIREMENTS
------------
python3 and its standard library. Nothing else -- no pip install, no FreeSWITCH
build, no running FreeSWITCH, no network access. This file writes nothing to the
filesystem.

WHAT IS CHECKED
---------------
The rules and every numeric ceiling below mirror the decoder in
src/mod/xml_int/mod_xml_curl/mod_xml_curl.c. See the CEILINGS table and the
RULES commentary further down for the specific function each check mirrors.
"""

import argparse
import errno
import json
import os
import re
import stat
import sys

# ---------------------------------------------------------------------------
# CEILINGS -- one table, mirroring mod_xml_curl.c L185-L192 (plus the derived
# ceiling at L203). These are the resource limits the decoder enforces so that
# a runaway or hostile response cannot exhaust memory. They are gathered here,
# and nowhere else in this file, so that drift between the validator and the
# module is visible as a diff in one place.
#
#   mod_xml_curl.c #define               value    meaning
#   -----------------------------------  -------  ----------------------------
#   XML_CURL_JSON_MAX_DEPTH                   32  nesting levels
#   XML_CURL_JSON_MAX_VALUES               50000  objects + arrays + strings
#   XML_CURL_JSON_MAX_OBJECT_MEMBERS         256  members of one object
#   XML_CURL_JSON_MAX_ARRAY_ELEMENTS         256  repeated children under one name
#   XML_CURL_JSON_MAX_CHILDREN_PER_PARENT    256  child elements under one parent
#   XML_CURL_JSON_MAX_NODES                20000  elements + attributes built
#   XML_CURL_JSON_MAX_STRING_BYTES          8192  bytes in one name or one value
#   XML_CURL_JSON_MAX_NAME_BYTES             128  bytes in one name
#
# The last entry is not a #define of its own in the module but the derived
# ceiling at L203, XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES, which bounds the
# cumulative NAME bytes the translation writes into the tree. It is a ceiling of
# its own rather than the payload length because an array writes one key once
# per element.
# ---------------------------------------------------------------------------
MAX_DEPTH = 32
MAX_VALUES = 50000
MAX_OBJECT_MEMBERS = 256
MAX_ARRAY_ELEMENTS = 256
MAX_CHILDREN_PER_PARENT = 256
MAX_NODES = 20000
MAX_STRING_BYTES = 8192
MAX_NAME_BYTES = 128
MAX_TRANSFORMED_NAME_BYTES = MAX_NODES * (MAX_NAME_BYTES + 1)

# The response body ceiling, mirroring XML_CURL_MAX_BYTES (mod_xml_curl.c L76).
# mod_xml_curl streams a response to a temporary file through a write callback
# that stops at this many bytes, so a payload larger than this is one the module
# would never decode at all -- and it is also the bound that makes this validator
# safe to point at untrusted output. It is applied from fstat() BEFORE anything is
# allocated, and again as a bounded read, because a file can grow between the two.
MAX_RESPONSE_BYTES = 1024 * 1024

# How much of a path this tool will echo back. A path arrives from the command
# line or from a directory listing, so it is untrusted text that ends up in a CI
# log; render_path() escapes it and this bounds the result, because an escaped
# rendering of a pathological name can be several times longer than the name.
MAX_RENDERED_PATH_CHARS = 512

# The provisioning sections mod_xml_curl binds by default, and therefore the
# only names the single top-level key of a response may carry. The module
# compares that key against the section the FreeSWITCH core asked the binding
# for, which this validator cannot know; membership in this set is the
# strongest statement that can be made from a captured payload alone, which is
# exactly why the set is explicit here and overridable with --sections.
DEFAULT_SECTIONS = ("configuration", "directory", "dialplan")

# The two element names the translator synthesises for the envelope. A payload
# whose top-level key is either of them has emitted the envelope the translator
# owns -- see ENVELOPE_ATTRIBUTES_EMITTED below.
ENVELOPE_KEYS = ("document", "section")

# BadgerFish markers.
ATTRIBUTE_PREFIX = "@"
TEXT_KEY = "$"

# Every JSON string literal in a document, matched on the RAW text. A valid
# JSON document cannot contain an unescaped quote inside a string, so this
# pattern cannot be fooled once json.loads() has accepted the document. It is
# used for the one measurement the decoded tree cannot supply: the length of a
# string as it is ENCODED, which is what the module's lexical gate bounds.
STRING_LITERAL_RE = re.compile(r'"(?:[^"\\]|\\.)*"')

# Exit statuses. Documented in the module docstring above and in --help.
EXIT_CONFORMANT = 0
EXIT_VIOLATION = 1
EXIT_USAGE = 2


def render_path(path):
    """Render an untrusted path as one line of printable, bounded ASCII.

    Every path this tool echoes comes from the command line or from a directory
    listing, and a filename may legally contain a carriage return, a newline, an
    ESC or any other control byte. Emitted verbatim into a CI log those bytes
    forge structure: a name carrying "\\n" splits one diagnostic into two, and one
    carrying an ANSI escape rewrites what an operator sees. Since the diagnostic
    format is "<path>: <NAME>: <explanation>", a forged line is indistinguishable
    from a real verdict about a different file.

    So the path is escaped rather than printed: backslash first (or the escaping
    would be ambiguous), then everything outside printable ASCII as a numeric
    escape. Non-ASCII is escaped too -- a legitimate UTF-8 filename becomes less
    pretty, which is the right trade for output that cannot be restructured by
    its own content. The result is bounded, because an escaped rendering can be
    up to six times the length of what it renders.
    """
    if isinstance(path, bytes):
        text = path.decode("utf-8", "replace")
    else:
        text = path

    out = []
    width = 0
    truncated = False

    for char in text:
        code = ord(char)

        if char == "\\":
            piece = "\\\\"
        elif 0x20 <= code < 0x7F:
            piece = char
        elif code <= 0xFF:
            piece = "\\x%02x" % (code,)
        elif code <= 0xFFFF:
            piece = "\\u%04x" % (code,)
        else:
            piece = "\\U%08x" % (code,)

        if width + len(piece) > MAX_RENDERED_PATH_CHARS:
            truncated = True
            break

        out.append(piece)
        width += len(piece)

    if truncated:
        out.append("...")

    return "".join(out)


class Violation(object):
    """One named contract breach, bound to the file and JSON path that caused it.

    Kept as a small value object rather than a formatted string so that the
    reporting format lives in exactly one place (see format_line) and so that
    duplicate findings can be collapsed by identity.
    """

    __slots__ = ("path", "name", "detail")

    def __init__(self, path, name, detail):
        self.path = path
        self.name = name
        self.detail = detail

    def identity(self):
        return (self.path, self.name, self.detail)

    def format_line(self):
        # The path is rendered, not interpolated: it is untrusted text and this is
        # the line a CI report is built from.
        return "%s: %s: %s" % (render_path(self.path), self.name, self.detail)


class DuplicateMemberError(ValueError):
    """Raised out of the object_pairs_hook when one object repeats a key.

    json.loads() would otherwise resolve a repeated key last-wins and hand back
    a dict in which the duplicate is invisible, while mod_xml_curl's
    xml_curl_json_check_object() refuses the document outright: cJSON preserves
    both members, so an "@id" twice would be last-wins on an attribute while a
    repeated child key would silently become two elements. An array is the one
    and only contractual way to express repetition.
    """

    def __init__(self, key):
        ValueError.__init__(self, key)
        self.key = key


class NonStringConstantError(ValueError):
    """Raised out of parse_constant for NaN, Infinity and -Infinity.

    Python's json accepts those three by default as an extension. They are not
    JSON, they are not strings, and the module's lexical gate refuses the tokens
    before its parser ever sees them.
    """

    def __init__(self, token):
        ValueError.__init__(self, token)
        self.token = token


def object_pairs_hook(pairs):
    """Build a dict while refusing a repeated key.

    Mirrors the duplicate-member half of xml_curl_json_check_object()
    (mod_xml_curl.c L830-L838).
    """
    seen = {}

    for key, value in pairs:
        if key in seen:
            raise DuplicateMemberError(key)
        seen[key] = value

    return seen


def parse_constant(token):
    """Refuse the three non-JSON constants Python's parser would otherwise accept."""
    raise NonStringConstantError(token)


def is_xml_char(code_point):
    """True when a code point may appear in XML 1.0 character data.

    Mirrors xml_curl_json_is_xml_char() (mod_xml_curl.c L306). Tab, newline and
    carriage return are the only control characters XML permits; NUL, backspace,
    form feed and the rest of C0 are forbidden, as are the surrogate range and
    the two permanently invalid code points.
    """
    if code_point in (0x09, 0x0A, 0x0D):
        return True

    if code_point < 0x20:
        return False

    if 0xD800 <= code_point <= 0xDFFF:
        return False

    if code_point in (0xFFFE, 0xFFFF):
        return False

    return code_point <= 0x10FFFF


def is_name_char(char, first):
    """One character of the element and attribute name alphabet.

    Mirrors xml_curl_json_is_name_char() (mod_xml_curl.c L408). A deliberate
    subset of the XML Name production, because a JSON member name reaches
    switch_xml_add_child_d() verbatim and the serializer writes names without
    escaping: a name such as x></x><evil would otherwise forge document
    structure.
    """
    if ("A" <= char <= "Z") or ("a" <= char <= "z") or char == "_":
        return True

    if not first and (("0" <= char <= "9") or char in ("-", ".")):
        return True

    return False


def is_valid_xml_name(name):
    """True when a string may be used as an element or attribute name.

    Mirrors xml_curl_json_is_valid_xml_name() (mod_xml_curl.c L424). A single
    colon is accepted as a namespace separator and both halves must then be
    well-formed names in their own right, so ":x", "x:" and "a:b:c" are all
    refused rather than passed through as malformed namespace syntax.

    The MAX_NAME_BYTES ceiling is deliberately NOT applied here: an over-long
    name is reported as its own ceiling violation so that a gateway author sees
    "too long" rather than the misleading "illegal character".
    """
    if not name:
        return False

    colons = 0
    first = True

    for char in name:
        if char == ":":
            colons += 1

            if colons > 1 or first:
                # a leading colon, or a second one
                return False

            # the local part after the colon must start a name of its own
            first = True
            continue

        if not is_name_char(char, first):
            return False

        first = False

    # first is still set only when the name ended on a colon
    return not first


def invalid_text_reason(text):
    """Explain why a decoded string may not be handed to the XML builders.

    Mirrors xml_curl_json_is_valid_text() (mod_xml_curl.c L352) for everything
    that survives JSON decoding. Returns None when the string is usable.

    The two-character sequence "<!" is refused outright because
    switch_xml_ampencode() special-cases a '<' whose next byte is '!': it emits
    the '<' raw and switches into an immune mode that copies every remaining
    byte verbatim, so a value of <![CDATA[..]]> would leave the escaping regime
    altogether and let a response forge attribute and element syntax in the
    serialised document. A lone '<' is still fine, because it never reaches
    that branch.
    """
    for index, char in enumerate(text):
        code_point = ord(char)

        if not is_xml_char(code_point):
            return ("code point U+%04X at offset %d is not legal XML character data"
                    % (code_point, index))

        if char == "<" and text[index + 1:index + 2] == "!":
            return ("the two-character sequence '<!' at offset %d would escape the "
                    "serializer's escaping regime" % (index,))

    return None


def measure_structure(value, depth=1):
    """Return (deepest structural nesting, total value count) for a decoded document.

    Mirrors the two document-wide counters of the lexical gate,
    xml_curl_json_validate_text() (mod_xml_curl.c L546-L660), which walks the
    RAW payload and therefore counts things the semantic walk below does not:

      * Nesting: the gate pushes a level for EVERY '{' and EVERY '[', so an
        array contributes a level of its own. The translator's own depth
        counter does not -- there, only element objects nest -- which is why
        both measures are taken and both are bounded by MAX_DEPTH. The
        object -> array -> object shape therefore consumes two structural
        levels per element level, and a document can breach the gate's ceiling
        while its element nesting is comfortably inside it.
      * Values: the gate counts one for every '{', every '[' and every string
        literal, and a member KEY is a string literal too. Counting only the
        decoded values would undercount a document by roughly half.
    """
    if isinstance(value, dict):
        deepest = depth
        # one for this object, plus one per member key
        count = 1 + len(value)

        for member in value.values():
            member_depth, member_count = measure_structure(member, depth + 1)
            deepest = max(deepest, member_depth)
            count += member_count

        return deepest, count

    if isinstance(value, list):
        deepest = depth
        count = 1

        for element in value:
            element_depth, element_count = measure_structure(element, depth + 1)
            deepest = max(deepest, element_depth)
            count += element_count

        return deepest, count

    # A string is one value; a number, boolean or null is not counted here at
    # all, because the gate refuses those tokens outright and the semantic walk
    # reports them as NON_STRING_LEAF.
    return depth - 1, 1 if isinstance(value, str) else 0


def describe(value):
    """Name a decoded JSON value the way a gateway author would recognise it."""
    if value is True or value is False:
        return "boolean"

    if value is None:
        return "null"

    if isinstance(value, str):
        return "string"

    if isinstance(value, (int, float)):
        return "number"

    if isinstance(value, dict):
        return "object"

    if isinstance(value, list):
        return "array"

    return type(value).__name__


class DocumentValidator(object):
    """Validates one decoded document against the BadgerFish contract.

    One instance per file. The instance carries the document-wide budget
    counters, because the module's ceilings are document-wide rather than
    per-node: a wide-and-shallow document must not be able to amplify past a
    deep-and-narrow one.
    """

    def __init__(self, path, sections):
        self.path = path
        self.sections = sections
        self.violations = []
        self._seen = set()

        # Document-wide budget, mirroring struct xml_curl_json_budget
        # (mod_xml_curl.c L217-L224). They are instance state rather than
        # per-call locals because the module's ceilings are document-wide.
        self.nodes = 0
        self.name_bytes = 0

        # Latched so a single breach of a document-wide ceiling is reported
        # once rather than once per node past the limit.
        self._reported = set()

    # -- reporting ----------------------------------------------------------

    def add(self, name, detail):
        """Record one violation, collapsing an exact duplicate finding."""
        violation = Violation(self.path, name, detail)
        identity = violation.identity()

        if identity in self._seen:
            return

        self._seen.add(identity)
        self.violations.append(violation)

    def add_once(self, name, detail):
        """Record a document-wide ceiling breach at most once per document."""
        if name in self._reported:
            return

        self._reported.add(name)
        self.add(name, detail)

    # -- budget -------------------------------------------------------------

    def measure(self, document):
        """Apply the two document-wide ceilings the lexical gate enforces."""
        depth, values = measure_structure(document)

        if depth > MAX_DEPTH:
            self.add("MAX_DEPTH_EXCEEDED",
                     "structural nesting reaches %d levels; the ceiling is %d, and every "
                     "JSON object AND every JSON array consumes one level, so the "
                     "object/array alternation a repeated-children array needs costs two "
                     "levels per element level" % (depth, MAX_DEPTH))

        if values > MAX_VALUES:
            self.add("MAX_VALUES_EXCEEDED",
                     "the document carries %d values; the ceiling is %d, counting every "
                     "object, every array and every string including member names"
                     % (values, MAX_VALUES))

    def charge_node(self, where):
        """Count one element or attribute against MAX_NODES."""
        self.nodes += 1

        if self.nodes > MAX_NODES:
            self.add_once("MAX_NODES_EXCEEDED",
                          "the document would build more than %d elements and attributes; "
                          "first seen at %s" % (MAX_NODES, where))

    def charge_name(self, name, where):
        """Count one written name against MAX_TRANSFORMED_NAME_BYTES.

        An array key is charged once per element, which is exactly the
        repeated-key expansion this ceiling exists to bound and the payload
        length must not.
        """
        self.name_bytes += len(name.encode("utf-8"))

        if self.name_bytes > MAX_TRANSFORMED_NAME_BYTES:
            self.add_once("MAX_TRANSFORMED_NAME_BYTES_EXCEEDED",
                          "the names this document would write into the tree exceed %d bytes "
                          "in total; first seen at %s" % (MAX_TRANSFORMED_NAME_BYTES, where))

    # -- the contract -------------------------------------------------------

    def validate(self, document):
        """Entry point. Mirrors xml_curl_json_to_xml() (mod_xml_curl.c L1001)."""
        if not isinstance(document, dict):
            self.add("ROOT_NOT_OBJECT",
                     "the payload is a JSON %s; the contract requires a single JSON object "
                     "at the top level" % (describe(document),))
            return

        # The two document-wide ceilings the lexical gate enforces on the raw
        # payload, taken before anything semantic, because a breach of either
        # means the module refuses the response no matter how well shaped the
        # rest of it is.
        self.measure(document)

        keys = list(document.keys())

        if not keys:
            self.add("NO_TOP_LEVEL_KEY",
                     "the payload carries no top-level key; exactly one is required, naming "
                     "the requested section")
            return

        # The envelope check runs first and, when it fires, is the whole
        # verdict for this document. A payload that emitted the envelope is
        # shaped one level too deep, so every deeper finding would be an
        # artefact of that offset rather than an independent defect -- reporting
        # them would bury the one thing the author has to change. Every other
        # violation in this file is still reported exhaustively; this one case
        # is terminal, and only this one.
        #
        # A section the caller declared with --sections always wins over the
        # envelope reading, so naming a section "document" is not turned into a
        # phantom envelope report.
        if (len(keys) == 1 and keys[0] not in self.sections
                and keys[0].lower() in ENVELOPE_KEYS):
            self.add("ENVELOPE_ATTRIBUTES_EMITTED",
                     "the top-level key is %r, so the payload emits the "
                     "<document>/<section> envelope and its attributes itself; the "
                     "translator owns that envelope and synthesises it from the single "
                     "top-level key, which must name the requested section instead "
                     "(one of: %s)" % (keys[0], ", ".join(self.sections)))
            return

        if len(keys) > 1:
            self.add("MULTIPLE_TOP_LEVEL_KEYS",
                     "the payload carries %d top-level keys (%s); exactly one is required, "
                     "naming the requested section"
                     % (len(keys), ", ".join(repr(key) for key in keys)))

        for key in keys:
            self.validate_section(key, document[key])

    def validate_section(self, key, value):
        """Validate one top-level key and the element it names."""
        where = "$.%s" % (key,)

        if key not in self.sections:
            self.add("UNBOUND_SECTION_KEY",
                     "the top-level key %r does not name a bound provisioning section; "
                     "mod_xml_curl compares it against the section the core requested, so "
                     "it must be one of: %s" % (key, ", ".join(self.sections)))

        if not isinstance(value, dict):
            self.add("ROOT_VALUE_NOT_OBJECT",
                     "the value of the top-level key %r is a JSON %s; it must be an object, "
                     "because it is translated into the <section> element"
                     % (key, describe(value)))
            return

        # The <document>/<section> envelope belongs to the translator, not to
        # the response. The root member is translated INTO the <section>
        # element, and switch_xml_set_attr() replaces an existing attribute in
        # place rather than appending a second one -- so a root-level "@name"
        # would silently rewrite the section name the translator had already
        # set. switch_xml_locate() then reads a <section name="result"> as a
        # deliberate "not found" and satisfies the lookup from static local
        # configuration, so the decode reports success, no fallback warning is
        # emitted, and the caller believes the gateway said nothing exists.
        # Mirrors mod_xml_curl.c L1069-L1078.
        for member in value:
            if member.startswith(ATTRIBUTE_PREFIX):
                self.add("ROOT_ATTRIBUTE",
                         "%s carries the attribute member %r; no attribute is contractual "
                         "directly under the top-level key, because that level IS the "
                         "<section> element the translator owns"
                         % (where, member))

        self.validate_element(value, where, 1)

    def validate_element(self, obj, where, depth):
        """Validate one BadgerFish element object.

        Mirrors xml_curl_json_check_object() (mod_xml_curl.c L797) and
        xml_curl_json_to_xml_node() (L860). depth is the element nesting level,
        counted the way the translator counts it: an array does not add a level
        of its own, the element objects inside it do.
        """
        if depth > MAX_DEPTH:
            self.add_once("MAX_DEPTH_EXCEEDED",
                          "element nesting reaches %d levels at %s; the ceiling is %d"
                          % (depth, where, MAX_DEPTH))
            return

        members = list(obj.keys())

        if len(members) > MAX_OBJECT_MEMBERS:
            self.add("MAX_OBJECT_MEMBERS_EXCEEDED",
                     "%s carries %d members; one object may carry at most %d"
                     % (where, len(members), MAX_OBJECT_MEMBERS))

        has_text = False
        has_children = False
        children = 0

        for member in members:
            if member == TEXT_KEY:
                has_text = True
            elif not member.startswith(ATTRIBUTE_PREFIX):
                has_children = True

        # switch_xml serialisation emits an element's text only when the
        # element has no children, so a node carrying both could not round
        # trip; the BadgerFish convention cannot recover the separate text runs
        # of mixed content either. Mirrors mod_xml_curl.c L827-L829.
        if has_text and has_children:
            self.add("MIXED_CONTENT",
                     "%s carries both %r text and child elements; an element has children "
                     "or text, never both" % (where, TEXT_KEY))

        for member in members:
            value = obj[member]

            if not member:
                self.add("EMPTY_MEMBER_KEY",
                         "%s carries a member with an empty key, which can name neither an "
                         "element, nor an attribute, nor the text marker" % (where,))
                continue

            if member.startswith(ATTRIBUTE_PREFIX):
                self.validate_attribute(member, value, where)
            elif member == TEXT_KEY:
                self.validate_text(value, where)
            elif isinstance(value, list):
                children += self.validate_array(member, value, where, depth)
            elif isinstance(value, dict):
                children += 1
                self.validate_child(member, value, "%s.%s" % (where, member), depth + 1)
            else:
                # Numbers, booleans and nulls have no guaranteed lexical round
                # trip through the const char * builders that receive every
                # name and value, so they are a translation error rather than
                # an implicit coercion: 1 against 1.0, true against "true".
                # Mirrors mod_xml_curl.c L989-L994.
                self.add("NON_STRING_LEAF",
                         "%s.%s is a JSON %s; every leaf value must be a JSON string, and a "
                         "child element must be an object or a non-empty array of objects"
                         % (where, member, describe(value)))

        if children > MAX_CHILDREN_PER_PARENT:
            self.add("MAX_CHILDREN_PER_PARENT_EXCEEDED",
                     "%s would build %d child elements; one parent may carry at most %d"
                     % (where, children, MAX_CHILDREN_PER_PARENT))

    def validate_child(self, name, value, where, depth):
        """Validate one child element and the name it is built under."""
        self.check_name(name, where, "element")
        self.charge_node(where)
        self.charge_name(name, where)
        self.validate_element(value, where, depth)

    def validate_array(self, name, elements, where, depth):
        """Validate a repeated-children array. Returns the child count it adds.

        Mirrors the array arm of xml_curl_json_to_xml_node()
        (mod_xml_curl.c L951-L978).
        """
        member_where = "%s.%s" % (where, name)

        self.check_name(name, member_where, "element")

        if not elements:
            # An empty array expresses nothing that omitting the key would not
            # express, and is not part of the canonical contract.
            self.add("EMPTY_ARRAY",
                     "%s is an empty array; omit the key instead, since an empty array "
                     "expresses nothing the contract can build" % (member_where,))
            return 0

        if len(elements) > MAX_ARRAY_ELEMENTS:
            self.add("MAX_ARRAY_ELEMENTS_EXCEEDED",
                     "%s carries %d elements; one array may carry at most %d"
                     % (member_where, len(elements), MAX_ARRAY_ELEMENTS))

        for index, element in enumerate(elements):
            element_where = "%s[%d]" % (member_where, index)

            if not isinstance(element, dict):
                self.add("NON_OBJECT_ARRAY_ELEMENT",
                         "%s is a JSON %s; every element of a repeated-children array must "
                         "be an object" % (element_where, describe(element)))
                continue

            self.charge_node(element_where)
            self.charge_name(name, element_where)
            self.validate_element(element, element_where, depth + 1)

        return len(elements)

    def validate_attribute(self, member, value, where):
        """Validate one "@"-prefixed attribute member.

        Mirrors the attribute arm of xml_curl_json_to_xml_node()
        (mod_xml_curl.c L887-L913).
        """
        member_where = "%s.%s" % (where, member)

        self.check_name(member[len(ATTRIBUTE_PREFIX):], member_where, "attribute")
        self.charge_node(member_where)
        self.charge_name(member, member_where)

        if not isinstance(value, str):
            self.add("NON_STRING_LEAF",
                     "%s is a JSON %s; an attribute value must be a JSON string"
                     % (member_where, describe(value)))
            return

        self.check_value(value, member_where)

    def validate_text(self, value, where):
        """Validate the "$" text member.

        Mirrors the text arm of xml_curl_json_to_xml_node()
        (mod_xml_curl.c L914-L923).
        """
        member_where = "%s.%s" % (where, TEXT_KEY)

        if not isinstance(value, str):
            self.add("NON_STRING_LEAF",
                     "%s is a JSON %s; element text must be a JSON string"
                     % (member_where, describe(value)))
            return

        self.check_value(value, member_where)

    def check_name(self, name, where, kind):
        """Apply the name alphabet and the name-length ceiling."""
        encoded = len(name.encode("utf-8"))

        if encoded > MAX_NAME_BYTES:
            self.add("MAX_NAME_BYTES_EXCEEDED",
                     "the %s name at %s is %d bytes long; the ceiling is %d"
                     % (kind, where, encoded, MAX_NAME_BYTES))
            return

        if not is_valid_xml_name(name):
            self.add("INVALID_NAME",
                     "%r is not a usable %s name at %s; a name is ASCII letters, digits, "
                     "underscore, hyphen and period, must start with a letter or underscore, "
                     "and may carry at most one colon as a namespace separator with a "
                     "well-formed name on each side" % (name, kind, where))

    def check_value(self, value, where):
        """Apply the value-length ceiling and the XML character-data rules."""
        encoded = len(value.encode("utf-8", "surrogatepass"))

        if encoded > MAX_STRING_BYTES:
            self.add("MAX_VALUE_BYTES_EXCEEDED",
                     "the value at %s is %d bytes long; the ceiling is %d"
                     % (where, encoded, MAX_STRING_BYTES))

        reason = invalid_text_reason(value)

        if reason is not None:
            self.add("INVALID_TEXT_VALUE", "the value at %s is unusable: %s" % (where, reason))

    def check_encoded_string_spans(self, text):
        """Bound every string literal as it is ENCODED, not as it decodes.

        The module's lexical gate measures a string against
        XML_CURL_JSON_MAX_STRING_BYTES on the raw payload
        (mod_xml_curl.c L647-L649), before any escape has been decoded, so a
        literal built out of \\uXXXX escapes can breach the ceiling while its
        decoded form is comfortably inside it. The decoded tree cannot show
        that, which is the one and only reason this pass over the raw text
        exists. Names are additionally bounded at MAX_NAME_BYTES, so in
        practice any literal that reaches this ceiling is a value.
        """
        for match in STRING_LITERAL_RE.finditer(text):
            # Measure the content, excluding the two delimiting quotes, the way
            # the gate does.
            span = len(match.group(0)[1:-1].encode("utf-8", "surrogatepass"))

            if span > MAX_STRING_BYTES:
                self.add_once("MAX_VALUE_BYTES_EXCEEDED",
                              "a JSON string literal spans %d encoded bytes; the ceiling is "
                              "%d, measured on the literal as written rather than on its "
                              "decoded form" % (span, MAX_STRING_BYTES))


def validate_unicode_escape(text, index):
    """Validate one \\uXXXX escape at text[index] == 'u'. Returns chars consumed, or 0.

    Mirrors xml_curl_json_validate_unicode_escape() (mod_xml_curl.c L520-L559):
    a high surrogate has to be followed by a \\uXXXX low surrogate, a lone low
    surrogate is refused, and the resulting code point has to be a legal XML
    character -- which rules out \\u0000, \\u0008, \\u000c, the rest of C0, and
    U+FFFE/U+FFFF. Returns the number of characters this escape occupies counting
    the leading 'u' (5 for a plain escape, 11 for a surrogate pair), so 0 is
    unambiguously a rejection.
    """
    if index + 5 > len(text):
        return 0

    digits = text[index + 1:index + 5]

    for digit in digits:
        if digit not in "0123456789abcdefABCDEF":
            return 0

    code = int(digits, 16)

    if 0xD800 <= code <= 0xDBFF:
        # A high surrogate is only legal as the first half of a pair.
        if index + 11 > len(text) or text[index + 5] != "\\" or text[index + 6] != "u":
            return 0

        low_digits = text[index + 7:index + 11]

        for digit in low_digits:
            if digit not in "0123456789abcdefABCDEF":
                return 0

        low = int(low_digits, 16)

        if low < 0xDC00 or low > 0xDFFF:
            return 0

        return 11

    if not is_xml_char(code):
        return 0

    return 5


class LexicalGate(object):
    """The pre-parse gate: one pass over the raw text, bounding it before json.loads().

    WHY THIS EXISTS AT ALL, AND WHY IT RUNS FIRST
    ---------------------------------------------
    This tool is pointed at output captured from a provisioning gateway, which is
    exactly the untrusted input mod_xml_curl itself refuses to hand to a parser
    unexamined. json.loads() materialises the complete object graph before any
    ceiling expressed over the decoded tree can look at it, so a document that
    breaches every ceiling in the table has already been allocated in full by the
    time it is measured. Python's own json documentation warns about precisely
    this. A 1 MiB payload of nested arrays, or of one enormous string, is cheap to
    send and expensive to decode.

    So the ceilings that CAN be enforced on the raw text are enforced on the raw
    text, in a single pass with a bounded integer stack, before the parser runs.
    This is not a Python-specific safety measure bolted on: it is the same gate the
    module applies, xml_curl_json_validate_text() (mod_xml_curl.c L575-L690), for
    the same reason and in the same order. The semantic walk that follows now only
    ever sees a document that is already known to be small, shallow and shaped
    like the profile.

    WHAT IT BOUNDS
    --------------
      * nesting -- every '{' and every '[' pushes a level, ceiling MAX_DEPTH
      * value count -- every '{', every '[' and every string literal, INCLUDING a
        member key, counts one; ceiling MAX_VALUES
      * string length -- measured on the literal as WRITTEN, so an \\uXXXX-heavy
        literal is charged what it costs on the wire; ceiling MAX_STRING_BYTES

    WHAT IT REFUSES OUTRIGHT
    ------------------------
    Anything outside the profile: a number, a boolean, a null, NaN, Infinity, an
    unterminated string, a mismatched bracket, a raw control byte inside a string,
    \\b and \\f (which name characters XML forbids), and any other escape.

    Every finding is reported once and the pass continues, so a gateway author sees
    the whole set rather than the first -- except after a structural error, where
    the rest of the scan would be describing a document this pass has already lost
    track of.
    """

    def __init__(self):
        self.violations = []
        self._reported = set()
        self.structural = False

    def add_once(self, name, detail):
        if name in self._reported:
            return

        self._reported.add(name)
        self.violations.append((name, detail))

    def scan(self, text):
        """Walk `text` once. Returns the list of (name, detail) findings."""
        depth = 0
        stack = []
        values = 0
        index = 0
        length = len(text)

        if not text:
            self.add_once("INVALID_JSON", "the body is empty; a response has to be a single "
                                          "well-formed JSON object")
            return self.violations

        while index < length:
            char = text[index]

            if char == "{" or char == "[":
                if depth >= MAX_DEPTH:
                    self.add_once("MAX_DEPTH_EXCEEDED",
                                  "structural nesting passes %d levels at character %d; every "
                                  "JSON object AND every JSON array consumes one level, so the "
                                  "object/array alternation a repeated-children array needs "
                                  "costs two levels per element level"
                                  % (MAX_DEPTH, index))
                    self.structural = True
                    return self.violations

                values += 1

                if values > MAX_VALUES:
                    self.add_once("MAX_VALUES_EXCEEDED",
                                  "the document passes %d values at character %d, counting every "
                                  "object, every array and every string including member names"
                                  % (MAX_VALUES, index))
                    self.structural = True
                    return self.violations

                stack.append(char)
                depth += 1
                index += 1
                continue

            if char == "}" or char == "]":
                expected = "{" if char == "}" else "["

                if not stack or stack[-1] != expected:
                    self.add_once("INVALID_JSON",
                                  "the body closes a %s at character %d that was never opened"
                                  % (char, index))
                    self.structural = True
                    return self.violations

                stack.pop()
                depth -= 1
                index += 1
                continue

            if char in ":, \t\r\n":
                index += 1
                continue

            if char == '"':
                index += 1
                start = index
                values += 1

                if values > MAX_VALUES:
                    self.add_once("MAX_VALUES_EXCEEDED",
                                  "the document passes %d values at character %d, counting every "
                                  "object, every array and every string including member names"
                                  % (MAX_VALUES, index))
                    self.structural = True
                    return self.violations

                while index < length and text[index] != '"':
                    if text[index] == "\\":
                        if index + 1 >= length:
                            self.add_once("INVALID_JSON",
                                          "the body ends inside an escape sequence at character %d"
                                          % (index,))
                            self.structural = True
                            return self.violations

                        following = text[index + 1]

                        if following in '"\\/nrt':
                            index += 2
                            continue

                        if following == "u":
                            consumed = validate_unicode_escape(text, index + 1)

                            if not consumed:
                                self.add_once("INVALID_TEXT_VALUE",
                                              "the escape at character %d does not name a "
                                              "character that may appear in XML character data; "
                                              "a lone surrogate, \\\\u0000, \\\\u0008, \\\\u000c, "
                                              "another C0 control or U+FFFE/U+FFFF is refused "
                                              "before the parser sees it" % (index,))
                                self.structural = True
                                return self.violations

                            index += 1 + consumed
                            continue

                        self.add_once("INVALID_TEXT_VALUE",
                                      "the escape \\\\%s at character %d is not usable: \\\\b and "
                                      "\\\\f name characters XML forbids, and anything else is not "
                                      "a JSON escape at all" % (following, index))
                        self.structural = True
                        return self.violations

                    if ord(text[index]) < 0x20:
                        self.add_once("INVALID_JSON",
                                      "a raw control byte U+%04X appears inside the string "
                                      "literal at character %d; JSON requires it to be escaped"
                                      % (ord(text[index]), index))
                        self.structural = True
                        return self.violations

                    index += 1

                if index >= length:
                    self.add_once("INVALID_JSON",
                                  "the string literal opened at character %d is never terminated"
                                  % (start - 1,))
                    self.structural = True
                    return self.violations

                span = len(text[start:index].encode("utf-8", "surrogatepass"))

                if span > MAX_STRING_BYTES:
                    self.add_once("MAX_VALUE_BYTES_EXCEEDED",
                                  "a JSON string literal spans %d encoded bytes; the ceiling is "
                                  "%d, measured on the literal as written rather than on its "
                                  "decoded form" % (span, MAX_STRING_BYTES))

                index += 1
                continue

            # Anything else is outside the profile. A digit, a sign, a period, or the
            # first letter of true/false/null/NaN/Infinity: all of them are scalars the
            # translation has no representation for, and all of them are refused before
            # the parser's number scanner can be reached.
            token = text[index:index + 12].split(",")[0].split("}")[0].split("]")[0].strip()

            self.add_once("NON_STRING_LEAF",
                          "the token %r at character %d is neither an object, an array nor a "
                          "string; every leaf value must be a JSON string, because the builders "
                          "the translation uses take a const char * and a JSON number has no "
                          "guaranteed lexical round trip" % (token or char, index))
            self.structural = True
            return self.violations

        if depth != 0:
            self.add_once("INVALID_JSON",
                          "the body ends with %d container(s) still open" % (depth,))
            self.structural = True
            return self.violations

        if values == 0:
            self.add_once("INVALID_JSON",
                          "the body carries no JSON value at all")
            self.structural = True

        return self.violations


class InputRef(object):
    """One thing to validate, named the way it will be OPENED rather than re-resolved.

    THE POINT OF THIS CLASS
    -----------------------
    An earlier shape of this tool decided what a path was with os.path.isdir() and
    os.path.isfile(), and then opened it again later. Those two steps are separated
    in time and all three of those predicates follow symbolic links, so an attacker
    who controls a capture directory can let the check see a plain .json file and
    the open see something else entirely -- a symlink out of the directory, a FIFO
    that blocks forever, a character device with no end. That is CWE-367, and the
    fix is not a better check: it is to stop checking and reopening.

    So a directory argument is OPENED once, as a directory, and every entry inside
    it is opened relative to THAT descriptor with dir_fd. A single entry name has no
    intermediate components, so there is nothing left to swap: the name resolves
    inside the directory this tool enumerated and nowhere else. A file argument is
    opened once, by the path the caller gave.

    `dir_fd` is None for a directly named path, and otherwise the descriptor the
    entry belongs to; `name` is what is passed to os.open(); `display` is the path a
    human recognises and is only ever used for output.
    """

    __slots__ = ("dir_fd", "name", "display")

    def __init__(self, dir_fd, name, display):
        self.dir_fd = dir_fd
        self.name = name
        self.display = display


def read_document_text(ref, max_response_bytes=MAX_RESPONSE_BYTES):
    """Acquire and read one input, returning its text.

    Raises ValueError((NAME, detail)) for a body that is unusable as a document,
    and EnvironmentError for something that could not be read at all -- the caller
    turns the first into a conformance verdict and the second into the I/O exit
    status, because a mistyped path must never look like a refused payload.

    THREE THINGS HAPPEN HERE IN THIS ORDER, AND THE ORDER IS THE SAFETY
    ------------------------------------------------------------------
    1. ONE open. O_NOFOLLOW so a symbolic link at the final component is refused
       rather than followed; O_CLOEXEC so the descriptor cannot leak; O_NONBLOCK so
       that a FIFO left in a capture directory returns immediately instead of
       blocking this process until somebody writes to it.
    2. fstat on THAT descriptor -- not a stat on the path, which would be a second
       resolution -- and then two refusals before a single byte is allocated: it has
       to be a regular file, and it has to be no larger than the response ceiling
       mod_xml_curl itself enforces. A character device or a directory would
       otherwise be read until memory ran out.
    3. A BOUNDED read from that same descriptor, of at most the ceiling plus one
       byte. The extra byte is not slack: st_size is a snapshot, and a file that
       grows between the fstat and the read would otherwise slip past the ceiling.
       Receiving that byte is itself the refusal.

    The two things the module establishes about the bytes are then established here
    too, mirroring xml_curl_json_read_file() (mod_xml_curl.c L714): no NUL anywhere,
    and well-formed UTF-8. One leading byte order mark is tolerated, exactly as the
    module's lexical gate tolerates it (mod_xml_curl.c L563-L566).
    """
    flags = os.O_RDONLY | os.O_CLOEXEC

    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW

    if hasattr(os, "O_NONBLOCK"):
        flags |= os.O_NONBLOCK

    try:
        if ref.dir_fd is None:
            handle = os.open(ref.name, flags)
        else:
            handle = os.open(ref.name, flags, dir_fd=ref.dir_fd)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise EnvironmentError(errno.ELOOP,
                                   "refusing to read a symbolic link; pass the file it resolves to "
                                   "if that is really what should be validated")
        raise EnvironmentError(error.errno, os.strerror(error.errno) if error.errno else str(error))

    try:
        info = os.fstat(handle)

        if not stat.S_ISREG(info.st_mode):
            raise EnvironmentError(errno.EINVAL,
                                   "not a regular file; a directory, a FIFO, a socket or a device "
                                   "has no bounded body to validate")

        if info.st_size > max_response_bytes:
            raise ValueError(("MAX_RESPONSE_BYTES_EXCEEDED",
                              "the body is %d bytes; mod_xml_curl stops streaming a response at "
                              "%d bytes (XML_CURL_MAX_BYTES, or response-max-bytes for the "
                              "binding), so a payload this large is one the module would never "
                              "decode" % (info.st_size, max_response_bytes)))

        raw = read_bounded(handle, max_response_bytes + 1)
    finally:
        os.close(handle)

    if len(raw) > max_response_bytes:
        raise ValueError(("MAX_RESPONSE_BYTES_EXCEEDED",
                          "the body exceeds %d bytes; mod_xml_curl stops streaming a response at "
                          "that many (XML_CURL_MAX_BYTES, or response-max-bytes for the binding), "
                          "so a payload this large is one the module would never decode"
                          % (max_response_bytes,)))

    if b"\x00" in raw:
        raise ValueError(("EMBEDDED_NUL",
                          "the body carries a NUL byte at offset %d; every name and value "
                          "reaches a builder that measures with strlen(), so a NUL would "
                          "silently truncate it" % (raw.index(b"\x00"),)))

    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]

    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(("INVALID_ENCODING",
                          "the body is not well-formed UTF-8: %s at byte offset %d"
                          % (error.reason, error.start)))


def read_bounded(handle, limit):
    """Read at most `limit` bytes from an open descriptor.

    os.read() may return fewer bytes than asked for without meaning end of file, so
    a single call cannot establish either the content or the size. The loop stops on
    a genuine end of file or when the limit is reached -- never on a short count,
    and never without a limit. EINTR is retried because an interrupted read is not
    a data error; EAGAIN ends the read because O_NONBLOCK is set and a regular file
    that answers EAGAIN has nothing more to give this call.
    """
    chunks = []
    total = 0

    while total < limit:
        try:
            chunk = os.read(handle, min(65536, limit - total))
        except OSError as error:
            if error.errno == errno.EINTR:
                continue
            if error.errno in (errno.EAGAIN, getattr(errno, "EWOULDBLOCK", errno.EAGAIN)):
                break
            raise EnvironmentError(error.errno, os.strerror(error.errno) if error.errno else str(error))

        if not chunk:
            break

        chunks.append(chunk)
        total += len(chunk)

    return b"".join(chunks)


def validate_file(ref, sections, max_response_bytes=MAX_RESPONSE_BYTES):
    """Validate one input. Returns a list of Violation, empty when it conforms.

    Raises EnvironmentError for something that could not be read at all, which the
    caller turns into the usage/I/O exit status rather than a conformance verdict.

    THE ORDER OF THE THREE STAGES IS LOAD BEARING
    ---------------------------------------------
      1. Acquire and read, bounded by the module's own response ceiling.
      2. The LEXICAL GATE, over the raw text, before json.loads(). Nothing that
         allocates in proportion to the document's shape runs before this, so a
         hostile payload is refused while it is still just a bounded string.
      3. Only then the parse and the semantic walk, which are safe precisely because
         the gate has already established the document is shallow, small and made of
         nothing but objects, arrays and strings.

    Stage 2 short-circuits stages 3 when it finds a STRUCTURAL problem, because past
    one the pass no longer knows where it is in the document and every further finding
    would be describing a shape it has lost track of. A ceiling breach that is not
    structural -- an over-long string literal -- is reported and the walk still runs,
    so a gateway author gets the whole picture where a whole picture exists.
    """
    path = ref.display

    try:
        text = read_document_text(ref, max_response_bytes)
    except ValueError as error:
        name, detail = error.args[0]
        return [Violation(path, name, detail)]

    gate = LexicalGate()

    try:
        found = gate.scan(text)
    except MemoryError:
        # The final fail-closed safeguard. The ceilings above make this
        # unreachable for any input this tool will accept -- the body is already
        # bounded at MAX_RESPONSE_BYTES and this pass allocates a stack bounded at
        # MAX_DEPTH -- but running out of memory must produce a verdict rather than
        # a traceback, because a traceback on stderr next to an exit code of 1 is
        # indistinguishable from a refused payload.
        return [Violation(path, "RESOURCE_EXHAUSTED",
                          "the body exhausted memory before it could be measured; it is refused")]

    if found:
        violations = [Violation(path, name, detail) for name, detail in found]

        if gate.structural:
            return violations
    else:
        violations = []

    try:
        document = json.loads(text, object_pairs_hook=object_pairs_hook,
                              parse_constant=parse_constant)
    except DuplicateMemberError as error:
        # Reported on its own because the parse cannot continue past it, and
        # because the fix -- use an array -- is specific.
        violations.append(Violation(path, "DUPLICATE_MEMBER",
                                    "an object repeats the member name %r; cJSON preserves both, so "
                                    "the document a producer validated and the tree mod_xml_curl "
                                    "builds could disagree. Express repetition with an array instead"
                                    % (error.key,)))
        return violations
    except NonStringConstantError as error:
        violations.append(Violation(path, "NON_STRING_LEAF",
                                    "the payload carries the token %s, which is neither JSON nor a "
                                    "string; every leaf value must be a JSON string" % (error.token,)))
        return violations
    except ValueError as error:
        # Not well-formed JSON at all, or well-formed with trailing bytes.
        # mod_xml_curl requires the parse to land exactly on the terminator, so
        # a document of "{...}GARBAGE" is refused rather than truncated.
        violations.append(Violation(path, "INVALID_JSON",
                                    "the body is not a single well-formed JSON value: %s"
                                    % (error,)))
        return violations
    except RecursionError:
        # Unreachable now that the gate bounds nesting at MAX_DEPTH before the
        # parser runs, and kept because "unreachable" is a claim about today's
        # ceilings: a raised MAX_DEPTH must not turn into a traceback.
        violations.append(Violation(path, "MAX_DEPTH_EXCEEDED",
                                    "the document is nested far past the ceiling of %d levels -- deep "
                                    "enough that it cannot be walked at all" % (MAX_DEPTH,)))
        return violations
    except MemoryError:
        violations.append(Violation(path, "RESOURCE_EXHAUSTED",
                                    "the body exhausted memory while being parsed; it is refused"))
        return violations

    validator = DocumentValidator(path, sections)

    try:
        validator.validate(document)
    except RecursionError:
        validator.add("MAX_DEPTH_EXCEEDED",
                      "the document is nested far past the ceiling of %d levels -- deep "
                      "enough that it cannot be walked in full" % (MAX_DEPTH,))
    except MemoryError:
        validator.add("RESOURCE_EXHAUSTED",
                      "the body exhausted memory while being walked; it is refused")

    validator.check_encoded_string_spans(text)

    return violations + validator.violations


def open_directory(argument):
    """Open `argument` as a directory, or return None when it is not one.

    O_DIRECTORY makes the kernel decide, in the same syscall that opens it, whether
    this is a directory -- which replaces an os.path.isdir() that would have to be
    followed by a separate open. ENOTDIR means "not a directory" and is the caller's
    signal to treat it as a file; everything else is a genuine error and is raised.

    O_NOFOLLOW means a symbolic link is refused here as well, which is deliberate and
    symmetric with files: this tool declines to decide on behalf of its caller that a
    link should be followed into somewhere else. ELOOP is reported as exactly that.
    """
    flags = os.O_RDONLY | os.O_CLOEXEC

    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY

    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW

    try:
        return os.open(argument, flags)
    except OSError as error:
        if error.errno == errno.ENOTDIR:
            return None

        if error.errno == errno.ELOOP:
            raise EnvironmentError("refusing to follow the symbolic link %s; pass the directory or "
                                   "file it resolves to if that is what should be validated"
                                   % (render_path(argument),))

        raise EnvironmentError("cannot open %s: %s"
                               % (render_path(argument),
                                  os.strerror(error.errno) if error.errno else error))


def collect_inputs(arguments, open_dirs):
    """Expand the command line into a sorted, de-duplicated list of InputRef.

    A directory contributes its *.json entries, non-recursively and sorted, so that
    two runs over the same tree report in the same order. A file is taken as given
    whatever its extension, because a captured gateway response is often named for
    the request that produced it.

    NOTHING IS RESOLVED TWICE
    -------------------------
    A directory is opened ONCE and its descriptor is kept in `open_dirs` for the
    caller to close; every entry it contributes is recorded against that descriptor
    and later opened relative to it. There is therefore no window in which a
    directory could be replaced between being listed and being read, and no entry
    name is ever resolved through a path a second time. scandir() is given the
    descriptor rather than the path for the same reason.

    An entry that is itself a symbolic link is not filtered out here, and that is not
    an oversight: filtering would mean asking about it now and opening it later, which
    is the split this shape exists to remove. It is refused at open time instead, by
    O_NOFOLLOW, where the refusal is atomic with the decision.
    """
    refs = []
    seen = set()

    for argument in arguments:
        dir_fd = open_directory(argument)

        if dir_fd is not None:
            open_dirs.append(dir_fd)

            try:
                entries = sorted(entry.name for entry in os.scandir(dir_fd))
            except OSError as error:
                raise EnvironmentError("cannot list directory %s: %s"
                                       % (render_path(argument),
                                          os.strerror(error.errno) if error.errno else error))

            found = False

            for name in entries:
                if not name.endswith(".json"):
                    continue

                display = os.path.join(argument, name)

                found = True

                if display in seen:
                    continue

                seen.add(display)
                refs.append(InputRef(dir_fd, name, display))

            if not found:
                raise EnvironmentError("no *.json file found in directory %s"
                                       % (render_path(argument),))

            continue

        # Not a directory. It is opened, fstat'ed and refused or read in one place,
        # read_document_text(), so nothing is decided about it here -- not even
        # whether it exists. A path that names nothing surfaces there as an I/O
        # error, which is the exit status a mistyped path has to produce.
        if argument in seen:
            continue

        seen.add(argument)
        refs.append(InputRef(None, argument, argument))

    return refs


def parse_sections(value):
    """Turn a --sections value into an ordered tuple of section names."""
    sections = []

    for token in value.replace(",", " ").split():
        if token not in sections:
            sections.append(token)

    if not sections:
        raise argparse.ArgumentTypeError("at least one section name is required")

    for section in sections:
        if not is_valid_xml_name(section) or len(section.encode("utf-8")) > MAX_NAME_BYTES:
            raise argparse.ArgumentTypeError(
                "%r is not a usable section name: a section name has to be a legal XML "
                "name, because mod_xml_curl writes it into the <section> element it "
                "synthesises" % (section,))

    return tuple(sections)


def parse_response_ceiling(value):
    """Turn a --max-response-bytes value into a positive integer.

    A ceiling of zero or less is not a smaller ceiling, it is a validator that refuses
    everything, so it is a usage error rather than a very strict run. The module's own
    parameter behaves the same way: do_config() rejects a negative response-max-bytes
    with an error rather than adopting it.
    """
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("%r is not an integer number of bytes" % (value,))

    if parsed <= 0:
        raise argparse.ArgumentTypeError("the response ceiling must be a positive number of "
                                         "bytes, not %r" % (value,))

    return parsed


def build_parser():
    """Build the argument parser. Its help text is the tool's documentation."""
    parser = argparse.ArgumentParser(
        prog="validate_badgerfish.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        # RawDescriptionHelpFormatter leaves the description and the epilog
        # exactly as written, so the description is hard-wrapped here rather
        # than left as one long line.
        description="""\
Validate a provisioning gateway's JSON output against the BadgerFish contract
mod_xml_curl accepts, so a gateway can be proven conformant before it is
deployed. Requires python3 and its standard library only: no FreeSWITCH build,
no running FreeSWITCH and no network access.""",
        epilog="""\
the contract, in one screen:
  * exactly ONE top-level key, naming the requested provisioning section
  * that key's value is the <section> element's content, as a JSON object
  * "@name" members are attributes; their order is the attribute order
  * "$" is element text; an element has children or text, NEVER both
  * any other member is a child element: an object, or a non-empty array of
    objects for repeated children of one name
  * EVERY leaf value is a JSON string -- no numbers, booleans or nulls
  * the top-level key carries NO "@" member: mod_xml_curl owns the
    <document>/<section> envelope and synthesises it itself

exit status:
  0  every input conforms
  1  at least one input violates the contract
  2  usage or I/O error, so a mistyped path is never read as a verdict.  A symbolic
     link, a FIFO, a socket, a device and an unlistable directory are refused here
     too: this tool reads untrusted output and declines to follow or block on
     anything that is not a plain file it opened itself

limits, all mirroring mod_xml_curl:
  a body over 1 MiB is refused before it is read; nesting past 32 levels, more than
  50000 values and a string literal over 8192 encoded bytes are refused before the
  JSON is parsed at all

output:
  one line per violation on stderr, as "<path>: <VIOLATION_NAME>: <explanation>";
  every violation in a file is reported, not just the first, except that a
  structurally broken document is reported and then abandoned

examples:
  validate_badgerfish.py response.json
  validate_badgerfish.py --verbose captures/
  validate_badgerfish.py --sections configuration,directory response.json

FreeSWITCH ships a conformance corpus beside this tool, in ../fixtures/: the
configuration_*, directory_* and dialplan_* JSON files are conformant reference
documents, and the badgerfish_invalid_* files are deliberate counter-examples,
one per rule.
""")

    parser.add_argument("inputs", metavar="PATH", nargs="+",
                        help="a JSON file, or a directory whose *.json entries are validated")
    parser.add_argument("--sections", metavar="LIST", type=parse_sections,
                        default=DEFAULT_SECTIONS,
                        help="comma-separated provisioning section names the single "
                             "top-level key may carry (default: %s)"
                             % (",".join(DEFAULT_SECTIONS),))
    parser.add_argument("--max-response-bytes", metavar="N", type=parse_response_ceiling,
                        default=MAX_RESPONSE_BYTES,
                        help="the response-body ceiling to judge against, in bytes "
                             "(default: %d, mod_xml_curl's own XML_CURL_MAX_BYTES). Mirrors the "
                             "per-binding response-max-bytes parameter, so a gateway whose "
                             "binding raises the cap can be judged against the cap it will "
                             "actually meet" % (MAX_RESPONSE_BYTES,))
    parser.add_argument("--verbose", action="store_true",
                        help="also print one OK line per conformant file on stdout")
    parser.add_argument("--quiet", action="store_true",
                        help="suppress the summary on stdout; violations and the exit "
                             "status still report in full")

    return parser


def main(argv):
    """Run the validator. Returns the process exit status.

    Every path this function prints goes through render_path(), including the ones
    inside a Violation, because a filename is untrusted text and these lines are what
    a CI report is assembled from.

    The directory descriptors collect_inputs() opened are closed here rather than
    there: they have to outlive the collection, since every entry is opened relative
    to the descriptor its directory was enumerated through, and that is what leaves no
    window for a directory to be swapped mid-run.
    """
    parser = build_parser()
    options = parser.parse_args(argv)
    open_dirs = []

    try:
        try:
            files = collect_inputs(options.inputs, open_dirs)
        except EnvironmentError as error:
            sys.stderr.write("validate_badgerfish.py: %s\n" % (error,))
            return EXIT_USAGE

        violations = 0
        conformant = 0

        for ref in files:
            try:
                found = validate_file(ref, options.sections, options.max_response_bytes)
            except EnvironmentError as error:
                sys.stderr.write("validate_badgerfish.py: cannot read %s: %s\n"
                                 % (render_path(ref.display),
                                    error.strerror if error.strerror else error))
                return EXIT_USAGE

            if found:
                violations += len(found)

                for violation in found:
                    sys.stderr.write("%s\n" % (violation.format_line(),))
            else:
                conformant += 1

                if options.verbose:
                    sys.stdout.write("%s: OK\n" % (render_path(ref.display),))

        if not options.quiet:
            sys.stdout.write("validate_badgerfish.py: %d of %d file(s) conform to the "
                             "BadgerFish contract, %d violation(s) reported\n"
                             % (conformant, len(files), violations))

        sys.stderr.flush()
        sys.stdout.flush()

        return EXIT_VIOLATION if violations else EXIT_CONFORMANT
    finally:
        for handle in open_dirs:
            try:
                os.close(handle)
            except OSError:
                pass


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BrokenPipeError:
        # A downstream consumer closed the pipe: report the I/O status rather
        # than letting the interpreter print a traceback that looks like a
        # conformance failure.
        os._exit(EXIT_USAGE)
    except OSError as exception:
        if exception.errno == errno.EPIPE:
            os._exit(EXIT_USAGE)
        sys.stderr.write("validate_badgerfish.py: %s\n" % (exception,))
        sys.exit(EXIT_USAGE)
