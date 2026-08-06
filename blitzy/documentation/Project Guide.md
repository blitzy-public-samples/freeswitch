# Blitzy Project Guide

**Project:** FreeSWITCH `1.11.2-dev` — `mod_xml_curl` BadgerFish JSON decoding + `mod_h323`/`mod_opal` test harnesses
**Branch:** `blitzy-736674cf-ebe1-40c1-b224-21affe971d9d` · **HEAD:** `906560dfa3` · **Upstream base:** `0a54a48f37`

---

# 1. Executive Summary

## 1.1 Project Overview

This engagement restructures two long-neglected areas of the FreeSWITCH telephony platform. Stream 1 turns the `mod_xml_curl` HTTP provisioning callout from a hardcoded single-format decoder into a format-dispatched pluggable stage, adding a BadgerFish JSON decoder behind an opt-in `response-format` parameter with total fallback to the original XML path. Stream 2 turns `mod_h323` and `mod_opal` — the tree's own examples of untested code — into module-plus-test-harness modules, and closes the crash that made enabling both fatal.

## 1.2 Completion Status

```mermaid
%%{init: {"theme":"base","themeVariables":{"pie1":"#5B39F3","pie2":"#FFFFFF","pieStrokeColor":"#B23AF2","pieStrokeWidth":"2px","pieOuterStrokeColor":"#B23AF2","pieTitleTextSize":"16px","pieSectionTextSize":"14px"}}}%%
pie showData title Completion Status — 86.3% Complete
    "Completed (AI)" : 386
    "Remaining" : 61.5
```

| Metric | Value |
|---|---|
| **Total Hours** | **447.5** |
| **Completed Hours (AI + Manual)** | **386** (386 AI-autonomous + 0 manual) |
| **Remaining Hours** | **61.5** |
| **Percent Complete** | **86.3%** |

`386 / (386 + 61.5) × 100 = 86.257% → 86.3%`. Legend: ▉ Completed = Dark Blue `#5B39F3` · ▁ Remaining = White `#FFFFFF`

> The denominator spans AAP deliverables *and* the path to production. All 35 AAP inventory items are Completed. Of the 61.5 h outstanding, 17 h is cross-platform verification no Linux container can perform; 44.5 h is review, sign-off and deployment.

## 1.3 Key Accomplishments

- ✅ **All 33 planned AAP paths delivered** — 78 files, +25,244 lines, 27 commits.
- ✅ **BadgerFish JSON decoding is live and opt-in** — 18 helpers, zero new includes, no new dependency.
- ✅ **Backward compatibility and total fallback proven at runtime**, not asserted.
- ✅ **Degradation is machine-observable** — a custom event pairs 1:1 with the existing warning.
- ✅ **18 fixture pairs serialise byte-identically** — double the required corpus, plus a standalone validator.
- ✅ **Three new suites: 72 cases, 1,109 assertion invocations** — modules declaring `TESTS` rose 12 → 15.
- ✅ **The endpoint co-load crash is closed** — a refusal guard replaces a deterministic SIGSEGV.
- ✅ **Every gate green** — `make check` exit 0 at 47/47 and 251/251, zero in-scope warnings, frozen paths 0-diff.

## 1.4 Critical Unresolved Issues

| Issue | Impact | Owner | ETA |
|---|---|---|---|
| The two endpoint co-load guards await ratification, and their extent is wider than the authorised carve-out | The AAP froze `mod_h323.cpp` and `mod_opal.cpp` at zero production edits. Two release lines per module sit outside the authorised top-of-load region, and three compiler-warning corrections per file sit below the guard. Narrowing the guard reintroduces both a post-unload crash and a documented TOCTOU, so ratification is the recommended disposition (§5.2 D1) | Endpoint maintainer | 3 h |
| Out-of-AAP core change `src/switch_event.c` (+22, dispatch-thread clamp) awaits ratification | Outside the 33-path scope but load-bearing: without it the test estate cannot run at all on a host reporting more than 64 CPUs (§5.2 D7) | Core maintainer | 4 h |
| Out-of-AAP rewrite of `src/mod/endpoints/mod_sofia/test/test_sofia_funcs.sh` awaits ratification | The shipped wrapper reported success while running nothing; it now genuinely reports PASSED (2/2) (§5.2 D7) | Sofia maintainer | 3 h |
| A per-installation SignalWire adoption UUID reached two committed log captures under `blitzy/documentation/` and has been rotated | The disclosed value now identifies no instance, and a captured HTTP 404 shows it never adopted one. The plaintext remains in earlier tree objects because rewriting this branch alone would not unpublish it. Residual action is one account-side confirmation | Repository / account owner | 0.5 h |
| Windows/MSVC compilation of the new translator is unverified | Project files are frozen by design, so a break would surface only in CI. Statically mitigated: MSVC-safe C, clean under `-Wdeclaration-after-statement -Wall -Wextra` | Windows build owner | 6 h |
| The `ci.sh` unit-test arm has never run on the target CI image | Both guard branches and the toolkit refusal are asserted locally, so this is a confirmation on the real image rather than unexercised logic | CI owner | 3 h |
| The provisioning gateway does not yet emit the BadgerFish contract | The JSON path stays dormant until the backend conforms. Fallback is total, so non-conformance degrades to XML with a warning rather than failing a lookup | Backend team | 5 h |

## 1.5 Access Issues

**No access issues identified.** Every build, test and runtime operation completed without credentials or network access.

| System/Resource | Type of Access | Issue Description | Resolution Status | Owner |
|---|---|---|---|---|
| Git repository | Read/write | None — 27 commits landed, every one authored and committed as `Blitzy Agent <agent@blitzy.com>` | ✅ No issue | — |
| All 16 `pkg-config` dependencies | Local toolchain | None — every one resolves with no `PKG_CONFIG_PATH` set | ✅ No issue | — |
| H.323 (PTLib 2.10.9 + H323Plus) and OPAL 3.12.10 toolkits | Local libraries | Both installed here, so both suites really compile and run. No distribution ships the OPAL development package, so reproducibility depends on the pinned provisioner being adopted | ✅ No issue locally / ⚠ image-build gap | CI owner |
| ESL / HTTP test credentials | Service auth | None — repository defaults `ClueCon` and `freeswitch`/`works`; no secrets required | ✅ No issue | — |
| Target CI image | CI execution | Not reachable from this environment; the unit-test arm is unverified against it | ⚠ Deferred to task H-15 | CI owner |
| Windows MSBuild / macOS toolchain | Cross-platform build | Neither platform is available here | ⚠ Deferred to tasks H-13, M-1 | Platform owners |

## 1.6 Recommended Next Steps

1. **[High]** Ratify the three disclosed out-of-AAP changes — the endpoint co-load guards and their widened carve-out, the dispatch-thread clamp, and the sofia wrapper rewrite (**11 h**). The only decision gating merge.
2. **[High]** Review and merge the change set (**14 h**): the four seams, the 18 helpers, the suites and fixtures, then the shell deliverables and build wiring.
3. **[High]** Verify MSVC compilation against the frozen Windows project files (**6 h**).
4. **[High]** Run the `ci.sh` unit-test arm on the target CI image, and confirm the rotated adoption token adopted no Connector (**3.5 h**).
5. **[Medium]** Bring the gateway into conformance with the shipped validator, then harden deployment configuration (**11 h**).

# 2. Project Hours Breakdown

## 2.1 Completed Work Detail

