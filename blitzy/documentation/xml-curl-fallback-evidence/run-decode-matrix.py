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
# Contributor(s):
# Blitzy Agent <agent@blitzy.com>
#
"""Live five-case BadgerFish decode matrix for mod_xml_curl, with 1:1 signal pairing.

WHAT THIS PROVES
    mod_xml_curl signals a JSON-to-XML total fallback twice from one site: a
    SWITCH_LOG_WARNING for a human and a SWITCH_EVENT_CUSTOM subclass
    `xml_curl::json_fallback' for a machine.  A unit test can assert that the two
    calls sit together in one function; only a live run can prove that a real
    provisioning lookup, against a real HTTP gateway, through a real event
    dispatch queue, delivers exactly one event per warning - and delivers NONE at
    all when the opt-in `response-format' parameter is absent.

    This script drives all five decode branches against a running FreeSWITCH and
    reports, per case: the value the lookup resolved, how many fallback WARNINGs
    the log gained, how many `xml_curl::json_fallback' events an ESL consumer
    received, and the Fallback-Reason / Binding / Gateway headers each event
    carried.  It then asserts warnings == events on every case.

WHAT IT NEEDS
    * python3 and its standard library.  Nothing else: no ESL bindings, no curl.
    * a running FreeSWITCH with mod_xml_curl and mod_event_socket loaded, whose
      ESL is reachable with the password given below.
    * permission to rewrite that instance's autoload_configs/xml_curl.conf.xml.
      The original is backed up and restored, including on failure.

WHY IT IS SAFE TO RUN
    The gateway listens on loopback only.  No call is placed, no media flows, and
    the only instance state touched is one configuration file and the module's own
    load/unload.  Nothing outside the instance's own directories is modified.

USAGE
    python3 run-decode-matrix.py [--prefix DIR] [--esl-port N] [--gateway-port N]

    Defaults match a stock install: --prefix /usr/local/freeswitch, ESL on
    127.0.0.1:8021 with password ClueCon.  Exit status is 0 only when every case
    matched its expectation AND every case paired 1:1.
"""

import argparse
import json
import os
import re
import shutil
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

# ---------------------------------------------------------------------------
# The provisioning gateway
# ---------------------------------------------------------------------------
# Four endpoints, one per representation the decoder has to cope with.  The JSON
# and XML bodies describe the SAME user with a DIFFERENT marker value, so the
# value the lookup resolves says which decoder produced the document - which is
# what makes the backward-compatibility case an observation rather than a claim.

BADGERFISH = {
    "directory": {
        "domain": {
            "@name": "127.0.0.1",
            "user": {
                "@id": "5001",
                "params": {"param": {"@name": "password", "@value": "badgerfish-secret"}},
                "variables": {"variable": {"@name": "provisioned_by", "@value": "badgerfish-json"}},
            },
        }
    }
}

XML_TWIN = (
    '<document type="freeswitch/xml">'
    '<section name="directory">'
    '<domain name="127.0.0.1">'
    '<user id="5001">'
    '<params><param name="password" value="classic-xml"/></params>'
    '<variables><variable name="provisioned_by" value="classic-xml"/></variables>'
    "</user>"
    "</domain>"
    "</section>"
    "</document>"
)

# Announced as JSON, truncated mid-document: the bytes are not a document at all.
MALFORMED_JSON = '{"directory": {"domain": {"@name": "127.0.0.1", "user": {"@id": "5001"'


class Gateway(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # keep the harness output clean
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)

        if self.path == "/json":
            body, ctype = json.dumps(BADGERFISH), "application/json"
        elif self.path == "/badjson":
            body, ctype = MALFORMED_JSON, "application/json"
        elif self.path == "/wrongtype":
            # A conformant JSON body announced as XML: the announcement is what the
            # classifier rejects, and the bytes are then not XML either.
            body, ctype = json.dumps(BADGERFISH), "text/xml"
        else:
            body, ctype = XML_TWIN, "text/xml"

        encoded = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


# ---------------------------------------------------------------------------
# A minimal ESL client
# ---------------------------------------------------------------------------
# The inbound event socket protocol is line oriented with Content-Length framed
# bodies, so a few dozen lines of socket code is the whole client.  Written out
# rather than reached for through a binding, so this script keeps its promise of
# needing the standard library and nothing else.


