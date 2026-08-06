# mod_xml_curl `response-format` — operator runbook

This runbook is for the person who runs FreeSWITCH and has to notice when provisioning
degrades. It documents the `response-format` parameter of `mod_xml_curl`: what the JSON
decoder guarantees, the two signatures a degradation emits and how to alert on them, the
resource ceilings a hostile or runaway gateway runs into, the boundary that keeps `file:`
gateway URLs XML-only, and the security-relevant parameters — the three TLS options and the
cookie jar — an operator should set by hand. It closes with the `mod_h323`/`mod_opal`
co-load prohibition and with a pointer for whoever writes the gateway at the other end.

Every claim below carries a `path:line` reference so it can be checked against the code
rather than taken on trust. Line numbers are those of the tree this file ships in; where a
later edit moves them, the named function, constant or format string in the same reference
still locates the code exactly.

**Contents**

1. [The fallback contract](#1-the-fallback-contract)
2. [Both fallback reasons and their stable signatures](#2-both-fallback-reasons-and-their-stable-signatures)
3. [The eleven resource ceilings](#3-the-eleven-resource-ceilings)
4. [The `file:` XML-only boundary](#4-the-file-xml-only-boundary)
5. [TLS hardening: `enable-cacert-check`, `enable-ssl-verifyhost`, `ssl-cacert-file`](#5-tls-hardening-enable-cacert-check-enable-ssl-verifyhost-ssl-cacert-file)
6. [Cookie-jar placement](#6-cookie-jar-placement)
7. [The `mod_h323` / `mod_opal` co-load prohibition](#7-the-mod_h323--mod_opal-co-load-prohibition)
8. [`response-format` documentation across the shipped profiles](#8-response-format-documentation-across-the-shipped-profiles)
9. [For gateway authors: proving BadgerFish conformance before deployment](#9-for-gateway-authors-proving-badgerfish-conformance-before-deployment)

---

## 1. The fallback contract

`response-format` is a per-binding parameter of `xml_curl.conf.xml`. It has two configured
states, and one guarantee that holds across both.

**`response-format` absent — XML decoding, bit-identical legacy behaviour, zero events.**
The binding's `response_format` member stays `NULL`, because the whole `xml_binding`
structure is zeroed after allocation
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1956`) and the parameter arm that would set
it never runs (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1937`). The format decision
therefore evaluates to 0 (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1547`), which means
no `Accept` header is appended (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1612`), no
JSON decode is attempted (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1749`), and the
pre-existing `switch_xml_parse_file()` call runs exactly as it always did
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1757`). **No `xml_curl::json_fallback` event
can fire on this path**, because the only call site of the event helper is inside the JSON
decode function (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1384`), which is only reached
from the guarded dispatch at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1749-1751`. A binding that does not opt in produces the same
request bytes, the same temporary file, the same HTTP-200 gate, the same log lines and the
same tree it produced before JSON support existed.

**`response-format="json"` — `Accept: application/json` is sent and a BadgerFish response
is translated.** The value is compared case-insensitively
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1547`), so `json`, `JSON` and `Json` all opt
in. The request then carries one additional header, `Accept: application/json`, appended
between the existing `Content-Type` header and the point where the header list is handed to
libcurl (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1612-1618`). Nothing else about the
request changes: the form body, the user agent `freeswitch-xml/1.0`, the redirect limit and
every TLS option are what they were. If the header cannot be appended at all — an
allocation failure inside libcurl — the fetch still happens and the format decision is
lowered back to XML on the spot, with a `WARNING`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1613-1617`), so the decoder can never expect
a representation this fetch did not ask for.

**Any JSON failure edge degrades to the untouched XML parse, so a lookup never fails
because of a decode problem.** The JSON decoder returns `NULL` on every failure edge, and
the caller's next statement parses the same already-downloaded body as XML
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1753-1757`). The HTTP-200 success gate
(`:1742`) and the non-200 error branch (`:1781-1785`) are unchanged, and the `NULL`-return
semantics the core sees are unchanged. A gateway that answers XML while a binding asks for
JSON is therefore still served — degraded in fidelity, never in availability.

**The single dispatch point and the single signal site.** There is exactly one place where
the format is chosen — `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1750` — and exactly one
place where a degradation is signalled: the `if (!xml)` block at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1374-1385`, which emits the `WARNING` and
fires the event side by side. Alerting therefore never has to correlate across sites: one
degradation produces one `WARNING` and one event, always together.

---

## 2. Both fallback reasons and their stable signatures

A total fallback is a silent degradation: the lookup resolves, nothing fails, and without a
signal an operator only learns of it by reading logs. The module therefore emits two
signatures for the same event, from the same place, and they pair 1:1.

### 2.1 The log signature

One `SWITCH_LOG_WARNING`, from `xml_curl_json_decode_response()`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1374-1379`). The format string is stable and
is not changed by this work:

```
JSON decode of the [%s] response from [%s] failed (%s) [Content-Type: %s]; falling back to XML parsing
```

The four operands are, in order: the requested provisioning section; the gateway URL,
redacted to its authority so a configured credential or query token is never written to the
log (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1377`); the human-readable reason; and the
response `Content-Type` as the gateway sent it. Section, URL and `Content-Type` are all
bounded and sanitized before they are rendered, so a hostile `Content-Type` cannot forge a
second log line.

The reason operand is one of **five** human-readable phrases
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1354-1368`), reproduced here exactly:

| # | Reason phrase in the `WARNING` | Line |
|---|---|---|
| 1 | `no response body was captured` | `mod_xml_curl.c:1355` |
| 2 | `the requested provisioning section is unknown` | `mod_xml_curl.c:1358` |
| 3 | `response Content-Type is not application/json` | `mod_xml_curl.c:1361` |
| 4 | `response body could not be read in full` | `mod_xml_curl.c:1364` |
| 5 | `response body is not a well-formed BadgerFish JSON document for the requested section` | `mod_xml_curl.c:1367` |

A sixth string, `unknown translation error`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1348`), is the initialiser of the same
variable. It describes no reachable edge: the five-armed `if`/`else if` ladder above it
assigns one of the five phrases on every path that can produce a `NULL` document, so it is a
defensive default rather than a sixth reason to alert on.

### 2.2 The event signature

One `SWITCH_EVENT_CUSTOM` with subclass **`xml_curl::json_fallback`**, fired by
`xml_curl_json_fire_fallback_event()`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1279-1296`). The subclass name is the constant
at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:229`; it is reserved when the module loads
(`:2095`) and released when it shuts down (`:2119`), so it is discoverable by name in
`fs_cli`. Reservation is best-effort: emission does not depend on it, so at worst
discoverability is degraded and the event still fires.

The event carries exactly three headers:

| Header | Value | Line |
|---|---|---|
| `Binding` | the `name` attribute of the `<binding>` whose fetch degraded, sanitized; the literal `(unnamed)` when the configuration named none | `mod_xml_curl.c:1289-1291`, constant at `:232` |
| `Fallback-Reason` | exactly one of `content-type-mismatch` or `malformed-json` | `mod_xml_curl.c:1292`, constants at `:230-231` |
| `Gateway` | the binding's gateway URL, **redacted** through the same helper the `WARNING` uses, so userinfo, path and query never reach a subscriber | `mod_xml_curl.c:1293` |

Emission is fire-and-forget and fully guarded: the helper returns `void`, the caller ignores
it, and every failure edge inside it — an event that could not be created, a dispatcher that
refused it — leaves the module's behaviour and the document it returns exactly as they were
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1285-1295`). Observability cannot change
provisioning.

### 2.3 Which failure edge maps to which reason

The `WARNING` carries five prose reasons; the event carries a two-valued machine taxonomy
over them. The mapping is fixed in the code
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1354-1368`) and is total:

| `Fallback-Reason` | Failure edges that produce it | Line |
|---|---|---|
| `content-type-mismatch` | **Only** the `Content-Type` classifier rejection: the response's declared media type is not `application/json`. This is the "the gateway answered in a representation this binding did not ask for" class. | `mod_xml_curl.c:1360-1362` |
| `malformed-json` | **Every** other edge: no response body was captured (`:1354-1356`); the requested provisioning section is unknown (`:1357-1359`); the response body could not be read in full (`:1363-1365`); the body is not a well-formed BadgerFish document for the requested section (`:1366-1368`). This is the "a body this module could not turn into a document" class. | `mod_xml_curl.c:1354-1368` |

The classifier itself accepts `application/json` bare or carrying parameters such as
`; charset=utf-8`, and rejects an absent or empty header, `text/xml`, `application/xml` and
every other media type, comparing type and subtype as one case-insensitive token
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:695-724`). A gateway that sends the right
bytes with the wrong header therefore reports `content-type-mismatch`, not
`malformed-json` — which is exactly the distinction worth alerting on separately, because
its remedy is a gateway header fix rather than a payload fix.

Note the practical consequence for a mismatch: the ceilings in section 3 are never reached
on that edge, because the classifier rejects before the body is read at all
(`mod_xml_curl.c:1360` precedes `mod_xml_curl.c:1363`).

### 2.4 Watching the event, and alerting without log scraping

Watch it live from a console. Start `fs_cli` and subscribe with its event command, which
takes the subclass verbatim as its third word:

```sh
fs_cli
# then, at the prompt:
/event plain CUSTOM xml_curl::json_fallback
```

Event subscription is a **connection** command rather than an API command, so it cannot be
issued with `fs_cli -x` — `fs_cli -x` wraps its argument in `api …`
(`libs/esl/fs_cli.c:1719`), and no API named `events` exists. Inside `fs_cli` the leading
slash sends the rest of the line straight to the event socket
(`libs/esl/fs_cli.c:926-934`), which is what makes `/event` and `/filter` work there.
Subscribing to `CUSTOM` with a subclass argument registers exactly that subclass and nothing
else (`src/mod/event_handlers/mod_event_socket/mod_event_socket.c:2484-2486`).

A fallback then prints an event whose shape is fixed by section 2.2 — the three headers
above, plus the standard `Event-Name: CUSTOM` and
`Event-Subclass: xml_curl::json_fallback` the core adds
(`src/switch_event.c:806`). In `plain` framing every header **value** is percent-encoded, so
the subclass and the gateway URL arrive escaped:

```
Event-Name: CUSTOM
Event-Subclass: xml_curl%3A%3Ajson_fallback
Event-Calling-Function: xml_curl_json_fire_fallback_event
Binding: example
Fallback-Reason: content-type-mismatch
Gateway: https%3A//provisioning.example.net
```

`Fallback-Reason` is unaffected by that encoding — neither `malformed-json` nor
`content-type-mismatch` contains a character that gets escaped — which is one more reason to
key an alert on it rather than on the URL or the subclass string.

Subscribe from an event-socket client. Over an inbound ESL connection the same command is
sent without the slash, and JSON framing is usually easier to consume from a monitoring
agent — it delivers header values unescaped
(`src/mod/event_handlers/mod_event_socket/mod_event_socket.c:2477-2479`):

```
event json CUSTOM xml_curl::json_fallback
```

The event then arrives as a single JSON object, with the three headers of section 2.2 among
the core's own:

```json
{
  "Event-Subclass": "xml_curl::json_fallback",
  "Event-Name": "CUSTOM",
  "Event-Calling-Function": "xml_curl_json_fire_fallback_event",
  "Binding": "example",
  "Fallback-Reason": "content-type-mismatch",
  "Gateway": "https://provisioning.example.net/[redacted]"
}
```

Note the `Gateway` value: everything after the authority is replaced with `[redacted]`,
because a gateway URL's path and query routinely carry provisioning tokens
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1293`).

If your consumer already subscribes to `CUSTOM` more broadly, add a `filter` so a busy
switch does not deliver every `CUSTOM` event to it. `filter` takes a header name and a value
(`src/mod/event_handlers/mod_event_socket/mod_event_socket.c:1950-1987`):

```
event json CUSTOM
filter Event-Subclass xml_curl::json_fallback
```

**Guidance for alerting.** Alert on the event, not on the log. The event's
`Fallback-Reason` is a token chosen to be matched exactly and is guaranteed to be one of two
values, whereas the `WARNING`'s reason is prose written for a human reader; matching prose
couples an alert rule to wording. Three rules cover the operational cases:

- **Any** `xml_curl::json_fallback` event is worth a low-severity alert, keyed on
  `Binding`, because a binding that opted into JSON is not getting JSON.
- `Fallback-Reason: content-type-mismatch`, sustained, means the gateway is answering with a
  media type the binding did not ask for. Remedy: fix the gateway's `Content-Type`, or
  remove `response-format` from that binding.
- `Fallback-Reason: malformed-json`, sustained, means the gateway is answering JSON that
  falls outside the accepted profile — including a document that breaches one of the
  ceilings in section 3. Remedy: run the gateway's output through the validator in section
  9.

Because the event and the `WARNING` are emitted from the same block
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1374-1385`), the two counts pair 1:1 over any
interval. That makes the log useful as a cross-check on the alerting path rather than as its
input: if the log shows fallback `WARNING`s that the event stream did not, the subscription
is the thing to look at.

For a one-off diagnosis rather than an alert, `fs_cli -x 'xml_curl debug_on'` leaves each
fetched response body on disk and logs its path
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1788-1790`), which is the fastest way to see
the bytes a gateway actually sent. Note that the temporary file keeps its historical
`.tmp.xml` suffix even for a JSON payload
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1597`); the suffix is cosmetic and deliberately
unchanged, since it appears in that operator-visible log line.

---

## 3. The eleven resource ceilings

The JSON decoder is a whitelist, not a best-effort parser, and it is bounded so that a
runaway or hostile response cannot exhaust memory or CPU on the fetch thread. The ceilings
are compile-time constants enforced by the module itself rather than parser configuration,
so behaviour is identical on every platform and every build
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:169-173`).

Every ceiling behaves the same way when exceeded: the document is **refused** — the decoder
returns `NULL` — which is one of the `malformed-json` edges of section 2.3, so the response
is parsed as XML instead and the lookup still resolves. There is no partial acceptance and
no truncation.

| # | Name | Value | What it bounds | On breach |
|---|---|---|---|---|
| 1 | `XML_CURL_JSON_MAX_DEPTH` (`mod_xml_curl.c:189`) | 32 | Nesting levels in the JSON document | Refused at `:600` (lexical gate) and `:901` (translation) -> fallback |
| 2 | `XML_CURL_JSON_MAX_VALUES` (`mod_xml_curl.c:190`) | 50000 | Objects + arrays + strings in the whole document | Refused at `:603`, `:625` -> fallback |
| 3 | `XML_CURL_JSON_MAX_OBJECT_MEMBERS` (`mod_xml_curl.c:191`) | 256 | Members of one JSON object | Refused at `:845` -> fallback |
| 4 | `XML_CURL_JSON_MAX_ARRAY_ELEMENTS` (`mod_xml_curl.c:192`) | 256 | Elements of one array, i.e. repeated children under one name | Refused at `:952`, before anything is built -> fallback |
| 5 | `XML_CURL_JSON_MAX_CHILDREN_PER_PARENT` (`mod_xml_curl.c:193`) | 256 | Child elements built under one parent, accumulated across all member names | Refused at `:969`, `:990` -> fallback |
| 6 | `XML_CURL_JSON_MAX_NODES` (`mod_xml_curl.c:194`) | 20000 | Elements + attributes built in total | Refused at `:922`, `:961`, `:984` -> fallback |
| 7 | `XML_CURL_JSON_MAX_STRING_BYTES` (`mod_xml_curl.c:195`) | 8192 | Bytes in one name or one value | Refused at `:395`, `:677` -> fallback |
| 8 | `XML_CURL_JSON_MAX_NAME_BYTES` (`mod_xml_curl.c:196`) | 128 | Bytes in one element or attribute name | Refused at `:466` -> fallback |
| 9 | `XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES` (`mod_xml_curl.c:207`) | `MAX_NODES * (MAX_NAME_BYTES + 1)` = 2580000 | Cumulative element and attribute **name** bytes written into the tree. It is a ceiling of its own rather than the payload length because an array writes one key once per element, so the output legitimately exceeds the input | Refused at `:927`, `:966` -> fallback |
| 10 | `XML_CURL_MAX_BYTES` / `response-max-bytes` (`mod_xml_curl.c:76`, default applied at `:1839`, parameter parsed at `:1930-1936`) | 1 MiB (`1024 * 1024`), per-binding override | The HTTP response **body**, streamed to the temporary file. `response-max-bytes` caps the body for JSON **exactly** as it does for XML: the JSON decoder re-checks the same binding ceiling when it reads the file back (`:1363`, helper at `:743`), so a JSON payload inherits the cap for free | Body over the cap -> the read fails -> `response body could not be read in full` -> fallback |
| 11 | The budget's `max_text_bytes` (`mod_xml_curl.c:251`, bound at `:1131`) | `strlen(json_text)` — the payload length | Cumulative decoded attribute-value and element-text bytes. Bounded by the payload rather than by a constant, because every decoded value appears exactly once in the payload and escape sequences only ever shrink | Refused at `:930`, `:939` -> fallback |

Ceilings 1 through 8 and 11 are document-wide rather than per-node because a single
caller-owned budget structure is threaded through the whole recursive translation
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:246-253`), so a wide-and-shallow document
cannot amplify past a deep-and-narrow one. Ceiling 5 exists because this translator passes a
constant insertion offset so that insertion order becomes document order, which makes `n`
children under one parent cost O(n^2) comparisons synchronously on the fetch path; capping
one parent's width is what stops a merely wide response becoming a denial of service
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:175-187`).

### 3.1 A note on nesting: 32 enforced, 1000 vendored

Three different nesting numbers exist in this tree, and only one of them governs:

- **32** is the ceiling this module enforces: `XML_CURL_JSON_MAX_DEPTH`
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:189`). This is the operative number. A
  document nested deeper than 32 levels is refused and falls back.