| Component | Hours | Description |
|---|---|---|
| **STREAM 1 — `mod_xml_curl` BadgerFish JSON** | **146** | |
| Design analysis and external-convention research | 12 | Serializer/parser semantics, dispatch lifecycle, BadgerFish rules, Automake exit-code protocol |
| Configuration seam | 4 | `response_format` appended as a struct member (`mod_xml_curl.c:65`), parameter arm (`:1974`), pool population under a NULL guard |
| Request seam | 3 | Conditional `Accept: application/json` (`:1649`) via a non-destructive append helper |
| Probe seam | 3 | `CURLINFO_CONTENT_TYPE` (`:1763`) read beside the response code (`:1756`) and before handle cleanup (`:1767`) |
| Decode seam | 5 | Single dispatch (`:1780`) inside the untouched HTTP-200 gate, total fallback to the untouched `switch_xml_parse_file` (`:1794`) |
| BadgerFish translator core | 22 | Recursive visitor, envelope synthesis, object shape checker |
| Content-type classifier | 3 | Full boundary semantics including `charset` parameters and an absent or empty header |
| Bounded response reader and decode orchestration | 5 | Reuses the already-capped temporary file, inheriting the 1 MiB ceiling |
| Input-validation and resource-budget family | 14 | 8 helpers and 11 declared ceilings |
| Log-hygiene redaction and cookie-jar validation | 9 | URL redaction, token sanitising, jar path checks |
| `mod_xml_curl/Makefile.am` test wiring | 3 | `noinst_PROGRAMS`, `TESTS`, flag and link parity, build-dependency closure |
| `conf/vanilla` operator documentation | 2 | Commented sample plus prose on ceilings, the `file:` boundary and the fallback contract |
| FST suite, initial | 26 | 36 cases as first delivered |
| `test/conf/freeswitch.xml` bootstrap root | 1 | Deliberately omits `xml_curl.conf` so the parse-failure branch is deterministic |
| First 9 paired parity fixtures (18 files) | 8 | Authored to serializer identity under all 8 fixture constraints |
| Fallback observability — `xml_curl::json_fallback` event | 4 | `Binding`, `Fallback-Reason`, redacted `Gateway`; 1:1 pairing with the warning verified live |
| 9 further paired parity fixtures (18 files) | 5 | Corpus 9 → 18 pairs; the original 9 left 0-diff |
| BadgerFish conformance validator and 5 counter-examples | 9 | `test/tools/validate_badgerfish.py`, 1,734 lines, mirroring the decoder function by function |
| Operator runbook `README.response-format.md` | 3 | 947 lines: fallback contract, both reason tokens, 11 ceilings, `file:` boundary, TLS, cookie jar, co-load prohibition, profile coverage, gateway contract |
| `response-format` documentation propagation | 1 | `conf/curl`, `conf/insideout`, `conf/testing` — block byte-identical to `conf/vanilla` |
| FST suite extension | 4 | 36 → 51 cases; 6,432 lines, 805 assertion invocations |
| **STREAM 2 — endpoint test harnesses** | **85** | |
| Design analysis: endpoint testability and symbol linkage | 8 | Static-versus-external analysis, duplicate-symbol contingency |
| `mod_h323/Makefile.am` test wiring | 4 | Flag parity by reference, macOS framework arm |
| FST suite `test_mod_h323.cpp` | 28 | 10 cases, 3,646 lines, 197 assertion invocations |
| `mod_h323/test/conf_h323/freeswitch.xml` fixture root | 1 | Deliberately omits `h323.conf` |
| `mod_opal/Makefile.am` test wiring | 4 | Convenience library, `pkg-config` parity, macOS arm |
| FST suite `test_mod_opal.cpp` | 28 | 11 cases, 3,800 lines, 107 assertion invocations |
| `mod_opal/test/conf_opal/freeswitch.xml` fixture root | 1 | Deliberately omits `opal.conf` |
| Co-load determination | 5 | Debugger backtraces in both load orders, linkage proof, static-initialisation probe, decision record |
| Co-load refusal guards in both load paths | 6 | Two-stage guard per module whose reservation deliberately survives an unload; three covering cases per suite |
| **CROSS-STREAM** | **47** | |
| Three `test/.gitignore` rule sets | 2 | Build-residue exclusion |
| `ci.sh` fail-closed enablement and capability probes | 11 | Compile-and-link probe, behavioural CVE-2013-1864 check, postcondition assertions |
| `ci.sh --guard-self-test` | 7 | Five arms including both guards positive and a stub-SDK verdict matrix |
| CVE verdict refinement and structured probe report | 5 | Three-way *vulnerable* / *no parser present* / *undecidable* decision over a machine-readable status channel |
| `build/provision_endpoint_toolkits.sh` | 16 | 2,766 lines: commit-pinned fetches, clean-export builds, `--dry-run`, `--uninstall-check`, sourced CVE gate, 8 pinned patches |
| Hermetic vulnerable-toolkit refusal proof | 6 | `--self-test` mode with a shim SDK, compiler wrapper, refusal and control arms |
| **QUALITY GATES AND AUTONOMOUS VALIDATION** | **108** | |
| Environment and dependency provisioning | 7 | 16 `pkg-config` dependencies plus both endpoint toolkits |
| Full-tree compilation, install, clean rebuilds, flag proof | 8 | Verbose builds proving `-Werror`, `-Wdeclaration-after-statement` and the nesting limit reach in-scope lines |
| Serializer-identity acceptance for all 18 pairs | 3 | Byte-identical output in both directions |
| Sanitizer and memory validation | 8 | Genuine sanitizer builds; every remaining allocation traced to third-party loader-time work |
| Static analysis and triage | 6 | Clang analyser over the translator; shell linters at baseline parity |
| Test collection and full-estate execution | 8 | Reproduced across sessions with identical results |
| Runtime validation | 11 | Live instances, the five-case decode matrix, the HTTP authentication boundary |
| Browser verification of the HTTP API surface | 4 | 9/9 checks, screenshots and recordings captured |
| Cross-cutting engineering across the test estate | 15 | The dispatch-thread clamp, the installed-library verification methodology, the sofia wrapper rewrite, and four supporting investigations |
| Security review | 9 | Secrets, TLS posture, redaction, cookie jar, advisories, CI driver |
| AAP compliance verification and commit-hygiene audit | 6 | 33/33 paths, invariants 0-diff, attribution on every commit |
| Committed record hygiene and documentation capture | 4 | Credential redaction across both log captures, probe licence and usage, transcript capture |
| Negative-control batteries for the new contracts | 4 | Structured-report shapes against a controlled probe; clean-export reproducibility |
| Module enablement and test re-collection | 3 | Driven through the real CI contract rather than by hand |
| Full-estate re-execution and allocation attribution | 4 | 251/251 cases; every remaining sanitizer frame traced to third-party plugin registries |
| Test-system engineering and final verification | 6 | The working-directory dependency that stopped `make check` completing, the endpoint suites' loader-time allocation containment, the six inherited endpoint diagnostics, and the sticky-reservation behaviour with one new case per suite |
| Assessment of the delivered state | 2 | Every figure in this guide measured first-hand against the branch |
| **TOTAL COMPLETED** | **386** | **Matches Completed Hours in §1.2** |

## 2.2 Remaining Work Detail

| Category | Hours | Priority |
|---|---|---|
| [P2P] Code review and merge of the change set — 78 code, test, fixture and configuration files | 14 | High |
| [P2P] Maintainer sign-off of the three disclosed out-of-AAP reconciliations | 11 | High |
| [AAP I7/S10] Windows/MSVC compilation verification of the JSON translator | 6 | High |
| [AAP 0.8.6] CI unit-test arm execution on the target image | 3 | High |
| [P2P] Confirm the rotated adoption token adopted no Connector | 0.5 | High |
| [AAP D6] macOS link verification of both endpoint test targets | 4 | Medium |
| [AAP S11] Static-analysis arm verification from `build/modules.conf.most` | 3 | Medium |
| [P2P] Provisioning-gateway conformance to the BadgerFish contract | 5 | Medium |
| [P2P] Production deployment configuration — format, TLS posture, cookie jar, credentials | 6 | Medium |
| [P2P] Adopt the toolkit provisioner in the CI and production image builds | 1 | Medium |
| [P2P] Wire the fallback signal into operator alerting | 1 | Medium |
| [AAP I9] Ratify the `response-format` documentation propagation | 1 | Medium |
| [P2P] Soak and regression baseline under representative provisioning load | 6 | Low |
| **TOTAL REMAINING** | **61.5** | **High 34.5 · Medium 21 · Low 6** |

**Prioritized human task list** — the 13 categories above decompose into **30 tasks totalling exactly 61.5 h**, and each task's hours roll up precisely into its parent category.

| ID | Task | Hours | Priority |
|---|---|---|---|
| H-1 | Confirm no Connector was adopted with the disclosed adoption UUID; decide whether the blob is purged across the affected branches | 0.5 | High |
| H-2 | Ratify the dispatch-thread clamp in `src/switch_event.c`: reproduce a host reporting more than 64 CPUs, review the clamp and its rationale, decide keep / upstream / revert | 4.0 | High |
| H-3 | Ratify the two endpoint co-load guards **and the widened carve-out** — the two pre-construction release lines outside the top-of-load region and the three warning fixes per file | 3.0 | High |
| H-4 | Ratify the sofia test-wrapper repair: confirm the shipped wrapper was a false green, accept the rewrite or split it out | 3.0 | High |
| H-5 | Decide the disposition of all three reconciliations plus the fourth build file and the two ignore-rule additions, and record it | 1.0 | High |
| H-6 | Review the Stream 1 production diff: four seams, the struct/parameter/pool arms, the fallback event path | 2.5 | High |
| H-7 | Review the 18 file-static `xml_curl_json_*` helpers: recursion, envelope synthesis, ceilings, validation, redaction | 3.0 | High |
| H-8 | Review the three FST suites (72 cases), the 18 parity pairs and the 5 counter-examples for coverage adequacy | 2.5 | High |
| H-9 | Review `ci.sh`: the guard self-test, the three-way CVE verdict and the structured probe report | 2.0 | High |
| H-10 | Review `build/provision_endpoint_toolkits.sh` including the refusal proof and the 8 pinned patches | 2.0 | High |
| H-11 | Review the contract validator and the four `Makefile.am` diffs | 1.0 | High |
| H-12 | Reconcile against the 33-path scope, confirm the frozen paths are 0-diff, merge | 1.0 | High |
| H-13 | Build the `mod_xml_curl` Windows project under MSBuild; confirm no declaration-placement diagnostics | 4.0 | High |
| H-14 | Correct any MSBuild diagnostic without touching the frozen project files; re-run the Linux suite | 2.0 | High |
| H-15 | Run the `ci.sh` unit-test arm on the target CI image; confirm the collected set and that every test passes | 3.0 | High |
| M-1 | Cross-link both endpoint test targets on macOS; validate the two framework arms | 3.0 | Medium |
| M-2 | Correct any macOS-only link failure inside those arms only | 1.0 | Medium |
| M-3 | Run the static-analysis arm end to end from `build/modules.conf.most` | 2.0 | Medium |
| M-4 | Triage any new analyser finding against the recorded false-positive analysis | 1.0 | Medium |
| M-5 | Ratify the documentation propagation: accept the four-carrier state, or extend it | 1.0 | Medium |
| M-6 | Implement BadgerFish emission in the provisioning gateway for the three bound sections | 3.0 | Medium |
| M-7 | Validate the gateway's output with the contract validator across all 18 pairs and the `Accept` negotiation | 1.5 | Medium |
| M-8 | Add a gateway contract test: one top-level key equal to the section, string-only values, no envelope attributes | 0.5 | Medium |
| M-9 | Enable `mod_xml_curl` in the deployment module list and set `response-format=json` per binding | 1.5 | Medium |
| M-10 | Harden TLS: `enable-cacert-check`, `enable-ssl-verifyhost`, `ssl-cacert-file` | 2.5 | Medium |
| M-11 | Provision gateway credentials and relocate the cookie jar to a non-world-writable path | 2.0 | Medium |
| M-12 | Subscribe operator alerting to `xml_curl::json_fallback` or its warning so degradation cannot go unnoticed | 1.0 | Medium |
| M-13 | Adopt the toolkit provisioner in the CI and production image builds, and add libpcap's development package so the collected count is host-independent | 1.0 | Medium |
| L-1 | Establish a soak and regression baseline under representative lookup load | 4.0 | Low |
| L-2 | Compare JSON-path against XML-path latency and memory under sustained load | 2.0 | Low |
| | **TOTAL** | **61.5** | |

## 2.3 Hours Methodology

Every AAP requirement was inventoried, mapped to concrete evidence — repository paths, line numbers, test-case names, commits — and classified. **All 35 inventory items are Completed; 0 partially completed; 0 not started.** Completed hours derive from implementation volume and complexity plus the verification actually performed; remaining hours cover the AAP items no Linux container can verify (17 h) plus standard path-to-production activities (44.5 h).

