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
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1993`) and the parameter arm that would set
it never runs (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1974`). The format decision
therefore evaluates to 0 (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1584`), which means
no `Accept` header is appended (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1649`), no
JSON decode is attempted (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1786`), and the
pre-existing `switch_xml_parse_file()` call runs exactly as it always did
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1794`). **No `xml_curl::json_fallback` event
can fire on this path**, because the only call site of the event helper is inside the JSON
decode function (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1421`), which is only reached
from the guarded dispatch at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1786-1788`. A binding that does not opt in produces the same
request bytes, the same temporary file, the same HTTP-200 gate, the same log lines and the
same tree it produced before JSON support existed.

**`response-format="json"` — `Accept: application/json` is sent and a BadgerFish response
is translated.** The value is compared case-insensitively
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1584`), so `json`, `JSON` and `Json` all opt
in. The request then carries one additional header, `Accept: application/json`, appended
between the existing `Content-Type` header and the point where the header list is handed to
libcurl (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1649-1655`). Nothing else about the
request changes: the form body, the user agent `freeswitch-xml/1.0`, the redirect limit and
every TLS option are what they were. If the header cannot be appended at all — an
allocation failure inside libcurl — the fetch still happens and the format decision is
lowered back to XML on the spot, with a `WARNING`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1650-1654`), so the decoder can never expect
a representation this fetch did not ask for.

**Any JSON failure edge degrades to the untouched XML parse, so a lookup never fails
because of a decode problem.** The JSON decoder returns `NULL` on every failure edge, and
the caller's next statement parses the same already-downloaded body as XML
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1790-1794`). The HTTP-200 success gate — the
`if (httpRes == 200)` at `:1779` — and the non-200 error branch that logs `Received HTTP error`
at `:1818-1822` are unchanged, and the `NULL`-return semantics the core sees are unchanged. A gateway that answers XML while a binding asks for
JSON is therefore still served — degraded in fidelity, never in availability.

**One thing the two paths do not share: `$${…}` interpolation.** The XML path returns through
`switch_xml_parse_file()` (`src/switch_xml.c:1724`), which runs the configuration preprocessor
over the body before parsing it (call at `src/switch_xml.c:1757`, `preprocess()` at
`src/switch_xml.c:1460`). That preprocessor expands `$${variable}` from the switch's global
variables (`expand_vars()` at `src/switch_xml.c:1305-1330`, called per line at
`src/switch_xml.c:1496`) and acts on `X-PRE-PROCESS` directives (`src/switch_xml.c:1523`). The
JSON path does none of it: the translator builds the tree directly through the XML builder API,
so a `$${…}` token in a JSON response reaches the core **verbatim**. One document served both
ways therefore differs — `value="10.236.2.230"` as XML against `value="$${domain}"` as
BadgerFish JSON, on a switch whose `domain` is set.

Two consequences worth planning for. **A gateway that moves a binding from XML to JSON must
resolve those tokens itself and send final values**, because nothing warns about it: a document
full of literal `$${…}` is well-formed and conformant, so it decodes successfully and the
difference only shows up in the value the dialplan or directory finally sees. And an *unset*
variable is no safer than a set one — the preprocessor writes nothing for a variable it cannot
resolve (`src/switch_xml.c:1327`), so the XML path drops the token while the JSON path keeps it.
If a value has to be computed switch-side rather than gateway-side, leave that binding on XML.
The `file:` shortcut of section 4 is on the XML path and so it does interpolate.

**The single dispatch point and the single signal site.** There is exactly one place where
the format is chosen — `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1786-1787` — and exactly one
place where a degradation is signalled: the `if (!xml)` block at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1410-1422`, which emits the `WARNING` and
fires the event side by side. Alerting therefore never has to correlate across sites: one
degradation produces one `WARNING` and one event, always together.

---

## 2. Both fallback reasons and their stable signatures

A total fallback is a silent degradation: the lookup resolves, nothing fails, and without a
signal an operator only learns of it by reading logs. The module therefore emits two
signatures for the same event, from the same place, and they pair 1:1.

### 2.1 The log signature

One `SWITCH_LOG_WARNING`, from `xml_curl_json_decode_response_ex()`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1411-1416`) — the `_ex` suffix is part of the
name, and it is the only spelling that exists in the module. The format string is stable and
is not changed by this work:

```
JSON decode of the [%s] response from [%s] failed (%s) [Content-Type: %s]; falling back to XML parsing
```