- **1000** is `CJSON_NESTING_LIMIT` in the vendored cJSON parser
  (`src/include/switch_cJSON.h:128-129`). It is far looser than the module's ceiling and is
  therefore never the binding constraint here. The module's own lexical gate rejects at 32
  before cJSON's limit could ever apply
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:600`).
- **31** is where the parity corpus's nesting-boundary fixture sits — one level inside the
  enforced ceiling, which is the boundary worth testing
  (`src/mod/xml_int/mod_xml_curl/test/fixtures/directory_nesting_boundary.json`, asserted by
  `src/mod/xml_int/mod_xml_curl/test/test_mod_xml_curl.c:5137`).

If you are sizing a gateway's documents, 32 is the number to design against.

---

## 4. The `file:` XML-only boundary

A `file:` gateway URL is **always** XML, and `response-format` cannot change that.

The fetch routine detects the `file:` prefix, reads the file straight off disk with
`switch_xml_parse_file()` and returns
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1554-1561`). That return precedes every part
of the HTTP path: no request is made, so no `Accept` header is sent — the only append site is
at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1612`, downstream of the return — and no
content type is negotiated or read, since the `CURLINFO_CONTENT_TYPE` probe is at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1726`, also downstream. The JSON decoder is
never reached, so a `file:` binding fires no `xml_curl::json_fallback` event either.