Confidence: **High 28.5 h · Medium 27 h · Low 6 h**. The one low-confidence item is the soak baseline, whose shape depends on production traffic nobody here can observe.

- Completed Hours = **386** (146 Stream 1 + 85 Stream 2 + 47 cross-stream + 108 quality gates and validation)
- Remaining Hours = **61.5** (High 34.5 + Medium 21 + Low 6)
- Total Project Hours = 386 + 61.5 = **447.5**
- Completion = 386 / 447.5 × 100 = **86.3%**

---

# 3. Test Results

The whole collected estate was re-executed for this assessment at HEAD `906560dfa3`. Full `make check` from the repository root **exits 0**, with the Automake summaries adding up to `# TOTAL: 47 / # PASS: 47 / # SKIP: 0 / # FAIL: 0 / # XPASS: 0 / # ERROR: 0`; the string `exit status: 77` appears zero times, and all 47 freshly written `.trs` files carry `:global-test-result: PASS`. Running every collected binary individually from its own directory reproduces 47 × exit 0 and totals 251 FCTX cases.

| Area / Category | Framework | Tests | Passed | Failed | Coverage | What This Proves |
|---|---|---|---|---|---|---|
| `mod_xml_curl` BadgerFish decode path (new) | FST / FCTX 1.6.1 | 51 | 51 | 0 | 805 assertion invocations; all 18 parity pairs, both fallback edges, the event, the classifier boundaries | A JSON provisioning response is decoded into the same document tree its XML twin produces, and every failure edge degrades to XML instead of failing the lookup |
| `mod_h323` endpoint harness (new) | FST / FCTX 1.6.1 | 10 | 10 | 0 | 197 invocations; 100% of the module's entry points | Load and shutdown both succeed and are asserted, gatekeeper and codec-preference parsing behave as documented, and a co-load attempt is refused rather than fatal |
| `mod_opal` endpoint harness (new) | FST / FCTX 1.6.1 | 11 | 11 | 0 | 107 invocations; 100% of the module's entry points | Dual-endpoint construction, the default signalling port and injected settings are observable through public interfaces, and the same co-load refusal holds |
| Pre-existing estate (regression) | FST / FCTX 1.6.1 | 179 | 179 | 0 | 44 binaries — 17 other module suites and 27 core binaries; `tests/unit/**` is 0-diff | Adding three suites regressed nothing: the 15 `TESTS`-declaring modules coexist and no public API, header or parser behaviour moved |
| Gateway contract conformance | `validate_badgerfish.py` (python3, stdlib only) | 23 | 23 | 0 | 18 conformant fixtures plus 5 counter-examples | A backend team can prove its own JSON conforms before deploying: every conformant document is accepted and each deliberately malformed one is rejected by name |
| Capability-guard and provisioning self-tests | `ci.sh --guard-self-test`, provisioner `--self-test` | 11 | 11 | 0 | 5 guard arms + 6 provisioner assertions | Both CI capability guards are asserted in both directions, the unconditional arm keeps its exactly-one-active-line postcondition, and provisioning refuses a vulnerable toolkit by name — all without mutating the toolchain or any tracked file |
| Memory safety and shell static analysis | AddressSanitizer / LeakSanitizer, `shellcheck -x`, `shfmt` | 5 | 5 | 0 | 3 suites under a sanitizer build; 2 shell deliverables | Under a genuine sanitizer build all three new suites exit 0 — the decoder suite with no sanitizer output at all and no suppression used, the two endpoint suites with third-party loader-time plugin-registry allocations contained by a suppression scoped to one class template. The provisioner reports zero linter findings and `ci.sh` reports exactly the finding set its baseline reports |
| **TOTAL (collected binaries / cases)** | | **47 / 251** | **47 / 251** | **0 / 0** | **100% pass** | 0 failures, 0 skips, 0 exit-77 outcomes, 0 segfaults |

## Not Covered

These capabilities were delivered but are not exercised by any automated test, and a human should decide what to do about each before release:

- **The provisioning script's install path** — `fetch_component`, `build_and_install_component`, `provision_missing` and `verify_after_install` have never run, because this environment has no outbound network and the toolkits are already present. Everything ahead of that path is exercised: the CVE gate, capability detection, the plan report, the tool guards, `--dry-run`, `--uninstall-check` and `--self-test`. Run it once on a networked provisioning host or inside an image build.
- **The Windows and macOS build arms** — no test in the tree exercises either. MSVC compilation of the translator and the two macOS framework link arms are unverified (tasks H-13, H-14, M-1, M-2).
- **The operator runbook's prose** — nothing in this tree asserts documentation. Its factual claims were instead checked mechanically against the code they cite, and every command it publishes was executed; what remains unexercised is the usefulness of the wording, which is a human judgement.
- **Runtime load of the `conf/curl`, `conf/insideout` and `conf/testing` carriers** — only `conf/vanilla` was loaded by a live instance. The gap is narrow by construction: the three additions are byte-identical to vanilla's and comment-only, so with comments stripped each file is byte-identical to its base version.
- **Two defensive branches in the event path** — a subclass-reservation status other than success or already-in-use, and a failure to create the event. Both are unreachable without fault injection into a frozen core file, and neither can alter behaviour: one only logs, the other returns before any header is added.
- **The `WIN32` arm of `mod_h323`'s load function** — pre-existing, unmodified and unreachable on Linux; the new guard sits above it and returns first.
- **The clang static analyser and valgrind** — neither tool is present in this environment, so their results predate the current translator. The analyser arm is scheduled as tasks M-3 and M-4.

One collection nuance worth knowing: the count reaches 47 only on a host carrying libpcap's development package, because `configure.ac` gates `tests/unit/switch_rtp_pcap` on `pcap-config`. Without it the count is 46, and the CI driver reports that capability explicitly rather than letting the number move silently (task M-13).

---

# 4. Runtime Validation & UI Verification

Legend: ✅ Operational · ⚠ Partial · ❌ Failing

- ✅ **Build, install and collection** — `make -j$(nproc)` exit 0; `make install` exit 0 with 53 module `.so` in place; `make -s print_tests` collects 47 unique tests; full `make check` exit 0 at 47/47. The tree-wide warning census is 32 lines — 27 from third-party PTLib headers, 4 from out-of-scope `mod_lua.cpp`, 1 tooling notice — and **none names an in-scope file**.
- ✅ **Core runtime** — an instance started from the install prefix reaches `FreeSWITCH (Version 1.11.2-dev git 906560d …) is ready`, `status` reports UP, and `SIGTERM` on the captured pid exits cleanly in 6 seconds with zero segfault or core-dump markers in the log.
- ✅ **HTTP API boundary** — `GET /api/status` returns **200** with Basic auth and **401** without it. The same surface was driven through a browser session earlier in the engagement: 9 of 9 checks passed with zero JavaScript errors and no unexpected console or network failures.
- ✅ **JSON provisioning happy path** — a loopback gateway answering `application/json` with a conformant BadgerFish document provisioned a directory user end to end, and its ACL went live (`acl 192.168.42.42 lan` → true, `acl 192.168.42.7 lan` → false).
- ✅ **Backward compatibility** — with `response-format` absent the classic XML path answered, `Accept: application/json` was never sent, and **zero** fallback warnings and **zero** events were emitted across the run.
- ✅ **Total fallback, both edges** — a malformed body and a conformant body served as `text/xml` each produced the documented warning and **still resolved the lookup**. Warnings and `xml_curl::json_fallback` events paired 1:1 across the five-case matrix (per case 0:0, 2:2, 2:2, 0:0, 1:1).
- ✅ **Log hygiene** — fallback diagnostics render the gateway as `http://127.0.0.1:PORT/[redacted]`; the URL path never appears anywhere in the log, while host and port are retained for diagnosis.
- ✅ **Endpoint modules, individually** — `mod_opal` loads and `show endpoint` lists `endpoint,opal,mod_opal`; `mod_h323` loads in its own instance and lists `endpoint,h323,mod_h323`.
- ✅ **Endpoint co-load is refused, not fatal** — verified in both load orders and, decisively, *across an unload*: after `load mod_h323` then `unload mod_h323`, `global_getvar _fs_ptlib_endpoint_reservation` still answered `mod_h323`, the following `load mod_opal` was refused with `mod_opal.cpp:173 Refusing to load mod_opal: the PTLib runtime in this process is already reserved by [mod_h323] …`, and the instance stayed ready with no crash markers. Load the two modules in separate instances.
- ✅ **Capability guards and toolkit provisioning** — `./ci.sh --guard-self-test` exits 0 with all 5 arms passing and leaves the working tree byte-identical; the provisioner's `--dry-run`, `--uninstall-check`, `--help` and `--self-test` modes all exit 0, the last at 6/6 assertions with the installed toolkit digests unchanged.

**Never exercised at runtime.** The Windows and macOS builds — no toolchain for either exists in this environment, and the project files are frozen by design. The provisioning script's actual fetch-build-install path — no outbound network, and the toolkits are already installed. The `conf/curl`, `conf/insideout` and `conf/testing` carriers — never loaded by a live instance, only verified statically. Beyond the XML-RPC HTTP surface above the project has no user interface: neither stream creates a screen, a component or a client-side asset, so nothing else was available to drive.

---

# 5. Compliance & Quality Review

## 5.1 Compliance Matrix