class Esl:
    def __init__(self, host, port, password, timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self._read_block()  # auth/request
        self.send("auth %s" % password)
        reply = self._read_block()
        if "+OK" not in reply[0].get("reply-text", ""):
            raise RuntimeError("ESL authentication refused: %r" % (reply,))

    def send(self, command):
        self.sock.sendall((command + "\n\n").encode())

    def _fill(self, want_bytes=None, want_blank_line=False):
        while True:
            if want_blank_line and b"\n\n" in self.buf:
                return
            if want_bytes is not None and len(self.buf) >= want_bytes:
                return
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("ESL connection closed")
            self.buf += chunk

    def _read_block(self):
        """Read one header block plus its body, if any.  Returns (headers, body)."""
        self._fill(want_blank_line=True)
        head, self.buf = self.buf.split(b"\n\n", 1)
        headers = {}
        for line in head.decode(errors="replace").splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                headers[key.strip().lower()] = value.strip()
        body = ""
        length = int(headers.get("content-length") or 0)
        if length:
            self._fill(want_bytes=length)
            body, self.buf = self.buf[:length].decode(errors="replace"), self.buf[length:]
        return headers, body

    def api(self, command):
        self.send("api " + command)
        while True:
            headers, body = self._read_block()
            ctype = headers.get("content-type", "")
            if ctype in ("api/response", "command/reply"):
                return body.strip() or headers.get("reply-text", "").strip()

    def subscribe(self, events):
        self.send("event plain " + events)
        headers, _ = self._read_block()
        if "+OK" not in headers.get("reply-text", ""):
            raise RuntimeError("ESL subscription refused: %r" % (headers,))


class EventCollector(threading.Thread):
    """Collect text/event-plain payloads off a dedicated ESL connection."""

    daemon = True

    def __init__(self, esl):
        super().__init__()
        self.esl = esl
        self.events = []
        self.lock = threading.Lock()
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                headers, body = self.esl._read_block()
            except Exception:
                return
            if headers.get("content-type") != "text/event-plain":
                continue
            fields = {}
            for line in body.splitlines():
                if ":" in line:
                    key, value = line.split(":", 1)
                    fields[key.strip()] = value.strip()
            with self.lock:
                self.events.append(fields)

    def snapshot(self):
        with self.lock:
            return list(self.events)


# ---------------------------------------------------------------------------
# The matrix
# ---------------------------------------------------------------------------

FALLBACK_WARNING = "falling back to XML parsing"

BINDING_TEMPLATE = """<configuration name="xml_curl.conf" description="cURL XML Gateway">
  <bindings>
    <binding name="%(binding)s">
      <param name="gateway-url" value="http://127.0.0.1:%(port)d%(path)s" bindings="directory"/>
%(format)s    </binding>
  </bindings>
</configuration>
"""

# name, gateway path, response-format on?, expected resolved value, expected
# fallbacks (None means "at least one"), expected Fallback-Reason
CASES = [
    ("json_happy_path", "/json", True, "badgerfish-json", 0, None),
    ("malformed_json", "/badjson", True, None, 1, "malformed-json"),
    ("wrong_content_type", "/wrongtype", True, None, 1, "content-type-mismatch"),
    ("xml_default_backward_compat", "/xml", False, "classic-xml", 0, None),
    ("xml_via_json_total_fallback", "/xml", True, "classic-xml", 1, "content-type-mismatch"),
]


def count_warnings(path):
    if not os.path.exists(path):
        return 0
    with open(path, "r", errors="replace") as handle:
        return sum(1 for line in handle if FALLBACK_WARNING in line)


def write_binding(conf_path, binding, port, path, with_format):
    fmt = '      <param name="response-format" value="json"/>\n' if with_format else ""
    with open(conf_path, "w") as handle:
        handle.write(BINDING_TEMPLATE % {"binding": binding, "port": port, "path": path, "format": fmt})


def apply_binding(control):
    """Make the module re-read the configuration just written.

    `reload' is unload-then-load and needs the module to be loaded already, which
    is not a given: the shipped sample leaves gateway-url commented out, so a
    stock instance refuses to load mod_xml_curl at startup with "Binding has no
    url!".  That is pre-existing behaviour rather than a fault, so the load is
    attempted after the reload rather than assumed to have happened.
    """
    reply = control.api("reload mod_xml_curl")

    if control.api("module_exists mod_xml_curl").strip() != "true":
        reply = control.api("load mod_xml_curl")

    return reply.strip()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", default="/usr/local/freeswitch")
    parser.add_argument("--conf-dir", default=None, help="defaults to <prefix>/conf")
    parser.add_argument("--log", default=None, help="defaults to <prefix>/log/freeswitch.log")
    parser.add_argument("--esl-host", default="127.0.0.1")
    parser.add_argument("--esl-port", type=int, default=8021)
    parser.add_argument("--esl-password", default="ClueCon")
    parser.add_argument("--gateway-port", type=int, default=18080)
    parser.add_argument("--settle", type=float, default=2.0, help="seconds to wait for async signals per case")
    args = parser.parse_args(argv)

    conf_dir = args.conf_dir or os.path.join(args.prefix, "conf")
    log_path = args.log or os.path.join(args.prefix, "log", "freeswitch.log")
    conf_path = os.path.join(conf_dir, "autoload_configs", "xml_curl.conf.xml")
    backup = conf_path + ".decode-matrix.bak"

    server = HTTPServer(("127.0.0.1", args.gateway_port), Gateway)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    control = Esl(args.esl_host, args.esl_port, args.esl_password)
    listener = Esl(args.esl_host, args.esl_port, args.esl_password)
    listener.subscribe("CUSTOM xml_curl::json_fallback")
    collector = EventCollector(listener)
    collector.start()

    print("mod_xml_curl live BadgerFish decode matrix")
    print("  instance   : %s" % args.prefix)
    print("  esl        : %s:%d" % (args.esl_host, args.esl_port))
    print("  gateway    : http://127.0.0.1:%d/{json,badjson,wrongtype,xml}" % args.gateway_port)
    print("  log        : %s" % log_path)
    print("  version    : %s" % control.api("version").splitlines()[0])
    print()

    if os.path.exists(conf_path):
        shutil.copy2(conf_path, backup)

    results = []
    failures = 0

    try:
        for name, path, with_format, expected_value, expected_fallbacks, expected_reason in CASES:
            write_binding(conf_path, name, args.gateway_port, path, with_format)

            warnings_before = count_warnings(log_path)
            events_before = len(collector.snapshot())

            apply_binding(control)
            time.sleep(0.4)

            resolved = control.api("user_data 5001@127.0.0.1 var provisioned_by").strip()
            time.sleep(args.settle)

            warnings = count_warnings(log_path) - warnings_before
            events = collector.snapshot()[events_before:]

            reasons = sorted({e.get("Fallback-Reason", "<absent>") for e in events})
            bindings = sorted({e.get("Binding", "<absent>") for e in events})
            gateways = sorted({e.get("Gateway", "<absent>") for e in events})

            problems = []
            if warnings != len(events):
                problems.append("warnings %d != events %d" % (warnings, len(events)))
            if expected_fallbacks == 0 and warnings != 0:
                problems.append("expected no fallback, saw %d" % warnings)
            if expected_fallbacks and warnings < 1:
                problems.append("expected a fallback, saw none")
            if expected_value is None:
                if resolved and "-ERR" not in resolved:
                    problems.append("expected the lookup to fail, resolved %r" % resolved)
            elif resolved != expected_value:
                problems.append("expected %r, resolved %r" % (expected_value, resolved))
            if expected_reason and reasons != [expected_reason]:
                problems.append("expected Fallback-Reason %r, saw %r" % (expected_reason, reasons))
            if not expected_reason and events:
                problems.append("expected no event, saw %r" % (reasons,))

            verdict = "PASS" if not problems else "FAIL"
            if problems:
                failures += 1

            print("%-28s %s" % (name, verdict))
            print("    response-format      : %s" % ("json" if with_format else "(absent)"))
            print("    gateway path         : %s" % path)
            print("    lookup resolved      : %s" % (resolved if resolved else "<lookup failed>"))
            print("    fallback WARNINGs    : %d" % warnings)
            print("    fallback events      : %d" % len(events))
            print("    Fallback-Reason      : %s" % (", ".join(reasons) if reasons else "-"))
            print("    Binding              : %s" % (", ".join(bindings) if bindings else "-"))
            print("    Gateway (redacted)   : %s" % (", ".join(gateways) if gateways else "-"))
            for problem in problems:
                print("    PROBLEM              : %s" % problem)
            print()

            results.append((name, warnings, len(events)))

        total_warnings = sum(w for _, w, _ in results)
        total_events = sum(e for _, _, e in results)

        print("pairing summary")
        for name, warnings, events in results:
            print("  %-28s WARNING %d : event %d   %s" % (name, warnings, events, "1:1" if warnings == events else "MISMATCH"))
        print("  %-28s WARNING %d : event %d" % ("TOTAL", total_warnings, total_events))
        print()

        if total_warnings != total_events:
            print("RESULT: FAIL - %d fallback WARNINGs against %d events" % (total_warnings, total_events))
            failures += 1
        elif failures:
            print("RESULT: FAIL - %d case(s) did not match expectations" % failures)
        else:
            print("RESULT: PASS - %d of %d cases matched, every fallback WARNING paired 1:1 with one "
                  "xml_curl::json_fallback event, and the response-format-absent case fired none"
                  % (len(CASES), len(CASES)))
    finally:
        collector.stop = True
        server.shutdown()
        if os.path.exists(backup):
            shutil.move(backup, conf_path)
            try:
                control.api("reload mod_xml_curl")
            except Exception:  # the instance may already be gone; the file is restored regardless
                pass
            print()
            print("restored %s" % conf_path)

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