Setting `response-format="json"` on a `file:` binding is therefore inert, not an error, and
produces no warning. If you want a local JSON document decoded, serve it over HTTP(S) from a
gateway that sets `Content-Type: application/json`.

---

## 5. TLS hardening: `enable-cacert-check`, `enable-ssl-verifyhost`, `ssl-cacert-file`

### What each parameter does

| Parameter | Effect | Lines |
|---|---|---|
| `enable-cacert-check` | When true, sets `CURLOPT_SSL_VERIFYPEER` back on, so libcurl verifies that the gateway's certificate chains to a trusted CA | parsed at `mod_xml_curl.c:1895`, applied at `:1667-1669` |
| `enable-ssl-verifyhost` | When true, sets `CURLOPT_SSL_VERIFYHOST` to 2, so libcurl verifies that the certificate actually names the host being contacted | parsed at `mod_xml_curl.c:1907`, applied at `:1695-1697` |
| `ssl-cacert-file` | Sets `CURLOPT_CAINFO` to a PEM bundle, so a private or internal CA can be trusted instead of the system trust store. Only meaningful together with `enable-cacert-check` | parsed at `mod_xml_curl.c:1905`, applied at `:1691-1693` |

Both booleans are parsed with `switch_true()` and the arm only matches when the value is
true (`mod_xml_curl.c:1895`, `:1907`), so writing `value="false"` is equivalent to omitting
the parameter rather than being an explicit opt-out.