| AAP Deliverable / Benchmark | Evidence | Status | Progress |
|---|---|---|---|
| **Scope** — all 33 planned paths delivered | Every path exists on disk; the delivered set is 78 code, test, fixture and configuration files — 61 added, 17 modified | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Four seams at their verified coordinates** | `response_format` appended to `struct xml_binding` (`mod_xml_curl.c:65`), parameter arm 19th at `:1974`, `Accept` header at `:1649`, content-type probe at `:1763` between the response-code read `:1756` and cleanup `:1767`, single dispatch at `:1780` | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Total fallback through untouched shared code** | The pre-existing `switch_xml_parse_file` at `:1794`, the HTTP-200 gate at `:1779` and the non-200 branch are textually intact; proven live on both failure edges | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Strict backward compatibility** | With the parameter absent the struct member stays NULL, no header is appended, no decode is attempted, and zero events fire — observed live, not inferred | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Immutable behaviours preserved** | All 18 legacy binding parameters present in their original order with the new arm appended 19th; the 1 MiB response ceiling, the permissive TLS defaults, the `file:` shortcut and the temporary-file contract all byte-for-byte | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Translator discipline** | Exactly two `#include` lines (`:33`, `:34`); 18 file-static `xml_curl_json_*` helpers; duplicating builder variants only; the cJSON tree released on every exit path | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Fixture parity and corpus** | 18 XML/JSON pairs serialise byte-identically through the same serializer call; the original 9 are 0-diff; all 8 fixture-authoring constraints hold | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Harness architecture and registration** | `TESTS = $(noinst_PROGRAMS)` in all three module `Makefile.am` files (modules declaring `TESTS` 12 → 15); every suite uses `FST_CORE_BEGIN` with `FST_SUITE_BEGIN`; module-local fixture roots that deliberately omit their own configuration section; configuration injected through the public binding API | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Endpoint production sources** | Both headers 0-diff. Both `.cpp` files carry the sanctioned co-load carve-out (+101/−3 and +100/−3), whose extent exceeds what was authorised — see §5.2 D1 | ⚠ Partial | ▓▓▓▓▓▓▓░░░ 70% — awaiting ratification |
| **Default-build contract and frozen API/ABI** | `build/modules.conf.in` 0-diff with all three modules still commented at L69/L71/L132; `src/include/**`, `configure.ac`, `build/modules.conf.most`, the three build-system Makefiles, `build/buildopal.sh`, `debian/**`, `freeswitch.spec`, `.github/**` and every Windows project file all 0-diff; nothing de-staticised | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Capability-guarded CI enablement** | `mod_xml_curl` enabled unconditionally; both endpoint arms gated on a compile-and-link probe plus a three-way CVE-2013-1864 verdict, fail-closed, with an exactly-one-active-line postcondition and a negative assertion; all five self-test arms pass | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |
| **Quality gates** | `make check` exit 0 at 47/47 and 251/251; zero warnings from any in-scope file; sanitizer-clean; shell linters at baseline parity; zero placeholders, TODOs or stubs; zero credential patterns across every added line; every commit authored and committed as `Blitzy Agent <agent@blitzy.com>` | ✅ Pass | ▓▓▓▓▓▓▓▓▓▓ 100% |

## 5.2 AAP & Rule Divergences and Gaps

No user-specified rules exist for this project — the rules document reports none — so no rule divergence is possible, and the tree's own conventions applied in their place and were verified. Every divergence below is a departure from the **Agent Action Plan**. Five were directed by an explicit human decision during delivery and are marked *Sanctioned*; one is an out-of-AAP disclosure; one is an engineering necessity that no directive named; one is an interpretation the plan's own arithmetic made unavoidable.

| # | What the AAP Required | What Was Delivered Instead | Why It Diverged | Impact | Remediation |
|---|---|---|---|---|---|
| **D1** | R2-1, §0.2.2, §0.8.1 — zero production edits to `mod_h323.cpp` and `mod_opal.cpp` | A two-stage co-load refusal guard in each load path: `mod_h323.cpp` +101/−3, `mod_opal.cpp` +100/−3 | *Sanctioned* — a real co-load crash was proven, and a recorded human decision selected an in-module guard. The delivered extent is wider than the authorised region | Enabling both endpoints can no longer kill the process; the departure is in scope discipline, not correctness | Ratify the widened carve-out — task H-3, 3 h. **Release-blocking; also in §1.4** |
| **D2** | §0.5.4 "Documentation: None" and I9 "documented in exactly one shipped file" | A 947-line operator runbook plus `response-format` documented identically in all four shipped carriers | *Sanctioned* — explicit human direction; the four-carrier enumeration was established on disk, excluding the single-carrier branch | None functional: the carrier additions are comment-only | Ratify the propagation — task M-5, 1 h |
| **D3** | §0.3.4 and §0.10.2 — one commented sample line and one warning are the whole operator-visible surface, and three insertions the complete set of production edits | A `SWITCH_EVENT_CUSTOM` subclass `xml_curl::json_fallback` firing beside the unchanged warning, one further append-only struct member, one added helper parameter | *Sanctioned* — human direction to make degradation alertable without log scraping | Degradation is observable over the event socket; the registered callback signature is unchanged | None — the event is requested work |
| **D4** | §0.2 — the change set comprises 33 files | 78 files: the corpus doubled to 18 pairs, plus 5 counter-examples, a 1,734-line validator, a 2,766-line pinned provisioner with 8 patches, and the runbook | *Sanctioned* — five separate human objectives, each superseding the frozen inventory | A materially larger review surface, which is why the review estimate grew from 10 h to 14 h | Covered by tasks H-6 … H-12 |
| **D5** | §0.5.4 and §0.3.1 — the CI ripple is the unit-test arm's module list, shown as two one-line edits | `ci.sh` is 2,014 lines (+1,771/−11), carrying the guard implementation, a structured verdict contract, a scratch registry and a five-arm self-test | *Sanctioned* — a capability guard that cannot be asserted in both directions is untestable, and C6 requires capability-gated endpoint arms | Default build unaffected; the pre-existing arms are byte-identical | Review — task H-9, 2 h |
| **D6** | §0.5.4 — build files changed are "exactly three" | A fourth, `src/mod/codecs/mod_openh264/Makefile.am` (+14/−1), plus that module's test source (+43/−6) and two `.gitignore` files (+10, +7) | **Not named by any directive.** That module's test target defined neither `SWITCH_TEST_BASE_DIR_*` macro, so under the Automake driver it deadlocked at start-up and stopped `make check` completing for the whole tree | `make check` terminates and passes; no production translation unit of that module was touched | Confirm the fourth build file and the ignore rules — inside tasks H-5 and H-12 |
| **D7** | §0.2.2 — `src/switch_event.c` and the sofia test wrapper are out of scope | `src/switch_event.c` +22/−0 (a dispatch-thread clamp) and `test_sofia_funcs.sh` +197/−11 | Disclosed out-of-AAP work: the test estate cannot run at all on a host reporting more than 64 CPUs, and the shipped wrapper reported success while running nothing | Both are load-bearing for the test estate; both carry in-file rationale | Ratify or split to a separate upstream PR — tasks H-2 (4 h) and H-4 (3 h). **Release-blocking; also in §1.4** |
| **D8** | A fixture at "the `CJSON_NESTING_LIMIT=64` boundary minus one" | A fixture at element depth 31 | Four nesting numbers exist and the tightest governs: the module enforces 32, `configure.ac` compiles 64, and the vendored 1000 is only a fallback. A depth-63 document is refused before cJSON sees it | The boundary that actually governs is the one tested | None — all four numbers are documented in the runbook |

**D1 — endpoint production edits.** The plan was categorical: both endpoint sources were to stay 0-diff, because the harnesses observe rather than alter. Delivery proved co-loading them deterministically fatal — they resolve two different PTLib runtimes (`libpt.so.2.10.9` versus `libpt.so.2.12-beta10`, confirmed by `ldd`) whose `PProcess` singletons collide, and module code demonstrably executes before the fault. A human decision authorised a guard "isolated at the top of `switch_module_load`". What shipped is wider: two release lines per module on the pre-construction failure edges (`mod_h323.cpp:241-246`, `:255`; `mod_opal.cpp:194`, `:205`), plus three diagnostic corrections per file far below. Both shutdown functions and both headers stay 0-diff. Narrowing to one stage reintroduces a post-unload crash and a documented race, so ratification is the recommendation.

**D2 — documentation ripple.** The plan bounded documentation to a single commented sample line in the vanilla profile, reasoning that the tree already tolerates that drift for `response-max-bytes`. A human then asked for an operator runbook and for the parameter to be documented in every shipped profile carrying an `xml_curl.conf.xml`. Four carriers exist — `conf/vanilla`, `conf/curl`, `conf/insideout`, `conf/testing` — so the single-carrier branch was factually excluded and the block was propagated to the other three, extracted programmatically so content identity is by construction. Every added line in all four is a comment: with comments stripped each file is byte-identical to its base version, so no default and no active parameter moved.

**D3 — the fallback event.** The plan fixed the operator-visible surface at one commented sample line plus one new warning, and declared three insertions the complete set of production edits. A human required the degradation to be machine-observable, because a decode regression that only warns is a regression nobody notices. `mod_xml_curl.c` now reserves a custom subclass at load, releases it at shutdown behind an ownership latch, and fires one event beside the unchanged warning at the single fallback site, carrying the binding name, a two-valued reason token and a redacted gateway. The registered callback signature is untouched, so the binding API contract holds. The event fires only from the fallback edge: an XML-default run emits none, observed rather than assumed.

**D4 — inventory growth.** The plan enumerated 33 files and asserted the count was specification rather than convenience. Five human objectives superseded it: double the parity corpus, ship a standalone conformance validator with deliberately malformed counter-examples, author an operator runbook, propagate the configuration documentation, and replace the dead vendor toolkit script with a pinned, CVE-gated provisioner. The result is 78 files rather than 33 — 61 added, 17 modified, +25,244 lines. Nothing in the original 33 was dropped, and the nine original fixture pairs are byte-for-byte unchanged. The consequence a reader feels is review effort, which is why that estimate grew from 10 h to 14 h across seven separately scoped tasks.

**D5 — the CI driver.** The plan showed the CI ripple as two `pkg-config … && sed` one-liners appended to the unit-test arm's module list. What that shape cannot do is prove itself: a guard whose refusal branch never runs is indistinguishable from a guard that always enables. `ci.sh` therefore now carries the capability probe and the CVE-2013-1864 verdict as separately callable functions publishing a structured three-way result, a validated scratch registry, and a five-arm `--guard-self-test` driving both guards' negative branches through environment isolation and both positive branches on the real host. The pre-existing arms, the configure invocation and the whole static-analysis arm are byte-identical, and `build/modules.conf.in` is untouched, so the default build is exactly what it was.

**D6 — the fourth build file.** No directive named `src/mod/codecs/mod_openh264`, and the plan fixed the build-file count at three. But that module's test target defined neither `SWITCH_TEST_BASE_DIR_FOR_CONF` nor `SWITCH_TEST_BASE_DIR_OVERRIDE`, so under the Automake driver its suite resolved its configuration root against the wrong directory, found none, and deadlocked at start-up with an empty log — which meant `make check` could never complete for the whole tree, and the tree's own acceptance command could not be evaluated. Both macros are now supplied, exactly as every other `TESTS`-declaring module supplies them, and that module's fixture paths were made absolute so its suite genuinely decodes. It appears on no freeze list; a maintainer should nonetheless confirm the addition.