The four operands are, in order: the requested provisioning section; the gateway URL,
redacted to its authority so a configured credential or query token is never written to the
log (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1414`); the human-readable reason; and the
response `Content-Type` as the gateway sent it. Section, URL and `Content-Type` are all
bounded and sanitized before they are rendered, so a hostile `Content-Type` cannot forge a
second log line.

The reason operand is one of **five** human-readable phrases
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1391-1405`), reproduced here exactly:

| # | Reason phrase in the `WARNING` | Line |
|---|---|---|
| 1 | `no response body was captured` | `mod_xml_curl.c:1392` |
| 2 | `the requested provisioning section is unknown` | `mod_xml_curl.c:1395` |
| 3 | `response Content-Type is not application/json` | `mod_xml_curl.c:1398` |
| 4 | `response body could not be read in full` | `mod_xml_curl.c:1401` |
| 5 | `response body is not a well-formed BadgerFish JSON document for the requested section` | `mod_xml_curl.c:1404` |

A sixth string, `unknown translation error`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1385`), is the initialiser of the same
variable. It describes no reachable edge: the five-armed `if`/`else if` ladder above it
assigns one of the five phrases on every path that can produce a `NULL` document, so it is a
defensive default rather than a sixth reason to alert on.

### 2.2 The event signature

One `SWITCH_EVENT_CUSTOM` with subclass **`xml_curl::json_fallback`**, fired by
`xml_curl_json_fire_fallback_event()`
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1296-1326`). The subclass name is the constant
at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:233`; it is reserved when the module loads
(`:2137`) and released when it shuts down (`:2167`), so it is discoverable by name in
`fs_cli`. Reservation is best-effort: emission does not depend on it, so at worst
discoverability is degraded and the event still fires.

The event carries exactly three headers:

| Header | Value | Line |
|---|---|---|
| `Binding` | the `name` attribute of the `<binding>` whose fetch degraded, sanitized; the literal `(unnamed)` when the configuration named none | `mod_xml_curl.c:1311-1315`, constant at `:236` |
| `Fallback-Reason` | exactly one of `content-type-mismatch` or `malformed-json` | `mod_xml_curl.c:1316`, constants at `:234-235` |
| `Gateway` | the binding's gateway URL, **redacted** through the same helper the `WARNING` uses, so userinfo, path and query never reach a subscriber | `mod_xml_curl.c:1317` |

Emission is fire-and-forget and fully guarded: the helper returns `void`, the caller ignores
it, and every failure edge inside it — an event that could not be created, a header that could
not be added, a dispatcher that refused it — leaves the module's behaviour and the document it
returns exactly as they were (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1304-1325`).
Observability cannot change provisioning.

**All three headers or none.** `switch_event_add_header_string()` returns a status and refuses
a value it cannot store, so the three-header contract above holds only because every one of
those statuses is checked (`mod_xml_curl.c:1315-1317`). On the first refusal the incomplete
event is destroyed and nothing is fired (`:1321`), because a subscriber cannot tell a header
that was refused from a header the module chose not to send — an absent `Fallback-Reason`
would read as a different class of degradation and alert on the wrong thing. **A subscriber
may therefore rely on all three headers being present on every event it receives.** The
abandonment is deliberately silent: the `WARNING` of section 2.1 has already reported the
degradation.

**One JSON-to-XML degradation deliberately fires no event, and it is worth knowing about if
you alert on the event alone.** Before the request goes out, the module appends
`Accept: application/json` for a JSON binding. If that one `switch_curl_slist_append()` fails
— an allocation failure inside libcurl's list, nothing a gateway can cause — the module logs
its own `SWITCH_LOG_WARNING`, stops asking for JSON for that request, and decodes the response
as XML (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1649-1655`). That is a JSON-to-XML
degradation, but it happens **upstream of the decode**, so no `xml_curl::json_fallback` event
is fired: the event is bound to the single decode-time fallback site, which is what makes the
1:1 pairing in section 2.4 exact. The consequence for monitoring is narrow but real — an
event-only alert will not see it, so keep the `WARNING` channel in the picture as section 2.4
recommends. Its distinguishing text begins `Could not add the Accept:` and it is the only
fallback `WARNING` in the module that does not carry the `JSON decode of the [...] response`
prefix, so the two are trivially separable in a log query.

### 2.3 Which failure edge maps to which reason

The `WARNING` carries five prose reasons; the event carries a two-valued machine taxonomy
over them. The mapping is fixed in the code
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1391-1405`) and is total:

| `Fallback-Reason` | Failure edges that produce it | Line |
|---|---|---|
| `content-type-mismatch` | **Only** the `Content-Type` classifier rejection: the response's declared media type is not `application/json`. This is the "the gateway answered in a representation this binding did not ask for" class. | `mod_xml_curl.c:1397-1399` |
| `malformed-json` | **Every** other edge: no response body was captured (`:1391-1393`); the requested provisioning section is unknown (`:1394-1396`); the response body could not be read in full (`:1400-1402`); the body is not a well-formed BadgerFish document for the requested section (`:1403-1405`). This is the "a body this module could not turn into a document" class. | `mod_xml_curl.c:1391-1405` |

The classifier itself accepts `application/json` bare or carrying parameters such as
`; charset=utf-8`, and rejects an absent or empty header, `text/xml`, `application/xml` and
every other media type, comparing type and subtype as one case-insensitive token
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:699-728`). A gateway that sends the right
bytes with the wrong header therefore reports `content-type-mismatch`, not
`malformed-json` — which is exactly the distinction worth alerting on separately, because
its remedy is a gateway header fix rather than a payload fix.

Note the practical consequence for a mismatch: the ceilings in section 3 are never reached
on that edge, because the classifier rejects before the body is read at all
(`mod_xml_curl.c:1398` precedes `mod_xml_curl.c:1401`).

Two of these five reasons are worth naming precisely, because their wording invites a wrong
reading:

- `response body could not be read in full` is the **read** edge, produced by
  `xml_curl_json_read_file()` (`mod_xml_curl.c:747-809`) returning `NULL`. Its triggers are a
  temporary file that is empty, cannot be opened or stat'd, shrinks while it is being read, or
  contains an embedded NUL byte (`:765`, `:801`). It is **not** what a body over
  `response-max-bytes` produces — that never reaches this function at all, for the reason set
  out in section 3, row 10.
- `no response body was captured` and `the requested provisioning section is unknown` are
  guards on the decoder's own arguments (`:1391-1396`) rather than on anything a gateway sends.
  Neither is reachable over the HTTP path, because the fetch routine always synthesises a
  temporary-file name (`:1634`) and `switch_xml_locate` always supplies a section. They exist so
  the decoder is total, and they are exercised by the module's test suite rather than by traffic.

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
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1313`, added as a header at `:1317`).

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

Because the event and the `WARNING` are emitted from the same block — the `if (!xml)` block at
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1410-1422`) — the two counts pair 1:1 over any
interval. That makes the log useful as a cross-check on the alerting path rather than as its
input: if the log shows fallback `WARNING`s that the event stream did not, the subscription
is the thing to look at.

**What the event cannot tell you.** The event is a *decode* signal, so it covers exactly the
five failure edges of section 2.3 and nothing else. A fetch that never produced a body worth
decoding emits **neither** the event **nor** a fallback `WARNING` — it is reported only by the
module's pre-existing log lines, which `response-format` leaves exactly as they were:

| Failure | What is logged, in order | Severity and line |
|---|---|---|
| The response body exceeded `response-max-bytes` (section 3, row 10) | `Oversized file detected [N bytes]` then `Error encountered! [<gateway-url>]` | `ERROR` at `mod_xml_curl.c:137`, `ERROR` at `:1776` |
| The transfer failed — timeout, connection refused, TLS rejection | `CURL returned error:[<n>] <message>` then `Received HTTP error 0 trying to fetch <gateway-url>` | `WARNING` at `mod_xml_curl.c:1753`, `ERROR` at `:1819` |
| The gateway answered a status other than 200 | `Received HTTP error <code> trying to fetch <gateway-url>` | `ERROR` at `mod_xml_curl.c:1819` |

That is the log-only class. An alerting rule that watches the event alone will not see any of
it, so pair the event rule with a log rule if you need to notice a gateway that stops answering
as well as one that answers badly: `Received HTTP error` and `Oversized file detected` are the
two strings that cover the whole table, and neither has changed in years.

One caution when you do scrape for them: those three lines render the gateway URL
**unredacted**, unlike every line `response-format` added. That is deliberate — they are
pre-existing operator-facing diagnostics and changing them would change behaviour for bindings
that never opted into JSON — but it means a `gateway-url` carrying userinfo or a query token
will appear in the log whenever the transport fails. Keep credentials in
`gateway-credentials` with `auth-scheme` instead, which no log line ever renders.

For a one-off diagnosis rather than an alert, `fs_cli -x 'xml_curl debug_on'` leaves each
fetched response body on disk and logs its path
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1825-1827`), which is the fastest way to see
the bytes a gateway actually sent. Note that the temporary file keeps its historical
`.tmp.xml` suffix even for a JSON payload
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1634`); the suffix is cosmetic and deliberately
unchanged, since it appears in that operator-visible log line.

---

## 3. The eleven resource ceilings

The JSON decoder is a whitelist, not a best-effort parser, and it is bounded so that a
runaway or hostile response cannot exhaust memory or CPU on the fetch thread. The decoder's
own ceilings — every row below except row 10, which is the operator-settable response cap —
are compile-time constants enforced by the module itself rather than parser configuration, so
behaviour is identical on every platform and every build
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:173-177`).