### Why the shipped defaults are permissive, and unchanged by this work

For any `https` gateway URL the module unconditionally disables both checks before applying
any per-binding option: `CURLOPT_SSL_VERIFYPEER` is set to 0 and `CURLOPT_SSL_VERIFYHOST` to
0 at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1620-1623`. The three parameters above are
opt-in overrides applied later in the same function. So out of the box, an `https`
provisioning fetch is encrypted but **not authenticated** — a machine-in-the-middle can
present any certificate and serve the switch its provisioning documents.

That posture is long-standing and is deliberately left byte-for-byte as it was. Tightening
it would change the behaviour of every existing deployment whose gateway uses a certificate
that would not verify, turning a working switch into one that cannot provision — a
behaviour change well outside a documentation and observability change. In all four shipped
profiles all three parameters remain **commented samples**, so no default moves:
`conf/vanilla/autoload_configs/xml_curl.conf.xml:18` (`enable-cacert-check`), `:20`
(`enable-ssl-verifyhost`) and `:36` (`ssl-cacert-file`), and correspondingly in
`conf/testing/autoload_configs/xml_curl.conf.xml:18`, `:20` and `:36`.

### Recommended hardened configuration for production

**This is operator action, not a changed default.** Nothing below happens unless you write
it into your own configuration.

For a gateway with a publicly trusted certificate, uncomment both checks:

```xml
<param name="enable-cacert-check" value="true"/>
<param name="enable-ssl-verifyhost" value="true"/>
```

For a gateway behind a private or internal CA, add the bundle as well:

```xml
<param name="enable-cacert-check" value="true"/>
<param name="enable-ssl-verifyhost" value="true"/>
<param name="ssl-cacert-file" value="$${certs_dir}/cacert.pem"/>
```

Two operational notes. Enable both together: peer verification without host verification
accepts any certificate a trusted CA ever issued, for any name. And roll it out on a
non-production binding first — if the gateway's certificate does not verify, the fetch fails
and the lookup returns nothing, which is a provisioning outage rather than a fallback. The
`response-format` fallback in section 1 covers decode problems only; it does not cover a
transport that never completed.

---

## 6. Cookie-jar placement

`cookie-file` enables cookie persistence for a binding by handing libcurl a jar path
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1705-1708`).