**D7 — out-of-AAP core and wrapper changes.** Two files the plan placed out of scope carry disclosed changes. `src/switch_event.c` gained a 22-line clamp bounding the dispatch-thread count to the size of its fixed bookkeeping arrays: on any host reporting more than 64 CPUs those arrays overrun, shutdown hangs, and the test estate cannot be evaluated at all. `test_sofia_funcs.sh` was rewritten because the shipped wrapper exited 126 under a non-interactive shell, used `pushd` and assumed Python 2 — so a collected test reported success while running nothing. Both carry in-file rationale, and both are frozen at their committed state. They need a maintainer decision on disposition, in this PR or as a separate upstream change.

**D8 — the nesting-boundary fixture.** A directive asked for a fixture nested at "the `CJSON_NESTING_LIMIT=64` boundary minus one". That arithmetic is unimplementable here, because four different numbers govern nesting and the tightest one wins: the module enforces `XML_CURL_JSON_MAX_DEPTH 32` (`mod_xml_curl.c:189`), `configure.ac:313-314` compiles `-DCJSON_NESTING_LIMIT=64` into every translation unit, and the 1000 in `switch_cJSON.h:128-130` is only an `#ifndef` fallback that never applies. A depth-63 document is refused by the decoder before cJSON ever sees it, so it could never satisfy serializer identity. The fixture therefore sits at element depth 31 against the ceiling of 32 — the boundary that actually governs — and all four numbers are stated in the runbook so nobody re-derives them.

# 6. Risk Assessment

| Risk | Category | Severity | Probability | Mitigation | Status |
|---|---|---|---|---|---|
| **R1** — Windows/MSVC compilation of the new translator has never been attempted, so a Windows build break would surface only in CI | Technical | Medium | Low | The new C is written to MSVC constraints throughout — declarations at block top, no variable-length arrays, no designated initialisers — and `gcc -fsyntax-only -Wdeclaration-after-statement -Wall -Wextra` reports zero diagnostics. No Windows project file changed, so the frozen build description still names a buildable module. Tasks H-13/H-14, 6 h | ⚠ Open, statically mitigated |
| **R2** — The remaining cross-platform and static-analysis verification: macOS framework link arms authored from precedent and never compiled, the clang analyser arm not re-run, and the CI unit-test arm never executed on the target image | Technical | Medium | Medium | Both capability-guard branches and the toolkit refusal are already asserted on this host, so the CI run confirms rather than discovers. The macOS arms mirror the only existing endpoint-module test wiring in the tree. Tasks M-1 … M-4 and H-15, 10 h | ⚠ Open |
| **R3** — Co-loading the two endpoint modules is fatal at the third-party level, and the process-global claim that prevents it can be defeated by clearing it by hand | Operational | High | Low | The claim deliberately outlives an unload, because an unloaded module's PTLib runtime stays mapped; only its own holder may re-claim it, so single-module reload still works. The runbook carries an explicit do-not-clear warning, three cases per endpoint suite assert the refusal, and it was reproduced live in both load orders with the instance staying ready | ✅ Mitigated |
| **R4** — The provisioning transport keeps its permissive TLS defaults, so an unhardened deployment trusts any peer certificate | Security | High | Medium in production | Deliberate: changing a shipped default would itself be an out-of-scope behaviour change, and the posture was verified byte-for-byte against base. All three hardening parameters are documented with their parse and apply sites, and each was proven effective at runtime against self-signed peers. Task M-10, 2.5 h | ⚠ Open by design, deployment-gated |
| **R5** — Provisioning responses are untrusted input arriving on a new recursive parse path | Security | High | Low | 11 declared ceilings bound depth, width, node and value counts; XML-name, text and unicode-escape validation reject anything the serializer could not represent; the 1 MiB transport cap is inherited unchanged; the suite carries adversarial width and depth cases; and the decoder suite is sanitizer-clean with no suppression used | ✅ Mitigated |
| **R6** — The JSON path does nothing until the provisioning backend emits the BadgerFish contract, and a non-conforming backend is silent | Integration | High | High until the gateway is updated | Fallback is total and was proven live, so non-conformance degrades to XML with a warning and never fails a lookup — and that warning now has a machine-readable twin to alert on. A standalone validator ships with the corpus so the backend team can check its own output first. Tasks M-6 … M-8, 5 h | ⚠ Open |
| **R7** — Reproducibility outside this host: no distribution ships the OPAL development package, and the collected-test count depends on libpcap being present | Operational | Medium | High | A commit-pinned, CVE-gated provisioner replaces the dead vendor script and exits 0 on an already-provisioned host. Where a toolkit is absent the capability guard leaves the module disabled and its suite uncollected rather than failing the build. The CI driver reports the pcap capability explicitly instead of letting the count move silently. Task M-13, 1 h | ⚠ Open |
| **R8** — Residual lower-severity items: no soak or latency baseline for the JSON path, operator alerting not yet subscribed to the fallback signal, and the rotated per-installation adoption UUID still present in earlier tree objects | Mixed | Low | Medium | The plan sets no performance target; the signal site is single and its message stable, so subscribing is configuration rather than development; and the rotation is complete, so the disclosed value identifies no instance. Tasks H-1, M-12, L-1, L-2, 7.5 h | ⚠ Open |

---

# 7. Visual Project Status

## Project hours breakdown

```mermaid
%%{init: {"theme":"base","themeVariables":{"pie1":"#5B39F3","pie2":"#FFFFFF","pieStrokeColor":"#B23AF2","pieStrokeWidth":"2px","pieOuterStrokeColor":"#B23AF2","pieTitleTextSize":"16px","pieSectionTextSize":"14px"}}}%%
pie showData title Project Hours Breakdown (447.5 h total)
    "Completed Work" : 386
    "Remaining Work" : 61.5
```

## Remaining work by priority

```mermaid
%%{init: {"theme":"base","themeVariables":{"pie1":"#5B39F3","pie2":"#A8FDD9","pie3":"#FFFFFF","pieStrokeColor":"#B23AF2","pieStrokeWidth":"2px","pieTitleTextSize":"16px"}}}%%
pie showData title Remaining 61.5 h by Priority
    "High" : 34.5
    "Medium" : 21
    "Low" : 6
```

## Completed work by stream

```mermaid
%%{init: {"theme":"base","themeVariables":{"pie1":"#5B39F3","pie2":"#B23AF2","pie3":"#A8FDD9","pie4":"#FFFFFF","pieStrokeColor":"#B23AF2","pieStrokeWidth":"2px","pieTitleTextSize":"16px"}}}%%
pie showData title Completed 386 h by Stream
    "Quality Gates and Validation" : 108
    "Stream 1 mod_xml_curl JSON" : 146
    "Stream 2 Endpoint Harnesses" : 85
    "Cross-Stream CI and provisioning" : 47
```

## Remaining hours per category

```
AAP-scoped (17 h)
  Windows/MSVC verification        ████████████             6 h
  macOS link verification          ████████                 4 h
  CI unit-test arm on target image ██████                   3 h
  Static-analysis arm              ██████                   3 h
  Docs propagation ratification    ██                       1 h

Path-to-production (44.5 h)
  Code review and merge            ████████████████████████████  14 h
  Reconciliation sign-off          ██████████████████████        11 h
  Deployment configuration         ████████████                   6 h
  Soak / regression baseline       ████████████                   6 h
  Gateway BadgerFish conformance   ██████████                     5 h
  Provisioner adoption in images   ██                             1 h
  Fallback alerting subscription   ██                             1 h
  Adoption-token confirmation      █                            0.5 h
```

**Integrity check:** "Remaining Work" is **61.5** in every chart above, identical to the §1.2 metrics table and to the §2.2 total. "Completed Work" is **386**, identical to the §2.1 total. `386 + 61.5 = 447.5` = Total Project Hours in §1.2.

**Brand colours:** Completed `#5B39F3` · Remaining `#FFFFFF` · Headings and accents `#B23AF2` · Highlight `#A8FDD9`.

---

# 8. Summary & Recommendations

The engagement is **86.3% complete** — 386 of 447.5 hours. All 35 AAP inventory items are classified Completed, all 33 planned paths were delivered, and both streams landed with their invariants intact. Stream 1 turned a hardcoded decode step into a genuine strategy dispatch: the four seams sit at exactly the coordinates the plan specified, including the one that governs correctness — the content-type probe is read beside the response code and *before* the transfer handle is destroyed, avoiding the use-after-free a single-site implementation would have introduced. The translator materially exceeds the plan's minimum, adding 11 resource ceilings, an input-validation family and log redaction while still introducing zero new `#include` lines and no new dependency. Stream 2 delivered both harnesses and raised the number of modules declaring `TESTS` from 12 to 15.

The strongest evidence is behavioural rather than structural. All 18 fixture pairs serialise byte-identically, double the corpus the plan required. A live five-case matrix proved that the JSON path provisions a real directory user, that an absent parameter reproduces classic XML behaviour with the JSON path never engaging and zero events fired, and that a response the gateway mislabels or malforms still resolves the lookup — backward compatibility and total fallback demonstrated, not asserted. Full `make check` exits 0 with 47 of 47 tests and 251 of 251 cases, zero warnings originate in any in-scope file, and the frozen paths — the module-list template, every core header, the build system, packaging, CI workflows and every Windows project file — are 0-diff. Beyond the plan's ask, the co-load crash that made enabling both endpoint modules fatal is closed: a refusal guard now returns an error naming the conflict while the instance keeps serving, verified in both load orders and across an unload.