Ceilings 1 to 9 and 11 behave the same way when exceeded: the document is **refused** — the
decoder returns `NULL` — which is one of the `malformed-json` edges of section 2.3, so the
response is parsed as XML instead. There is no partial acceptance and no truncation.

A refusal can never by itself fail the fetch, but whether the *lookup* then resolves depends
on what the body actually holds. A gateway that answered XML is still served in full. A
JSON-only document that this decoder refused will not parse as XML either, so a lookup that
only that gateway could satisfy is a provisioning **miss** rather than a loss of fidelity —
`xml_locate` reports `can't find anything` and the XML parser logs its own error about the
JSON bytes it was handed. When some other source can satisfy the same lookup the core reaches
it, because `switch_xml_locate` retries the static root after a binding misses
(`src/switch_xml.c:1861-1868`). Section 1's phrasing is the precise one: degraded in fidelity,
never in availability of the **fetch**.

**Row 10 is the exception to everything above** and is worth reading on its own:
`response-max-bytes` is enforced on the transport, before any decode is attempted, so
breaching it is not a fallback at all.

| # | Name | Value | What it bounds | On breach |
|---|---|---|---|---|
| 1 | `XML_CURL_JSON_MAX_DEPTH` (`mod_xml_curl.c:193`) | 32 | Nesting levels in the JSON document | Refused at `:604` (lexical gate) and `:905` (translation) -> fallback |
| 2 | `XML_CURL_JSON_MAX_VALUES` (`mod_xml_curl.c:194`) | 50000 | Objects + arrays + strings in the whole document | Refused at `:607`, `:629` -> fallback |
| 3 | `XML_CURL_JSON_MAX_OBJECT_MEMBERS` (`mod_xml_curl.c:195`) | 256 | Members of one JSON object | Refused at `:849` -> fallback |
| 4 | `XML_CURL_JSON_MAX_ARRAY_ELEMENTS` (`mod_xml_curl.c:196`) | 256 | Elements of one array, i.e. repeated children under one name | Refused at `:956`, before anything is built -> fallback |
| 5 | `XML_CURL_JSON_MAX_CHILDREN_PER_PARENT` (`mod_xml_curl.c:197`) | 256 | Child elements built under one parent, accumulated across all member names | Refused at `:973`, `:994` -> fallback |
| 6 | `XML_CURL_JSON_MAX_NODES` (`mod_xml_curl.c:198`) | 20000 | Elements + attributes built in total | Refused at `:926`, `:965`, `:988` -> fallback |
| 7 | `XML_CURL_JSON_MAX_STRING_BYTES` (`mod_xml_curl.c:199`) | 8192 | Bytes in one name or one value | Refused at `:399`, `:681` -> fallback |
| 8 | `XML_CURL_JSON_MAX_NAME_BYTES` (`mod_xml_curl.c:200`) | 128 | Bytes in one element or attribute name | Refused at `:470` -> fallback |
| 9 | `XML_CURL_JSON_MAX_TRANSFORMED_NAME_BYTES` (`mod_xml_curl.c:211`) | `MAX_NODES * (MAX_NAME_BYTES + 1)` = 2580000 | Cumulative element and attribute **name** bytes written into the tree. It is a ceiling of its own rather than the payload length because an array writes one key once per element, so the output legitimately exceeds the input | Refused at `:931`, `:970` -> fallback |
| 10 | `XML_CURL_MAX_BYTES` / `response-max-bytes` (`mod_xml_curl.c:76`, default assigned to `curl_max_bytes` at `:1876`, parameter parsed at `:1967`, handed to the transfer at `:1639`) | 1 MiB (`1024 * 1024`), per-binding override | The HTTP response **body** — enforced on the **transport**, not in either decoder. `file_callback()` counts every chunk libcurl delivers and aborts the transfer as soon as the running total passes the cap (`:128-140`). It therefore caps a JSON body exactly as it caps an XML one, because it acts before either representation is looked at. The decoder does re-check the same binding ceiling when it reads the body back (call at `:1400`, helper `xml_curl_json_read_file` at `:747`, test at `:765`), but that is defence in depth: the transport cap and the read cap are the same `binding->curl_max_bytes`, so the transport always fires first | **Not a fallback — see the note below.** `file_callback()` sets the fetch's error flag (`:138`) and the result block tests that flag (`:1775`) **before** the format dispatch (`:1786`), so the JSON decoder is never entered. Two `ERROR` lines are logged — `Oversized file detected [N bytes]` (`:137`) and `Error encountered! [<gateway-url>]` (`:1776`) — **no fallback `WARNING` and no `xml_curl::json_fallback` event fire**, and the fetch yields no document |
| 11 | The budget's `max_text_bytes` (`mod_xml_curl.c:255`, bound at `:1135`) | `strlen(json_text)` — the payload length | Cumulative decoded attribute-value and element-text bytes. Bounded by the payload rather than by a constant, because every decoded value appears exactly once in the payload and escape sequences only ever shrink | Refused at `:934`, `:943` -> fallback |