### Why the jar must sit in a directory other users cannot write

libcurl opens the jar for writing at the end of **every** transfer. That open follows
symbolic links and creates a missing file with whatever the process umask happens to be
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1391-1399`). So an unvalidated path in a
directory any local user can write lets that user choose which file FreeSWITCH truncates,
and leaves a freshly created jar world-readable even though it holds session cookies for the
provisioning gateway. A predictable name in a shared temporary directory is exactly the
precondition such an attack needs.

### The recommendation

Put the jar in a service directory only the FreeSWITCH user can write — `$${db_dir}` is the
natural choice, and it is what the shipped sample recommends
(`conf/vanilla/autoload_configs/xml_curl.conf.xml:42-51`):

```xml
<param name="cookie-file" value="$${db_dir}/xml_curl-cookies.txt"/>
```

A world-writable directory is acceptable **only** if it carries the sticky bit, because that
is what stops the name being renamed away and re-created between the module's check and
libcurl's open (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1466`). The rule is
deliberately not "owner only": a service directory such as `$${db_dir}` is conventionally
readable and traversable by others, and demanding mode 0700 there would refuse the very
location the sample recommends
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1412-1418`).

### What the module does when the path is unacceptable

It **refuses the jar, keeps performing the fetch, and logs**. The validation function
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1427`) gates only the two cookie `setopt`
calls (`:1705-1708`); everything else about the fetch is untouched, so a refusal costs
cookie persistence for that binding and nothing else. The refusal is a single
`SWITCH_LOG_WARNING` (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1495-1498`):

```
Refusing cookie file [%s] for the binding at [%s] because %s; cookies will not be persisted for this binding
```

The reasons it can carry are these, and they say nothing at all on success:

| Reason phrase | Line |
|---|---|
| `its path is too long to be validated` | `mod_xml_curl.c:1446` |
| `the directory holding it could not be read` | `mod_xml_curl.c:1462` |
| `the path holding it is not a directory` | `mod_xml_curl.c:1464` |
| `the directory holding it is writable by other users and not sticky, so the name could be replaced after it is checked` | `mod_xml_curl.c:1466` |
| `it is not a regular file, so a symbolic link, directory or special file would be written through` | `mod_xml_curl.c:1477` |
| `it belongs to another user` | `mod_xml_curl.c:1479` |
| `other users can write it, so its contents cannot be trusted` | `mod_xml_curl.c:1481` |
| `its status could not be read` | `mod_xml_curl.c:1486` |
| `it does not exist and could not be created privately` | `mod_xml_curl.c:1488` |

An existing jar that is a regular file, owned by this process and writable by nobody else,
is accepted untouched and its mode is never altered
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1475-1483`). A jar that does not exist yet is
created here with `O_EXCL` and no-follow semantics at mode 0600
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1487-1491`), so libcurl later truncates a file
that is already private instead of creating a public one. The check is validated at fetch
time rather than at configuration time, so a jar replaced by a symbolic link after the module
loaded is caught just the same
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1699-1704`). On Windows the check is a no-op
and behaviour is byte-for-byte what it was, because that platform has neither the link
semantics nor the ownership model the check is written against
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1429-1430`).

---

## 7. The `mod_h323` / `mod_opal` co-load prohibition

### The prohibition

**`mod_h323` and `mod_opal` must never be loaded into the same FreeSWITCH process.** Run
them in separate instances — one endpoint per FreeSWITCH process — if you need both
protocols on one host.

The cause is two PTLib runtimes in one address space. `mod_h323` links
`libpt.so.2.10.9`; `mod_opal` links `libpt.so.2.12-beta10`. A process can hold exactly one
PTLib `PProcess` singleton, and each module builds its own during load, so whichever module
loads **second** crashes inside `PProcess::Construct()` while constructing its `FSProcess`.
The crash is deterministic and symmetric — it happens in **both** load orders — and both
faulting frames are in frozen third-party code, so it cannot be fixed in FreeSWITCH:

- `mod_h323` then `mod_opal`: the fault is in `/opt/opalvoip/lib/libpt.so.2.12-beta10`,
  reached from `mod_opal_load`.
- `mod_opal` then `mod_h323`: the fault is in `/lib/libpt.so.2.10.9`, reached from
  `mod_h323_load`.

Both backtraces, both log tails, and a corroborating probe showing that `dlopen` static
initialisation of the two modules in one process completes cleanly are archived at
`blitzy/documentation/oos9-coload-determination.md` and
`blitzy/documentation/oos9-coload-evidence/`.

### The guard that exists in this tree

**Outcome A was selected**: a mutual-exclusion guard in module code, at the top of each
module's load function.

The discriminating question was whether the SIGSEGV happens during `dlopen` static
initialisation — before any module code runs — or during/after `switch_module_load`. It was
answered empirically, under gdb, in both load orders. Quoting the determination's own
conclusion (`blitzy/documentation/oos9-coload-determination.md`, section 6):

> **Module code executes before the crash ⇒ OUTCOME A.**

The three rows of that section's table that decide it: the second module's load-function log
line **was** observed in both orders; a frame **inside** `mod_*_load` was on the faulting
stack in both orders (`#7 mod_opal_load … mod_opal.cpp:116` and
`#6 mod_h323_load … mod_h323.cpp:167`); and the fault was **not** in `dl_init`, a library
constructor or a static initialiser — a `dlopen(RTLD_NOW|RTLD_LOCAL)`-only probe of both
modules in one process completed with exit 0. Because module code runs first, module code can
refuse.