The 61.5 remaining hours split into 17 hours of AAP-scoped verification and 44.5 hours of path-to-production work, and **none of the remaining AAP hours is unfinished implementation** — they are verifications no Linux container can perform: MSVC compilation against frozen project files, macOS linking, a static-analysis arm whose tool is not installed here, and the CI arm on its target image. What genuinely needs human judgement is narrower. Three changes sit outside the agreed scope and are disclosed rather than hidden: the two endpoint co-load guards, whose delivered extent is wider than the carve-out that authorised them; a 22-line clamp in the core event dispatcher, without which the test estate cannot run at all on a host reporting more than 64 CPUs; and the rewrite of a shipped test wrapper that was reporting success while running nothing. A fourth build file was also touched — the working-directory dependency that stopped the tree's own acceptance command from ever completing. Each needs a decision on disposition, not more engineering. The critical path is correspondingly short: ratify the three reconciliations and record the disposition (11 h), review and merge (14 h), then verify MSVC and run the CI arm on its own image (9 h). That 34 hours of High-priority work clears every merge blocker, and deployment is independent of it — the feature is opt-in per binding, an absent parameter reproduces today's behaviour byte-for-byte, and every JSON failure converges on the original XML parse, so the JSON path stays dormant and harmless until the provisioning backend conforms, which the shipped validator lets the backend team verify first.

| Metric | Target | Actual | Status |
|---|---|---|---|
| AAP paths delivered | 33 | 33 | ✅ |
| Test pass rate | 100%, no failure budget | 47/47 binaries, 251/251 cases | ✅ |
| New test cases | ≥5 per endpoint module | 10 (h323), 11 (opal), 51 (xml_curl) | ✅ |
| Fixture parity | 9 pairs byte-identical | 18/18 byte-identical | ✅ |
| In-scope compiler warnings | 0 | 0 | ✅ |
| Memory safety | No leaks or use-after-free | Sanitizer-clean; decoder suite with no suppression | ✅ |
| Public API/ABI changes | 0 | 0 | ✅ |
| Placeholders or stubs | 0 | 0 | ✅ |
| Endpoint production edits | 0 | 2 files, sanctioned, extent wider than authorised | ⚠ |
| Out-of-scope changes | 0 | 3 disclosed, all load-bearing | ⚠ |
| Cross-platform verification | Windows + macOS | Neither toolchain available here | ⚠ |

**Production readiness: conditionally ready, pending human review.** The Linux build is production-quality — it compiles warning-free under the project's own strict flags, passes the tree's own acceptance command outright, survives sanitizers, and the new provisioning path was proven end to end against a live gateway together with its backward-compatibility and total-fallback guarantees. Risk is structurally bounded by the opt-in design. Three conditions gate release: ratification of the disclosed out-of-scope changes, verification on the real CI image, and MSVC confirmation. None of them indicates defective work; each is a decision or an environment this container cannot provide. Deployment additionally requires hardening the permissive TLS posture, which was deliberately preserved because changing a shipped default would itself have been an out-of-scope behaviour change.

---

# 9. Development Guide

> Every command below was executed against this branch, and the stated outputs are actual observed results. `$REPO` is your clone of the repository.

## 9.1 System prerequisites

| Requirement | Verified value | Notes |
|---|---|---|
| OS | Ubuntu 25.10, x86_64 | Any modern glibc Linux will do; the CI target is Debian-based |
| gcc / g++ | **12.5.0** | **Pinned. Do not move to a newer major** — the H.323 and OPAL toolkits do not compile cleanly under it and the build is `-Werror` |
| autoconf / automake / libtool | 2.72 / 1.17 / 2.5.4 | The tree declares no minimum |
| GNU make / pkg-config | 4.4.1 / 1.8.1 | |
| python3 | 3.13.7 | Needed only for the contract validator (standard library only) |
| Disk / RAM | ~2 GB build tree, 4 GB RAM | 5,273 tracked files, 493 MB working tree |

```bash
gcc --version | head -1        # gcc (Ubuntu 12.5.0-6ubuntu1) 12.5.0
g++ --version | head -1        # g++ (Ubuntu 12.5.0-6ubuntu1) 12.5.0
autoconf --version | head -1   # autoconf (GNU Autoconf) 2.72
automake --version | head -1   # automake (GNU automake) 1.17
make --version | head -1       # GNU Make 4.4.1
pkg-config --version           # 1.8.1
```

## 9.2 Dependency verification

Every dependency must resolve with **no `PKG_CONFIG_PATH` set**:

```bash
for p in libcurl sqlite3 openssl speex speexdsp libpcre2-8 sofia-sip-ua spandsp \
         libks2 signalwire_client2 ldns sndfile lua5.3 ptlib opal libpcap; do
  printf '%-20s %s\n' "$p" "$(pkg-config --modversion "$p" 2>/dev/null || echo MISSING)"
done
```

Observed: `libcurl 8.14.1`, `sqlite3 3.46.1`, `openssl 3.5.3`, `speex/speexdsp 1.2.1`, `libpcre2-8 10.46`, `sofia-sip-ua 1.13.17`, `spandsp 3.1.1`, `libks2 2.0.11`, `signalwire_client2 2.0.5`, `ldns 1.8.4`, `sndfile 1.2.2`, `lua5.3 5.3.6`, `ptlib 2.12.10`, `opal 3.12.10`, `libpcap 1.10.5`.

Two subtleties worth knowing. `pkg-config ptlib` answers for the **OPAL-bundled** PTLib; `mod_h323` links a different one through `-L/usr/lib -lpt`, which `ldd` confirms (`mod_h323.so` → `libpt.so.2.10.9`, `mod_opal.so` → `libpt.so.2.12-beta10`). And libpcap's development package is what makes the collected test count 47 rather than 46, because `configure.ac` gates `tests/unit/switch_rtp_pcap` on `pcap-config`.

The optional endpoint toolkits — absence degrades to uncollected suites, never a failed build:

```bash
ls /usr/include/openh323/h323.h                      # H323Plus headers
pkg-config --atleast-version=3.12.8 opal && echo OK  # matches mod_opal.h's #error floor

# Reproducible provisioning, pinned and CVE-gated. Inspect before installing:
./build/provision_endpoint_toolkits.sh --help             # exit 0
./build/provision_endpoint_toolkits.sh --dry-run          # exit 0 - prints the exact plan
./build/provision_endpoint_toolkits.sh --uninstall-check   # exit 0 - inventory only
./build/provision_endpoint_toolkits.sh --self-test         # exit 0 - 6/6 assertions, installs nothing
```

## 9.3 Environment setup and build

```bash
cd "$REPO"

# First time only - regenerate the build system
./bootstrap.sh -j

# Configure. This single flag is the one this tree is built with:
./configure --enable-fake-dlclose

# Build and install
make -j$(nproc)      # exit 0
make install         # exit 0 -> /usr/local/freeswitch with 53 module .so
```

**Expected build output.** 32 `warning:` lines tree-wide: 27 from third-party `/opt/opalvoip/include/ptlib/*.h`, 4 from out-of-scope `mod_lua.cpp`, 1 tooling notice. **Zero originate in any in-scope file.**

> ⚠ **Enabling the three modules.** All three ship commented out in `build/modules.conf.in` by design — that is the default-build contract, and the file must not be edited. `bootstrap.sh` copies the template to `modules.conf` only when the latter is absent, so uncomment them in the **generated** copy:
> ```bash
> sed -i -e '/xml_int\/mod_xml_curl/s/^#//' \
>        -e '/endpoints\/mod_h323/s/^#//'   \
>        -e '/endpoints\/mod_opal/s/^#//' modules.conf
> ```
> A module commented out in `modules.conf` contributes **nothing** to `print_tests` and **nothing** to `check` — silently, with no warning. `ci.sh` guards against exactly that with an exactly-one-active-line postcondition assertion.

## 9.4 Test collection and execution

```bash
# Collect. Expect 47 unique tests, including all three new suites.
make -s print_tests | tr ' ' '\n' | grep -v '^$' | sort -u | tee /tmp/collected.txt | wc -l   # 47

# The tree's own acceptance command
make check      # exit 0 -> "# TOTAL: 47 / # PASS: 47 / # SKIP: 0 / # FAIL: 0 / # ERROR: 0"
```

The three in-scope suites individually:

```bash
(cd src/mod/xml_int/mod_xml_curl/test && ./test_mod_xml_curl)  # PASSED (51/51 tests in ~0.031s)
(cd src/mod/endpoints/mod_h323/test   && ./test_mod_h323)      # PASSED (10/10 tests in ~0.020s)
(cd src/mod/endpoints/mod_opal/test   && ./test_mod_opal)      # PASSED (11/11 tests in ~0.039s)
```

Per-binary execution, useful for attributing a failure to one binary:

```bash
for t in $(cat /tmp/collected.txt); do
  ( cd "$(dirname "$t")" && "./$(basename "$t")" ) >/dev/null 2>&1
  printf '%s %s\n' "$?" "$t"
done   # every line begins with 0; 47 binaries, 251 FCTX cases
```

> ⚠ **Always run the libtool wrapper `test/<name>`, never `test/.libs/<name>`.** The collected paths are wrapper shell scripts; the real ELF binaries live under `.libs/` and carry `RUNPATH=/usr/local/freeswitch/lib`, so a **stale install silently defeats the run**. Always `make install` before testing.

The capability guards and the contract validator:

```bash
./ci.sh --guard-self-test    # exit 0, "all 5 arms passed", working tree left byte-identical

cd src/mod/xml_int/mod_xml_curl/test/tools
python3 validate_badgerfish.py ../fixtures/directory_user_simple.json              # exit 0
python3 validate_badgerfish.py ../fixtures/badgerfish_invalid_root_attribute.json  # exit 1 + named violation
```

## 9.5 Runtime startup and verification

```bash
cd /usr/local/freeswitch                    # must be its own statement: & binds looser than &&
setsid nohup ./bin/freeswitch -nc -nonat -ncwait > /tmp/fs.out 2>&1 < /dev/null & disown
sleep 30

CLI="./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x"
$CLI 'status'                # -> "FreeSWITCH (Version 1.11.2-dev ...) is ready"
$CLI 'load mod_xml_rpc'      # -> +OK

curl -s -o /dev/null -w '%{http_code}\n' -u freeswitch:works http://127.0.0.1:8080/api/status  # 200
curl -s -o /dev/null -w '%{http_code}\n'                      http://127.0.0.1:8080/api/status  # 401
```

> ⚠ **`-nc` means console output does NOT reach your redirect target.** The authoritative log sink is **`/usr/local/freeswitch/log/freeswitch.log`**. Grepping the wrong file shows zero warnings even when warnings were emitted.