**Row 10 is the one ceiling whose breach is invisible to section 2's signatures.** A body over
`response-max-bytes` is a transport failure rather than a decode failure, so it produces the two
`ERROR` lines above and nothing else: no `WARNING`, no event, and a lookup that succeeds only if
another source can satisfy it. An alerting rule keyed on the `xml_curl::json_fallback` event
alone (section 2.4) will not see it, so watch the log for `Oversized file detected` as well. That
line is the pre-existing signal for a gateway that has outgrown its cap and it long predates
`response-format`; the only thing JSON support changes about this edge is nothing at all. Note
also that the reason phrase `response body could not be read in full` belongs to the read edge in
section 2.3 — an empty, unreadable, shrinking or NUL-bearing body — and **not** to this row.

Ceilings 1 through 8 and 11 are document-wide rather than per-node because a single
caller-owned budget structure is threaded through the whole recursive translation
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:250-257`), so a wide-and-shallow document
cannot amplify past a deep-and-narrow one. Ceiling 5 exists because this translator passes a
constant insertion offset so that insertion order becomes document order, which makes `n`
children under one parent cost O(n^2) comparisons synchronously on the fetch path; capping
one parent's width is what stops a merely wide response becoming a denial of service
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:179-191`).

### 3.1 A note on nesting: 32 enforced, 64 compiled, 1000 only as a fallback

**Four** different nesting numbers exist in this tree. Only one of them governs, but the other
three are all cited in the wild, so each is pinned here to the line that produces it:

- **32** is the ceiling this module enforces: `XML_CURL_JSON_MAX_DEPTH`
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:193`). **This is the operative number.** A
  document nested deeper than 32 levels is refused and falls back. It is enforced by the
  module on every platform precisely so that the parser's configuration cannot move the
  boundary.
- **64** is what `CJSON_NESTING_LIMIT` actually is in any build produced by this tree's
  autotools configuration: `configure.ac:313-314` adds `-DCJSON_NESTING_LIMIT=64` to both
  `SWITCH_AM_CFLAGS` and `SWITCH_AM_CXXFLAGS`, with the comment that the upstream default can
  overflow small thread stacks. **This, not 1000, is the vendored parser's effective limit
  here** — and it is still looser than 32, so the engineering conclusion is unchanged.
- **1000** is only the header's *fallback* value: `src/include/switch_cJSON.h:128-130` defines
  it under `#ifndef CJSON_NESTING_LIMIT`, so the definition is skipped whenever the build
  supplies its own. In an autotools build it is **never in force**. It matters only to a build
  that compiles cJSON without this tree's flags — a hand-rolled or IDE build, for instance.
- **31** is where the parity corpus's nesting-boundary fixture sits — one level inside the
  enforced ceiling, which is the boundary worth testing
  (`src/mod/xml_int/mod_xml_curl/test/fixtures/directory_nesting_boundary.json`, asserted by
  `src/mod/xml_int/mod_xml_curl/test/test_mod_xml_curl.c:5606`, whose own comment records the
  same four numbers).