So each load function's **first** statement now asks whether the sibling is already loaded,
and refuses if it is:

- `mod_h323_load` refuses when `switch_loadable_module_exists("mod_opal")` succeeds
  (`src/mod/endpoints/mod_h323/mod_h323.cpp:174-179`).
- `mod_opal_load` refuses when `switch_loadable_module_exists("mod_h323")` succeeds
  (`src/mod/endpoints/mod_opal/mod_opal.cpp:121-127`).

The guard has to be the first statement because the conflicting singleton is created by the
`new FSProcess()` that follows it. Everything else in both modules is unchanged.

### What an operator observes when the guard fires

The second load is **refused**, and the process **survives**. Concretely:

- An `ERROR` naming the singleton conflict and both PTLib runtimes is written to the log,
  for example (`src/mod/endpoints/mod_h323/mod_h323.cpp:175-178`):

  ```
  Refusing to load mod_h323: mod_opal is already loaded in this process, and their conflicting
  PProcess singletons - one per PTLib runtime, libpt.so.2.10.9 for mod_h323 against
  libpt.so.2.12-beta10 for mod_opal - crash the process (OOS-9). Run the two endpoints in
  separate FreeSWITCH instances.
  ```

- The load function returns `SWITCH_STATUS_FALSE`
  (`src/mod/endpoints/mod_h323/mod_h323.cpp:179`,
  `src/mod/endpoints/mod_opal/mod_opal.cpp:127`), which the core reports as
  `Error Loading module … Module load routine returned an error`, so `fs_cli -x 'load
  mod_h323'` answers `-ERR [module load file routine returned an error]`.