Endpoint modules — **one per instance**:

```bash
$CLI 'load mod_opal'; $CLI 'show endpoint' | grep '^endpoint,opal'   # endpoint,opal,mod_opal
# mod_h323 belongs in a FRESH instance. In this one it is refused, by design:
#   mod_h323.cpp: ... already reserved by [mod_opal] ... restart FreeSWITCH
```

Stop cleanly:

```bash
PID=$(pgrep -f 'bin/freeswitch -nc' | head -1); kill -TERM "$PID"   # exits in ~6 s
```

## 9.6 Example usage — live BadgerFish JSON provisioning

Stand up a minimal provisioning gateway:

```bash
mkdir -p /tmp/gw && cat > /tmp/gw/gw.py <<'PYEOF'
import sys, json
from http.server import BaseHTTPRequestHandler, HTTPServer

def badgerfish():
    # NOTE: no "@type" on the top-level key - the translator OWNS the envelope.
    return json.dumps({"directory": {"domain": {"@name": "127.0.0.1", "user": {
        "@id": "5001",
        "params":    {"param":    {"@name": "password",       "@value": "badgerfish-secret"}},
        "variables": {"variable": {"@name": "provisioned_by", "@value": "badgerfish-json"}}}}}})

XML = ('<document type="freeswitch/xml">\n<section name="directory">\n<domain name="127.0.0.1">\n'
       '<user id="5001">\n<params>\n<param name="password" value="classic-xml"/>\n</params>\n'
       '<variables>\n<variable name="provisioned_by" value="classic-xml"/>\n</variables>\n'
       '</user>\n</domain>\n</section>\n</document>\n')

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0) or 0))
        if   self.path == '/json':      body, ct = badgerfish(), 'application/json'
        elif self.path == '/badjson':   body, ct = '{"directory": {"@type": ', 'application/json'
        elif self.path == '/wrongtype': body, ct = badgerfish(), 'text/xml'
        else:                           body, ct = XML, 'text/xml'
        b = body.encode()
        self.send_response(200); self.send_header('Content-Type', ct)
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)

HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
PYEOF
setsid nohup python3 /tmp/gw/gw.py 18080 > /tmp/gw/gw.log 2>&1 < /dev/null & disown
```

Point a binding at it, backing up the shipped file first:

```bash
cd /usr/local/freeswitch
cp conf/autoload_configs/xml_curl.conf.xml /tmp/xml_curl.conf.xml.orig
cat > conf/autoload_configs/xml_curl.conf.xml <<'EOF'
<configuration name="xml_curl.conf" description="cURL XML Gateway">
  <bindings>
    <binding name="dg">
      <param name="gateway-url" value="http://127.0.0.1:18080/json" bindings="directory"/>
      <param name="response-format" value="json"/>
    </binding>
  </bindings>
</configuration>
EOF

CLI="./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x"
$CLI 'load mod_xml_curl'                                  # +OK
$CLI 'user_data 5001@127.0.0.1 var provisioned_by'        # badgerfish-json
$CLI 'user_data 5001@127.0.0.1 param password'            # badgerfish-secret
```

Observed results across all five cases — swap the URL path, toggle the parameter, then `reload mod_xml_curl`:

| Case | Configuration | Observed result |
|---|---|---|
| JSON happy path | `/json` + `response-format=json` | `provisioned_by=badgerfish-json`, `password=badgerfish-secret`; 0 warnings, 0 events |
| Malformed JSON | `/badjson` + `response-format=json` | Warning `not a well-formed BadgerFish JSON document … falling back to XML parsing` plus one `Fallback-Reason: malformed-json` event |
| Wrong content type | `/wrongtype` + `response-format=json` | Warning `response Content-Type is not application/json …` plus one `Fallback-Reason: content-type-mismatch` event |
| **XML default** | `/xml`, **parameter absent** | `classic-xml` — the JSON path never engages, no `Accept` header is sent, 0 events (**backward compatibility**) |
| XML via JSON config | `/xml` + `response-format=json` | Warning fires **and the lookup still succeeds** (**total fallback**) |

Watch the degradation signal — the event is the one to alert on, because it needs no log scraping:

```bash
# Interactive subscription (fs_cli -x runs one command and exits, so it cannot watch a stream):
./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon
#   /event plain CUSTOM xml_curl::json_fallback

L=/usr/local/freeswitch/log/freeswitch.log
grep -c 'falling back to XML parsing' "$L"
grep -c '/badjson' "$L"          # 0 - the URL path never leaks; host:port is retained
```

Restore afterwards:

```bash
cp /tmp/xml_curl.conf.xml.orig /usr/local/freeswitch/conf/autoload_configs/xml_curl.conf.xml
GP=$(pgrep -f 'gw.py 18080' | head -1); [ -n "$GP" ] && kill "$GP"
```

## 9.7 The BadgerFish contract — read this before writing a gateway

```jsonc
{
  "directory": {                 // single top-level key == the requested section name
    "user": {                    // child element
      "@id": "1000",             // attribute, "@"-prefixed, order matters
      "params": {
        "param": [               // repeated same-name children become a JSON array
          { "@name": "password",    "@value": "1234" },
          { "@name": "vm-password", "@value": "1000" }
        ]
      }
    }
  }
}
```

Rules, each enforced by a test case and by `test/tools/validate_badgerfish.py`:

- **The translator owns the envelope.** It synthesises `<document type="freeswitch/xml"><section name="{key}">` from the single top-level key. **Never emit envelope attributes on the top-level key** — that is rejected.
- **All values must be JSON strings.** No numbers, booleans or nulls: there is no guaranteed lexical round-trip.
- **Attributes are `@`-prefixed**, text content uses `$`, and `@`-member order must match the intended XML attribute order.
- **An element has children or text, never both** — mixed content is unrepresentable.
- **Nesting is bounded at depth 32** by the module itself, which is the only limit that governs; the corpus's boundary fixture sits at depth 31.
- **The XML and JSON paths differ on one thing:** the XML path expands `$${global}` tokens during parsing, the translator copies values verbatim. Do not emit such tokens from a gateway.
- Non-conformance is safe: the module warns, fires the fallback event and falls back to XML rather than failing the lookup. Run your output through the validator first:

```bash
python3 src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py /path/to/response.json
# exit 0 = conformant, exit 1 = named violations on stderr, exit 2 = usage error
```

The full operator reference — the fallback contract, both reason tokens with their stable log and event signatures, all 11 resource ceilings, the `file:` boundary, TLS hardening, cookie-jar placement and the endpoint co-load prohibition — ships beside the module at `src/mod/xml_int/mod_xml_curl/README.response-format.md`.

## 9.8 Troubleshooting

| Symptom | Cause | Resolution |
|---|---|---|
| `load mod_xml_curl` → `-ERR [module load file routine returned an error]` | The shipped configuration ships a `<binding>` whose `gateway-url` is commented out, and the module refuses a binding with no URL. Pre-existing behaviour, not a regression | Set a real `gateway-url` in `xml_curl.conf.xml`, then `reload mod_xml_curl` |
| Zero warnings in your log even though warnings fired | `-nc` sends console output away from your redirect target | Read `/usr/local/freeswitch/log/freeswitch.log` |
| Tests pass but sanitizers report nothing, or behaviour looks stale | The binaries carry `RUNPATH=/usr/local/freeswitch/lib` and loaded a **stale installed** library | `make install` before testing; run `test/<name>`, never `test/.libs/<name>` |
| `load mod_opal` refused with *"already reserved by [mod_h323]"* | By design. The two modules link different PTLib runtimes whose process singletons collide, so the second load would crash the process. The claim deliberately outlives an unload, because the first runtime stays mapped | Run the two endpoint modules in **separate instances**. Never clear `_fs_ptlib_endpoint_reservation` by hand — doing so re-opens the crash |
| A new suite never runs and no error appears | Its module is commented out in `modules.conf`, so it contributes nothing to `print_tests` — silently | Uncomment it in the **generated** `modules.conf` (§9.3), never the template |
| `mod_h323` or `mod_opal` will not build | The H.323 or OPAL toolkit is absent, and no distribution ships the OPAL development package | Expected degradation — the capability guards leave the module disabled. Provision with `./build/provision_endpoint_toolkits.sh` (inspect with `--dry-run` first) |
| `make -s print_tests` reports 46, not 47 | libpcap's development package is missing, so `configure` disables the tree's own pcap test | Install it and re-run `./configure`; the CI driver reports the capability explicitly so the count never moves silently |
| A JSON body over `response-max-bytes` produces no fallback warning and no event | The cap is enforced on the transport, before the format dispatch, so the decoder is never entered | Alert on the `Oversized file detected` log line as well as on the fallback event |
| `xmllint`, `xxd`, `valgrind` or `scan-build` not found | None is installed in this environment | Use `python3 -c 'import xml.etree.ElementTree as E; E.parse("f.xml")'` and `od -c`; run the analyser arm on a host that carries clang's tooling |

---

# 10. Appendices

## Appendix A — Command Reference

| Purpose | Command |
|---|---|
| Regenerate the build system | `./bootstrap.sh -j` |
| Configure | `./configure --enable-fake-dlclose` |
| Build | `make -j$(nproc)` |
| Install | `make install` |
| Clean rebuild of one module | `cd src/mod/xml_int/mod_xml_curl && make clean && make` |
| Collect tests | `make -s print_tests` |
| Run the whole estate | `make check` |
| Run one suite | `(cd src/mod/xml_int/mod_xml_curl/test && ./test_mod_xml_curl)` |
| Prove the compile flags reach a file | `make V=1 2>&1 \| grep -- -Werror` |
| Capability-guard self-test | `./ci.sh --guard-self-test` |
| Toolkit provisioning plan | `./build/provision_endpoint_toolkits.sh --dry-run` |
| Toolkit provisioning self-test | `./build/provision_endpoint_toolkits.sh --self-test` |
| Validate gateway JSON | `python3 src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py <file-or-dir>` |
| Start FreeSWITCH | `setsid nohup ./bin/freeswitch -nc -nonat -ncwait > /tmp/fs.out 2>&1 < /dev/null & disown` |
| ESL command | `./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x '<cmd>'` |
| Stop FreeSWITCH | `kill -TERM $(pgrep -f 'bin/freeswitch -nc' \| head -1)` |
| Diff against the upstream base | `git diff 0a54a48f37..HEAD --stat` |
| Verify authorship | `git log --format='%an <%ae>' 0a54a48f37..HEAD \| sort -u` |