Why the module refuses first, whichever of 64 or 1000 is in force: its lexical gate rejects at
32 (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:604`) and its translation budget rejects at 32
again (`:905`), both before cJSON's own recursion limit can ever apply, so the boundary is
identical on every platform and in every build however that macro is set (`:173-177`).

If you are sizing a gateway's documents, **32** is the number to design against.

---

## 4. The `file:` XML-only boundary

A `file:` gateway URL is **always** XML, and `response-format` cannot change that.

The fetch routine detects the `file:` prefix, reads the file straight off disk with
`switch_xml_parse_file()` and returns
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1591-1598`). That return precedes every part
of the HTTP path: no request is made, so no `Accept` header is sent — the only append site is
at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1649`, downstream of the return — and no
content type is negotiated or read, since the `CURLINFO_CONTENT_TYPE` probe is at
`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1763`, also downstream. The JSON decoder is
never reached, so a `file:` binding fires no `xml_curl::json_fallback` event either.

Setting `response-format="json"` on a `file:` binding is therefore inert, not an error, and
produces no warning. If you want a local JSON document decoded, serve it over HTTP(S) from a
gateway that sets `Content-Type: application/json`.

---

## 5. TLS hardening: `enable-cacert-check`, `enable-ssl-verifyhost`, `ssl-cacert-file`

### What each parameter does

| Parameter | Effect | Lines |
|---|---|---|
| `enable-cacert-check` | When true, sets `CURLOPT_SSL_VERIFYPEER` back on, so libcurl verifies that the gateway's certificate chains to a trusted CA | parsed at `mod_xml_curl.c:1932`, applied at `:1705` (the `CURLOPT_SSL_VERIFYPEER, TRUE` setopt) |
| `enable-ssl-verifyhost` | When true, sets `CURLOPT_SSL_VERIFYHOST` to 2, so libcurl verifies that the certificate actually names the host being contacted | parsed at `mod_xml_curl.c:1944`, applied at `:1733` (the `CURLOPT_SSL_VERIFYHOST, 2` setopt) |
| `ssl-cacert-file` | Sets `CURLOPT_CAINFO` to a PEM bundle, so a private or internal CA can be trusted instead of the system trust store. Only meaningful together with `enable-cacert-check` | parsed at `mod_xml_curl.c:1942`, applied at `:1729` (the `CURLOPT_CAINFO` setopt) |

Both booleans are parsed with `switch_true()` and the arm only matches when the value is true —
the `enable-cacert-check` arm at `mod_xml_curl.c:1932` and the `enable-ssl-verifyhost` arm at
`:1944` — so writing `value="false"` is equivalent to omitting the parameter rather than being
an explicit opt-out.

### Why the shipped defaults are permissive, and unchanged by this work

For any `https` gateway URL the module unconditionally disables both checks before applying
any per-binding option: `CURLOPT_SSL_VERIFYPEER` is set to 0 and `CURLOPT_SSL_VERIFYHOST` to
0 at `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1657-1660`. The three parameters above are
opt-in overrides applied later in the same function. So out of the box, an `https`
provisioning fetch is encrypted but **not authenticated** — a machine-in-the-middle can
present any certificate and serve the switch its provisioning documents.

That posture is long-standing and is deliberately left byte-for-byte as it was. Tightening
it would change the behaviour of every existing deployment whose gateway uses a certificate
that would not verify, turning a working switch into one that cannot provision — a
behaviour change well outside a documentation and observability change.

**No shipped profile sets any of the three**, so no default moves — but they do not all carry
the same samples, and it is worth knowing which file you are looking at:

| Profile | What it carries |
|---|---|
| `conf/vanilla/autoload_configs/xml_curl.conf.xml` | all three as commented samples: `:18` (`enable-cacert-check`), `:20` (`enable-ssl-verifyhost`), `:36` (`ssl-cacert-file`) |
| `conf/testing/autoload_configs/xml_curl.conf.xml` | the same three, at the same lines `:18`, `:20`, `:36` |
| `conf/curl/autoload_configs/xml_curl.conf.xml` | **none of the three.** Only a commented `ignore-cacert-check` sample at `:16` |
| `conf/insideout/autoload_configs/xml_curl.conf.xml` | **none of the three.** Only a commented `ignore-cacert-check` sample at `:16` |

`ignore-cacert-check` is a legacy spelling that this module has never had a parameter arm for —
the parsing chain recognises `enable-cacert-check` and no variant of it
(`mod_xml_curl.c:1932`) — so uncommenting it in `conf/curl` or `conf/insideout` verifies
nothing. If you start from one of those two profiles, add
`<param name="enable-cacert-check" value="true"/>` by hand rather than reaching for the sample
that is already there.

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
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1742-1745`).

### Why the jar must sit in a directory other users cannot write