- The refused module's own `Starting loading …` line never appears, because the guard returns
  ahead of it.
- The already-loaded sibling keeps serving calls, and `fs_cli -x status` still reports
  `UP … is ready`. Measured in both load orders; captured in
  `blitzy/documentation/oos9-coload-evidence/guard-refusal-runtime-proof.txt`.

The refusal is therefore a safe degradation, not a mitigation of the prohibition: the module
you asked for is unavailable in that process. The remedy is still one endpoint per instance.

Each endpoint's test suite also carries a case asserting the refusal —
`coload_guard_refuses_when_sibling_is_loaded`
(`src/mod/endpoints/mod_h323/test/test_mod_h323.cpp:3370`,
`src/mod/endpoints/mod_opal/test/test_mod_opal.cpp:3443`) — which exercises the guard against
a sibling registered through the module-load API, so no test binary ever links two PTLib
runtimes.

One thing this guard is **not**: a build-time or test-time restriction. Both modules are
built and both suites are run in the same tree; the prohibition is about one running
FreeSWITCH process, and it is enforced there.

---

## 8. `response-format` documentation across the shipped profiles

The shipped profiles under `conf/` were enumerated rather than assumed:

```sh
find conf -name xml_curl.conf.xml | sort
```

**Four** profiles carry an `xml_curl.conf.xml` variant, and **all four document
`response-format` identically** — the same prose, the same commented
`<param name="response-format" value="json"/>` sample, and the same pointer back to this
runbook:

| # | Profile carrier | State |
|---|---|---|
| 1 | `conf/vanilla/autoload_configs/xml_curl.conf.xml` | documents `response-format` (block at `:56`) |
| 2 | `conf/curl/autoload_configs/xml_curl.conf.xml` | documents `response-format`, propagated by this work |
| 3 | `conf/insideout/autoload_configs/xml_curl.conf.xml` | documents `response-format`, propagated by this work |
| 4 | `conf/testing/autoload_configs/xml_curl.conf.xml` | documents `response-format`, propagated by this work |

So the enumeration did **not** find vanilla to be the sole carrier: three sibling profiles
also ship the file, and the documentation block was propagated into each of them rather than
the item being closed as a no-op. Every propagated line is a comment or a commented sample
line: no profile gained an active parameter, no default changed, and the TLS posture of
section 5 is byte-for-byte what it was in every profile.

One further copy of the file exists in the tree, at
`src/mod/xml_int/mod_xml_curl/conf/autoload_configs/xml_curl.conf.xml`. It is the module's own
sample rather than a shipped `conf/` profile, so it is outside this enumeration and was
deliberately left alone.

---

## 9. For gateway authors: proving BadgerFish conformance before deployment

`mod_xml_curl` owns this JSON contract; a gateway must conform to it, not the reverse.

### The contract in one screen

The accepted shape is a restricted BadgerFish profile, and it is a whitelist — anything
outside it is refused and falls back
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:155-167`):

- **Exactly one top-level key**, and it must name the provisioning section that was
  requested — `configuration`, `directory` or `dialplan`
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1030`).
- That key's value is the `<section>` element's content, as a JSON object. The module
  synthesises the `<document type="freeswitch/xml"><section name="…">` envelope itself
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1109-1114`), so the gateway must **not** emit
  it.
- A member named `@something` is an **attribute**; the order of the `@` members is the
  attribute order in the emitted XML.
- A member named `$` is the element's **text**.
- Any other member is a **child element**: a nested object, or a non-empty array of objects
  for repeated children of one name.
- **Every leaf value is a JSON string.** Numbers, booleans and nulls are refused.
- An element carries **either** `$` **or** children — never both.
- The top-level key carries **no** `@` member of any name, because the module owns the
  envelope.
- The ceilings in section 3 apply to every document.

### The conformance corpus

Eighteen XML/JSON fixture pairs ship beside the suite, in
`src/mod/xml_int/mod_xml_curl/test/fixtures/`. Each `.json` file is a conformant reference
document and each `.xml` file is its twin; the suite asserts that both serialise
byte-identically through `switch_xml_toxml()`, in both directions. They are worth reading as
worked examples: `configuration_*`, `directory_*` and `dialplan_*` cover repeated children at
the width ceiling, attribute-only elements, mixed attribute-and-text nodes with sibling
elements, nesting one level inside the depth ceiling, `\uXXXX` unicode escapes surviving the
round trip, and empty/self-closing element forms.

Five deliberate counter-examples ship alongside them, one per rule, named
`badgerfish_invalid_*.json`: multiple top-level keys, a top-level key that is not a bound
section name, a non-string leaf value, envelope attributes the gateway should not have
emitted, and an `@`-attribute under the root key itself.

### Running the validator against your gateway's output

`src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py` is a standalone checker:
python3 and its standard library only, no FreeSWITCH build, no running FreeSWITCH and no
network access. Capture your gateway's response body to a file and run it:

```sh
python3 src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py response.json
```

It also takes a directory, validating every `*.json` entry in it — which is the shape to use
in a gateway's own CI over a directory of captured responses:

```sh
python3 src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py --verbose captures/
```

If your deployment binds fewer sections, narrow the accepted top-level keys:

```sh
python3 src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py \
    --sections configuration,directory response.json
```

Exit status is the verdict: `0` when every input conforms, `1` when at least one input
violates the contract, and `2` on a usage or I/O error — so a mistyped path is never read as
a pass. Violations print one line each on stderr, as
`<path>: <VIOLATION_NAME>: <explanation>`, and every violation in a file is reported rather
than just the first. Run `--help` for the full contract summary and the option list.

A gateway whose captured responses exit 0 here will not produce a `malformed-json` fallback
for a shape reason. Two things the validator cannot check for you: that the gateway sets
`Content-Type: application/json` — that is the `content-type-mismatch` edge of section 2.3 —
and that the response body stays inside the binding's `response-max-bytes` ceiling
(section 3, row 10).