## Appendix B — Port Reference

| Port | Service | Notes |
|---|---|---|
| 8021 | Event Socket (ESL) | `fs_cli` control; password `ClueCon` |
| 8080 | `mod_xml_rpc` HTTP | Basic auth `freeswitch`/`works`; 200 authenticated, 401 without |
| 5060 / 5080 | SIP (`mod_sofia`) | Internal / external profiles |
| 1720 | H.323 signalling | `H323EndPoint::DefaultTcpSignalPort`; the test fixtures bind loopback on a high unprivileged port instead |
| 4569 | IAX2 (`mod_opal`) | The `mod_opal` suite waits for and releases this port rather than assuming it is free |
| 18080 | Test provisioning gateway | Ad-hoc, §9.6 only |

## Appendix C — Key File Locations

| Path | Role |
|---|---|
| `src/mod/xml_int/mod_xml_curl/mod_xml_curl.c` | Stream 1 target — 624 → 2,191 lines; four seams plus 18 file-static helpers |
| `src/mod/xml_int/mod_xml_curl/test/test_mod_xml_curl.c` | 51-case suite, 6,432 lines; white-box includes `../mod_xml_curl.c` |
| `src/mod/xml_int/mod_xml_curl/test/fixtures/` | 18 XML/JSON parity pairs plus 5 deliberately non-conformant samples |
| `src/mod/xml_int/mod_xml_curl/test/tools/validate_badgerfish.py` | Standalone gateway contract validator, 1,734 lines, standard library only |
| `src/mod/xml_int/mod_xml_curl/README.response-format.md` | Operator runbook, 947 lines — the authoritative operator reference |
| `src/mod/endpoints/mod_h323/test/test_mod_h323.cpp` | 10-case suite, 3,646 lines |
| `src/mod/endpoints/mod_opal/test/test_mod_opal.cpp` | 11-case suite, 3,800 lines |
| `src/mod/endpoints/mod_h323/mod_h323.cpp`, `src/mod/endpoints/mod_opal/mod_opal.cpp` | Carry the sanctioned co-load refusal guard; awaiting ratification (§5.2 D1) |
| `conf/vanilla/autoload_configs/xml_curl.conf.xml` | Canonical operator documentation for `response-format`; `conf/curl`, `conf/insideout` and `conf/testing` carry the identical block |
| `ci.sh` | Capability-guarded CI enablement and the five-arm `--guard-self-test` (2,014 lines) |
| `build/provision_endpoint_toolkits.sh` | Pinned, CVE-gated endpoint-toolkit provisioning (2,766 lines) |
| `build/patches/endpoint_toolkits/` | 8 pinned compatibility patches, inputs to provisioning only |
| `build/modules.conf.in` | **Template — must stay commented.** The default-build contract |
| `modules.conf` | Generated, git-ignored, dev-enabled copy — edit this one |
| `src/switch_event.c` | Disclosed out-of-AAP dispatch-thread clamp (+22) — §5.2 D7 |
| `src/mod/endpoints/mod_sofia/test/test_sofia_funcs.sh` | Disclosed out-of-AAP wrapper repair (+197/−11) — §5.2 D7 |
| `blitzy/documentation/` | In-repository record of the endpoint co-load determination, the toolkit refusal proof and the fallback-matrix captures |
| `/usr/local/freeswitch/log/freeswitch.log` | **Authoritative runtime log sink** |

## Appendix D — Technology Versions

| Component | Version | Source |
|---|---|---|
| FreeSWITCH | 1.11.2-dev | `configure.ac` |
| cJSON (vendored) | 1.7.12 | `src/include/switch_cJSON.h` |
| FCTX test framework | 1.6.1 | `src/include/test/switch_fct.h` |
| fspr (APR fork) | APR 1.2.8 | `libs/apr/include/fspr_version.h` |
| libcurl | 8.14.1 (declared minimum 7.19) | pkg-config / `configure.ac` |
| OpenSSL | 3.5.3 | pkg-config |
| SQLite | 3.46.1 | pkg-config |
| sofia-sip-ua | 1.13.17 | pkg-config |
| spandsp | 3.1.1 | pkg-config |
| libpcap | 1.10.5 | pkg-config — decides whether the collected count is 47 or 46 |
| PTLib for `mod_h323` | **2.10.9**, with H323Plus 1.28.0 | soname via `ldd`; pinned by `build/provision_endpoint_toolkits.sh` |
| PTLib bundled with OPAL | **2.12-beta10** | `ldd` on `mod_opal.so` |
| OPAL | 3.12.10 | pkg-config |
| gcc / g++ | 12.5.0 (**pinned**) | host |
| python3 | 3.13.7 | host — validator only |
| OS | Ubuntu 25.10 | host |

## Appendix E — Environment Variable Reference

**No environment variables are required** for build, test or runtime. Credentials are repository defaults.

| Variable | Purpose | Value used |
|---|---|---|
| `ASAN_OPTIONS` | Sanitizer behaviour | Exported by `ci.sh` in the unit-test arm. Never relax it globally — that would disarm the decoder suite's leak gate |
| `PKG_CONFIG_PATH` | Toolkit discovery | **Deliberately unset** — every dependency resolves without it |
| `CI` | Non-interactive tooling | `true` in CI |
| — | ESL password | `ClueCon` (repository default) |
| — | HTTP Basic auth | `freeswitch` / `works` (repository default) |

## Appendix F — Developer Tools Guide

| Tool | Availability | Usage |
|---|---|---|
| `python3` | Present | The contract validator, the ad-hoc test gateway, XML well-formedness checks |
| `curl` | Present | HTTP authentication matrix |
| `gdb` | Present | Used to establish the endpoint co-load ordering |
| `shellcheck` / `shfmt` | Present | The provisioner reports zero findings; `ci.sh` reports exactly its baseline set |
| Chrome (headless) | Present | HTTP API surface verification; needs `--no-sandbox --disable-dev-shm-usage` |
| `scan-build` (clang) | **Not installed here** | Run the static-analysis arm on a host that carries clang's tooling (tasks M-3, M-4) |
| `valgrind` | **Not installed here** | AddressSanitizer covers the same ground for these suites |
| `xmllint` | **Not installed here** | Use `python3 -c 'import xml.etree.ElementTree as E; E.parse("f.xml")'` |
| `xxd` | **Not installed here** | Use `od -c` |

## Appendix G — Glossary

| Term | Definition |
|---|---|
| **AAP** | Agent Action Plan — the specification this engagement was measured against |
| **BadgerFish** | The XML-to-JSON convention used here: attributes `@`-prefixed, text under `$`, repeated children as arrays, root key equal to the requested section name |
| **FST** | FreeSWITCH Test macros (`src/include/test/switch_test.h`) layered over the vendored FCTX framework |
| **Seam** | A narrowly scoped insertion point where new behaviour attaches without disturbing surrounding code |
| **Total fallback** | Every JSON failure edge converges on the untouched XML parse, so a decode failure never fails a lookup |
| **Serializer identity** | The acceptance criterion for a fixture pair: the XML and its JSON twin produce byte-identical serialiser output |
| **White-box inclusion** | A test unit `#include`s the production source to reach file-static symbols without changing their visibility |
| **Convenience library** | A `noinst_LTLIBRARIES` archive letting a test binary link module objects without a second compile |
| **`PProcess`** | PTLib's process singleton. Two PTLib runtimes in one process mean two colliding singletons, which is why the endpoint co-load guard exists |
| **Reservation** | The process-global claim (`_fs_ptlib_endpoint_reservation`) naming whichever endpoint module owns the PTLib runtime. It deliberately outlives an unload |
| **Path-to-production** | Deployment activities required to release the AAP deliverables, distinct from the deliverables themselves |
| **Exit 77** | Automake's "skip" status. Zero occurrences here — the tree carries no failure budget |

---

## Cross-Section Integrity Verification

| Rule | Check | Result |
|---|---|---|
| **Rule 1** (§1.2 ↔ §2.2 ↔ §7) | Remaining hours identical in the §1.2 metrics table (**61.5**), the §2.2 total (**61.5**) and the §7 pie "Remaining Work" (**61.5**) | ✅ Pass |
| **Rule 2** (§2.1 + §2.2 = Total) | §2.1 = **386**; §2.2 = **61.5**; 386 + 61.5 = **447.5** = §1.2 Total Hours | ✅ Pass |
| **Rule 3** (§3) | Every figure was measured on this branch during this assessment — `make check` exit 0 at 47/47, 251 cases harvested from all 47 binary logs, 47 `.trs` files all PASS, the three suites run individually. §3 states what no test covers | ✅ Pass |
| **Rule 4** (§1.5) | Access validated against live permissions — repository writes, all 16 dependencies resolving with no `PKG_CONFIG_PATH`, both endpoint toolkits, ESL and HTTP authentication | ✅ Pass |
| **Rule 5** (colours) | Completed = `#5B39F3`; Remaining = `#FFFFFF`; accents `#B23AF2`; highlight `#A8FDD9` — applied in §1.2 and every §7 chart | ✅ Pass |
| **Rule 6** (§5.2 ↔ §1.4 ↔ §2.2) | Both release-blocking divergences (D1, D7) appear in §1.4; every divergence needing human work maps to a §2.2 category — D1 and D7 to the ratification line, D2 to documentation ratification, D4 and D5 to review and merge, D6 to tasks H-5/H-12. D3 and D8 need no human action and say so | ✅ Pass |
| **Percentage consistency** | **86.3%** in §1.2, §7 (386/447.5) and §8. No other completion figure appears | ✅ Pass |
| **Task reconciliation** | 30 tasks = **61.5 h**; priority split High **34.5** / Medium **21** / Low **6** matches §2.2; all 13 category roll-ups match cell for cell | ✅ Pass |
| **Hour mentions** | Every hour figure traces to 386 / 61.5 / 447.5; §2.1 verified summing to 386 (146 + 85 + 47 + 108) and §2.2 to 61.5 by computation | ✅ Pass |