libcurl opens the jar for writing at the end of **every** transfer. That open follows
symbolic links and creates a missing file with whatever the process umask happens to be
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1428-1436`). So an unvalidated path in a
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
libcurl's open (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1503`). The rule is
deliberately not "owner only": a service directory such as `$${db_dir}` is conventionally
readable and traversable by others, and demanding mode 0700 there would refuse the very
location the sample recommends
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1449-1455`).

### What the module does when the path is unacceptable

It **refuses the jar, keeps performing the fetch, and logs**. The validation function
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1464`) gates only the two cookie `setopt`
calls — `CURLOPT_COOKIEJAR` and `CURLOPT_COOKIEFILE` at `:1743-1744`; everything else about the
fetch is untouched, so a refusal costs cookie persistence for that binding and nothing else. The refusal is a single
`SWITCH_LOG_WARNING` (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1532-1535`):

```
Refusing cookie file [%s] for the binding at [%s] because %s; cookies will not be persisted for this binding
```

The reasons it can carry are these, and they say nothing at all on success:

| Reason phrase | Line |
|---|---|
| `its path is too long to be validated` | `mod_xml_curl.c:1483` |
| `the directory holding it could not be read` | `mod_xml_curl.c:1499` |
| `the path holding it is not a directory` | `mod_xml_curl.c:1501` |
| `the directory holding it is writable by other users and not sticky, so the name could be replaced after it is checked` | `mod_xml_curl.c:1503` |
| `it is not a regular file, so a symbolic link, directory or special file would be written through` | `mod_xml_curl.c:1514` |
| `it belongs to another user` | `mod_xml_curl.c:1516` |
| `other users can write it, so its contents cannot be trusted` | `mod_xml_curl.c:1518` |
| `its status could not be read` | `mod_xml_curl.c:1523` |
| `it does not exist and could not be created privately` | `mod_xml_curl.c:1525` |

The last two are the only ones you are unlikely ever to see. Both require an `lstat` or an
`open` in the jar's directory to be refused, and a FreeSWITCH running as `root` — still the
common case for a switch that binds privileged ports — bypasses the directory permissions that
would refuse them. Run the switch as its own unprivileged user (`freeswitch -u freeswitch -g
freeswitch`) and they become reachable like the rest.

An existing jar that is a regular file, owned by this process and writable by nobody else,
is accepted untouched and its mode is never altered
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1512-1520`). A jar that does not exist yet is
created here with `O_EXCL` and no-follow semantics at mode 0600
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1524-1528`), so libcurl later truncates a file
that is already private instead of creating a public one. The check is validated at fetch
time rather than at configuration time, so a jar replaced by a symbolic link after the module
loaded is caught just the same
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1736-1741`). On Windows the check is a no-op
and behaviour is byte-for-byte what it was, because that platform has neither the link
semantics nor the ownership model the check is written against
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1466-1467`).

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
`#6 mod_h323_load … mod_h323.cpp:167` — those two line numbers are gdb's own output against
the **unguarded** sources, where they are the `new FSProcess()` statements; in the guarded tree
the same statements are `mod_opal.cpp:199` and `mod_h323.cpp:249`); and the fault was **not**
in `dl_init`, a library
constructor or a static initialiser — a `dlopen(RTLD_NOW|RTLD_LOCAL)`-only probe of both
modules in one process completed with exit 0. Because module code runs first, module code can
refuse.

The guard is **two stages**, and both run before the `new FSProcess()` that creates the
conflicting singleton.

**Stage 1 — name the sibling.** Each load function's first statement asks whether the sibling
is already in the core's module hash, and refuses if it is. This is the common case and the
only stage that can name the offending module in its message.

- `mod_h323_load` refuses when `switch_loadable_module_exists("mod_opal")` succeeds
  (`src/mod/endpoints/mod_h323/mod_h323.cpp:198`).
- `mod_opal_load` refuses when `switch_loadable_module_exists("mod_h323")` succeeds
  (`src/mod/endpoints/mod_opal/mod_opal.cpp:145`).

**Stage 2 — claim the runtime atomically.** Stage 1 alone is only a *snapshot*:
`switch_loadable_module_exists()` takes the module-hash lock for its own lookup and releases
it, and the core publishes a module into that hash only *after* the module's load routine has
returned. Nothing serialises the interval in between, so two concurrent load requests — one
per endpoint — can both see the sibling absent and both go on to construct a `PProcess`,
which is exactly the operation that kills the process. Unloading has the same window in
reverse: the sibling leaves the hash before its shutdown has torn PTLib down.

Each module therefore claims one process-global reservation with a compare-and-swap before it
constructs anything:

| | |
|---|---|
| Reservation variable | `_fs_ptlib_endpoint_reservation` — spelled `H323_PTLIB_RESERVATION` in `mod_h323.cpp:162` and `OPAL_PTLIB_RESERVATION` in `mod_opal.cpp:109`, deliberately the identical string so the two modules contend for **one** entry. The `_fs_` prefix keeps it clear of any name an operator would pick |
| Claimed with | `switch_core_set_var_conditional(<RESERVATION>, modname, "")` — it holds `runtime.global_var_rwlock` in **write** mode across the whole test-and-set |
| Claim sites | `src/mod/endpoints/mod_h323/mod_h323.cpp:221`, `src/mod/endpoints/mod_opal/mod_opal.cpp:169` |
| Released on | every load-failure edge after the claim, and **last** in shutdown — after the `FSProcess` is deleted, so the claim also spans the unload window |
| Release sites | `mod_h323.cpp:243`, `:252`, `:268`, `:290`; `mod_opal.cpp:195`, `:201`, `:215`, `:233` |

The release passes the module's own name as the expected value, so a second shutdown, or a
shutdown after a refused load, is a no-op rather than a way to free another module's claim.
No core API was added for this: `switch_core_set_var_conditional()` is already exported to
modules and already used by `mod_commands` and `mod_v8`.

**Inspecting it.** `fs_cli -x 'global_getvar _fs_ptlib_endpoint_reservation'` names the module
holding the PTLib runtime, or returns empty when neither endpoint is loaded.

**One consequence worth knowing before you go looking for it.** The reservation is an ordinary
core global variable, so it is writable by an operator — `global_setvar` takes
`<var>=<value>`, i.e. `fs_cli -x 'global_setvar _fs_ptlib_endpoint_reservation=anything'`, and
`X-PRE-PROCESS set` in a configuration file does the same. There is no core facility for a
variable that is readable but not writable. Setting `_fs_ptlib_endpoint_reservation` to any
non-empty value by hand therefore makes **both** endpoints refuse to load, with an `ERROR`
reporting that the runtime is `already reserved by [<your value>]` — the refusal comes from the
compare-and-swap at `mod_h323.cpp:221` / `mod_opal.cpp:169` and is logged at `mod_h323.cpp:224` /
`mod_opal.cpp:172`, which is a different line from the sibling-already-loaded refusal above. That is a configuration mistake rather than a
security boundary: the crash this section exists to prevent stays prevented either way, because
the sibling-in-the-hash check runs first and is independent of the reservation. If both
endpoints refuse and neither is loaded, read this variable first — an unexpected value in it is
the diagnosis. The `_fs_` prefix exists so that value cannot get there by accident.

Beyond the two guard stages and the four release sites per module tabulated above, nothing in
either module is changed — the exact per-file extent is tabulated in
`blitzy/documentation/oos9-coload-determination.md` §7.

### What an operator observes when the guard fires

The second load is **refused**, and the process **survives**. Concretely:

- An `ERROR` naming the singleton conflict and both PTLib runtimes is written to the log,
  for example (`src/mod/endpoints/mod_h323/mod_h323.cpp:199-202`):

  ```
  Refusing to load mod_h323: mod_opal is already loaded in this process, and their conflicting
  PProcess singletons - one per PTLib runtime, libpt.so.2.10.9 for mod_h323 against
  libpt.so.2.12-beta10 for mod_opal - crash the process (OOS-9). Run the two endpoints in
  separate FreeSWITCH instances.
  ```

- The load function returns `SWITCH_STATUS_FALSE`
  (`src/mod/endpoints/mod_h323/mod_h323.cpp:203`,
  `src/mod/endpoints/mod_opal/mod_opal.cpp:179`), which the core reports as
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
(`src/mod/endpoints/mod_h323/test/test_mod_h323.cpp:3402`,
`src/mod/endpoints/mod_opal/test/test_mod_opal.cpp:3506`) — which exercises the guard against
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
(`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:159-171`):

- **Exactly one top-level key**, and it must name the provisioning section that was
  requested — `configuration`, `directory` or `dialplan`
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1034`).
- That key's value is the `<section>` element's content, as a JSON object. The module
  synthesises the `<document type="freeswitch/xml"><section name="…">` envelope itself
  (`src/mod/xml_int/mod_xml_curl/mod_xml_curl.c:1113-1118`), so the gateway must **not** emit
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
- **Every value is used verbatim.** `$${…}` global-variable interpolation happens on the XML
  decode path only (section 1), so send resolved values: a literal `$${domain}` in a JSON
  response reaches the dialplan or directory as those eleven characters, not as the switch's
  domain.
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
for a shape reason. Three things the validator cannot check for you: that the gateway sets
`Content-Type: application/json` — that is the `content-type-mismatch` edge of section 2.3 —
that the response body stays inside the binding's `response-max-bytes` ceiling (section 3,
row 10), and that no value is a `$${…}` token left for the switch to expand, which decodes
without complaint and then means something different from what the same document meant as XML
(section 1). The last of those is a contract the corpus keeps rather than states: no fixture
contains such a token, and the suite fails if one is ever added
(`FST_TEST_BEGIN(fixture_corpus_carries_no_preprocessor_tokens)` in
`src/mod/xml_int/mod_xml_curl/test/test_mod_xml_curl.c`).
