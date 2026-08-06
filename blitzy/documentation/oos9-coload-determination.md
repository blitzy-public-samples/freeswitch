# OOS-9 co-load determination — `mod_h323` + `mod_opal`

**Question the determination had to answer.** The OOS-9 guard policy is selected by a decision
rule that turns on one fact: when `mod_h323` and `mod_opal` are loaded into the same FreeSWITCH
process, does the SIGSEGV happen during `dlopen` static initialisation — before any module code
executes — or during/after `switch_module_load`?

**Answer: the fault happens INSIDE the second module's `switch_module_load`. Module code executes
first, in both load orders. The decision rule therefore selects OUTCOME A** — a mutual-exclusion
guard at the top of each module's load function.

The prior record (`blitzy/documentation/Project Guide.md` §1.4 and §9.8) described the symptom and
its cause but archived no backtrace; **no backtrace existed under `blitzy/`** when this
determination began. Everything below was produced first-hand on this host.

---

## 1. Environment

| Fact | Value |
|---|---|
| Repository state | `323d52c88a` plus the Project Guide commit; both endpoint sources 0-diff, i.e. **unguarded** |
| Installed artefacts | `/usr/local/freeswitch/mod/mod_h323.so`, `/usr/local/freeswitch/mod/mod_opal.so` |
| `mod_h323.so` PTLib | `libpt.so.2.10.9` (`/lib`), with `libh323_linux_x86_64_.so.1.28.0` |
| `mod_opal.so` PTLib | `libpt.so.2.12-beta10` (`/opt/opalvoip/lib`), with `libopal.so.3.12-beta10` |
| Debugger | `gdb` 16.3 (installed for this determination; the Project Guide's tool table claimed it was present, it was not) |
| Load mode | `switch_dso_open()` → `dlopen(path, RTLD_NOW | RTLD_LOCAL)`; both modules declare `SMODF_NONE` (`switch_types.h:2650-2651`), so neither asks for global symbols |

**Reading the line numbers below.** Every frame and log line quoted here comes from the
**unguarded** sources, so its line numbers are those of `323d52c88a`. The shipped tree adds a net
**98 lines to `mod_h323.cpp` and 97 to `mod_opal.cpp`**, and — this is the part worth reading
carefully — **the shift is piecewise, not one constant**, because the change lands in six separate
hunks per file: four belong to the guard (§7) — the reservation name above the load function, the
two refusal stages at the top of it, and the two release sites on the failure edges that precede
`new FSProcess()` — and two are unrelated compiler-warning fixes far below it (in `mod_h323.cpp` a
log format string that carried four conversions for five arguments, and two `uint32_t` fields
printed with `%lu`; in `mod_opal.cpp` three string literals that abutted an identifier, which C++11
lexes as a user-defined literal). Measured against the shipped tree, where a dash marks a line that
was rewritten rather than moved:

| `mod_h323.cpp`, pre-guard line | shift | | `mod_opal.cpp`, pre-guard line | shift |
|---|---|---|---|---|
| 1 – 154 | +0 | | 1 – 101 | +0 |
| 155 – 156 | +9 | | 102 – 103 | +9 |
| 157 – 161 | +79 | | 104 – 112 | +81 |
| 162 – 169 | +85 | | 113 – 117 | +87 |
| 170 – 2084 | +86 | | 118 – 172 | +88 |
| 2085 | — rewritten | | 173 | — rewritten |
| 2086 – 2155 | +93 | | 174 – 293 | +94 |
| 2156 – 2157 | — rewritten | | 294 – 295 | — rewritten |
| 2158 and after | +98 | | 296 and after | +97 |

Every anchor this document quotes, translated once so no reader has to do the arithmetic:

| Pre-guard anchor | Guarded tree | What it is |
|---|---|---|
| `mod_h323.cpp:157` | **`:236`** | `mod_h323_load`'s entry log line |
| `mod_h323.cpp:167` | **`:252`** | `h323_process = new FSProcess();` — the gdb frame in §4 |
| `mod_h323.cpp:174` | **`:260`** | "H323 mod initialized and running" |
| `mod_h323.cpp:363` | **`:449`** | the `FSProcess::FSProcess` frame in §4 |
| `mod_h323.cpp:42`, `:57` | **`:42`, `:57`** | unchanged — both precede the guard |
| `mod_opal.cpp:104` | **`:185`** | `mod_opal_load`'s entry log line |
| `mod_opal.cpp:116` | **`:203`** | `opal_process = new FSProcess();` — the gdb frame in §3 |
| `mod_opal.cpp:122` | **`:210`** | "Opal manager initialized and running" |
| `mod_opal.cpp:239` | **`:333`** | the `FSProcess::FSProcess` frame in §3 |
| `mod_opal.cpp:58`, `:61`, `:62` | **`:58`, `:61`, `:62`** | unchanged — all precede the guard |
| `mod_h323.h:630` | **`:630`** | `h323_process`; the header is 0-diff |

The same piecewise mapping applies to the `mod_h323.cpp:<line>` and `mod_opal.cpp:<line>`
anchors cited in the two endpoint suites' comments, which were deliberately left on the
pre-guard numbering rather than mass-rewritten. Each suite's header now says so explicitly and
points here, so no reader of those files is misled by an anchor that lands on a comment.

`readelf -d` on both modules confirms each names its own PTLib SONAME plus
`libfreeswitch.so.1`, so the two runtimes coexist rather than one satisfying the other:

```
mod_h323.so NEEDED: libh323_linux_x86_64_.so.1.28.0, libpt.so.2.10.9, libfreeswitch.so.1, …
mod_opal.so NEEDED: libopal.so.3.12-beta10,          libpt.so.2.12-beta10, libfreeswitch.so.1, …
```

## 2. Why the ordering question is decidable at all

`switch_loadable_module_load_file()` (`src/switch_loadable_module.c:1636`) performs the two steps
in a fixed order:

1. `switch_dso_open(path, …)` — `dlopen`, which runs every static initialiser in the module and in
   each library it pulls in (`src/switch_dso.c`, `dlopen(path, RTLD_NOW | RTLD_LOCAL)`);
2. `status = load_func_ptr(&module_interface, pool);` (`src/switch_loadable_module.c:1720`) — the
   module's own `switch_module_load`.

Two independent discriminators therefore exist. `mod_h323_load`'s first statement is
`switch_log_printf(… SWITCH_LOG_CONSOLE, "Starting loading mod_h323\n")` (`mod_h323.cpp:157`) and
`mod_opal_load`'s is the same for OPAL (`mod_opal.cpp:104`), so **a log line proves module code
ran**; and a stack frame inside `mod_*_load` proves the same thing independently of logging.

## 3. Controlled dual-load, order 1 — `mod_h323` then `mod_opal`

Exact commands (one disposable instance, no shipped or installed configuration modified):

```sh
cd /usr/local/freeswitch
gdb -batch -nx \
  -ex 'set pagination off' -ex 'set confirm off' -ex 'set print frame-arguments none' \
  -ex 'handle SIGPIPE nostop noprint pass' \
  -ex 'handle SIGUSR1 nostop noprint pass' -ex 'handle SIGUSR2 nostop noprint pass' \
  -ex 'handle SIG32 nostop noprint pass'  -ex 'handle SIG33 nostop noprint pass' \
  -ex 'handle SIG34 nostop noprint pass' \
  -ex run -ex 'info program' -ex 'frame' -ex 'bt' -ex 'info sharedlibrary' \
  --args ./bin/freeswitch -nc -nf -nonat
# from a second shell, once ./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x status answers:
./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x 'load mod_h323'   # +OK
./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x 'load mod_opal'   # process dies
./bin/fs_cli -H 127.0.0.1 -P 8021 -p ClueCon -x status            # Error Connecting []
```

`mod_h323` loaded cleanly (`Successfully Loaded [mod_h323]`, `Adding Endpoint 'h323'`). The second
load killed the process. Faulting thread (full capture:
`oos9-coload-evidence/order1-h323-then-opal.gdb.txt`):

```
It stopped with signal SIGSEGV, Segmentation fault.
#0  PThreadLocalStorage<PTraceInfo::ThreadLocalInfo>::Get() const () from /opt/opalvoip/lib/libpt.so.2.12-beta10
#1  PTraceInfo::InternalBegin(bool, unsigned int, char const*, int, PObject const*, char const*) () from …libpt.so.2.12-beta10
#2  PTrace::Begin(unsigned int, char const*, int, PObject const*, char const*) () from …libpt.so.2.12-beta10
#3  PProcess::Construct() () from /opt/opalvoip/lib/libpt.so.2.12-beta10
#4  PProcess::PProcess(…) () from /opt/opalvoip/lib/libpt.so.2.12-beta10
#5  PLibraryProcess::PLibraryProcess (…) at /opt/opalvoip/include/ptlib/pprocess.h:764
#6  FSProcess::FSProcess (this=…) at mod_opal.cpp:239
#7  mod_opal_load (module_interface=…, pool=…) at mod_opal.cpp:116
#8  switch_loadable_module_load_file (…) at src/switch_loadable_module.c:1720
#9  switch_loadable_module_load_module_ex (…) at src/switch_loadable_module.c:1827
#10 switch_loadable_module_load_module (…) at src/switch_loadable_module.c:1781
#11 load_function (…) at mod_commands.c:2736
…  api_exec → parse_command → listener_run (mod_event_socket)
```

`info sharedlibrary` for the same process, showing **both PTLib runtimes mapped simultaneously**:

```
/usr/local/freeswitch/mod/mod_h323.so
/lib/libh323_linux_x86_64_.so.1.28.0
/lib/libpt.so.2.10.9
/usr/local/freeswitch/mod/mod_opal.so
/opt/opalvoip/lib/libopal.so.3.12-beta10
/opt/opalvoip/lib/libpt.so.2.12-beta10
```

Log tail (`oos9-coload-evidence/order1-h323-then-opal.freeswitch-log.txt`) — the last line the
process ever wrote is the OPAL load function's own first statement:

```
[CONSOLE] mod_h323.cpp:157 Starting loading mod_h323
[CONSOLE] mod_h323.cpp:174 H323 mod initialized and running
[CONSOLE] switch_loadable_module.c:1772 Successfully Loaded [mod_h323]
[NOTICE]  switch_loadable_module.c:172 Adding Endpoint 'h323'
…
[CONSOLE] mod_opal.cpp:104 Starting loading mod_opal      <-- last line written
```

## 4. Controlled dual-load, order 2 — `mod_opal` then `mod_h323`

Same commands with the two module names exchanged. `mod_opal` loaded cleanly (`Opal manager
initialized and running`, `Successfully Loaded [mod_opal]`); the second load killed the process.
Faulting thread (full capture: `oos9-coload-evidence/order2-opal-then-h323.gdb.txt`):

```
It stopped with signal SIGSEGV, Segmentation fault.
#0  PString::IsEmpty() const () from /lib/libpt.so.2.10.9
#1  PTrace::Begin(unsigned int, char const*, int) () from /lib/libpt.so.2.10.9
#2  PProcess::Construct() () from /lib/libpt.so.2.10.9
#3  PProcess::PProcess(…) () from /lib/libpt.so.2.10.9
#4  PLibraryProcess::PLibraryProcess (…) at /usr/include/ptlib/pprocess.h:769
#5  FSProcess::FSProcess (this=…) at mod_h323.cpp:363
#6  mod_h323_load (module_interface=…, pool=…) at mod_h323.cpp:167
#7  switch_loadable_module_load_file (…) at src/switch_loadable_module.c:1720
…  same core → mod_commands → mod_event_socket tail as order 1
```

Log tail (`oos9-coload-evidence/order2-opal-then-h323.freeswitch-log.txt`):

```
[CONSOLE] mod_opal.cpp:104 Starting loading mod_opal
[CONSOLE] mod_opal.cpp:122 Opal manager initialized and running
[CONSOLE] switch_loadable_module.c:1772 Successfully Loaded [mod_opal]
…
[CONSOLE] mod_h323.cpp:157 Starting loading mod_h323      <-- last line written
```

The crash is therefore symmetric and deterministic: whichever module loads **second** faults while
constructing its `FSProcess`, inside the PTLib runtime *that module* is linked against.

## 5. Corroborating probe — static initialisation alone does NOT fault

To rule out the possibility that the fault merely *appeared* to be in module code, both modules
were mapped into one process with no module entry point called at all, using the same mode
`switch_dso_open()` uses (`oos9-coload-evidence/staticinit-probe.c`, output in
`oos9-coload-evidence/staticinit-probe.txt`):

```
$ ./staticinit-probe /usr/local/freeswitch/mod/mod_h323.so /usr/local/freeswitch/mod/mod_opal.so
dlopen OK (static initialisation complete) /usr/local/freeswitch/mod/mod_h323.so
dlopen OK (static initialisation complete) /usr/local/freeswitch/mod/mod_opal.so
both modules mapped into ONE process; no fault during static initialisation
exit=0

$ ./staticinit-probe /usr/local/freeswitch/mod/mod_opal.so /usr/local/freeswitch/mod/mod_h323.so
… exit=0
```

`dlopen(RTLD_NOW)` resolves every relocation eagerly, so this also proves the two runtimes can be
*linked* into one image without a fault. Only constructing the second `PProcess` fails.

## 6. Applying the decision rule

| Discriminator the rule names | Order 1 (h323 → opal) | Order 2 (opal → h323) |
|---|---|---|
| Second module's load-function log line observed | **Yes** — `mod_opal.cpp:104 Starting loading mod_opal` | **Yes** — `mod_h323.cpp:157 Starting loading mod_h323` |
| Frame inside `mod_*_load` on the faulting stack | **Yes** — `#7 mod_opal_load … mod_opal.cpp:116` | **Yes** — `#6 mod_h323_load … mod_h323.cpp:167` |
| Fault in `dl_init` / a library constructor / a static initialiser | No | No |
| Static initialisation of both modules in one process | Completes, exit 0 | Completes, exit 0 |

**Module code executes before the crash ⇒ OUTCOME A.**

The mechanism is now fully explained, and it is why the refusal has to live in module code:
`h323_process` (`mod_h323.h:630`) and `opal_process` (`mod_opal.cpp:58`) are both null until
`switch_module_load` allocates them, so the conflicting `PProcess` singleton only comes into
existence when the second load function runs — and at that moment `PProcess::Construct()` reaches
PTLib's `PTrace` state (`PTraceInfo` thread-local storage in 2.12-beta10,
`PString::IsEmpty()` in 2.10.9) through a process whose PTLib global state the *other* runtime
already owns. Both crash frames are in frozen third-party code, so the crash itself cannot be
fixed here; refusing the second load is the safe degradation.

## 7. What was delivered, and what it means for a reviewer

Outcome A was delivered as a **two-stage** guard, and both stages are the first thing each load
function does — before `switch_loadable_module_create_module_interface` and before
`new FSProcess()`, which is the operation that would crash.

### 7.1 Stage 1 — the sibling-in-the-hash check

- `mod_h323_load` refuses when `switch_loadable_module_exists("mod_opal") == SWITCH_STATUS_SUCCESS`
  (`mod_h323.cpp:189`, ERROR at `:190`, `return SWITCH_STATUS_FALSE` at `:196`);
- `mod_opal_load` refuses when `switch_loadable_module_exists("mod_h323") == SWITCH_STATUS_SUCCESS`
  (`mod_opal.cpp:136`, ERROR at `:137`, `return SWITCH_STATUS_FALSE` at `:144`).

Each logs `SWITCH_LOG_ERROR` naming the `PProcess` singleton conflict and both PTLib runtimes, then
returns `SWITCH_STATUS_FALSE`, which `switch_loadable_module_load_file()` reports as
`Error Loading module … **Module load routine returned an error**` while the process keeps running.

### 7.2 Stage 2 — the atomic reservation, which is the stage that is safe under concurrency

Stage 1 is a **snapshot**. `switch_loadable_module_exists()` takes `loadable_modules.mutex` for its
own lookup and releases it, and the core holds no single lock across a sibling observation, a
module's load routine and the publication of its result —
`switch_loadable_module_load_module_ex()` checks the hash, releases, calls the module's load
routine, and publishes under a separate lock later. Two concurrent load requests can therefore
both observe the sibling absent and both proceed to construct a `PProcess`, which is exactly the
fatal case. Unloading defeats stage 1 outright rather than merely racing it: the sibling leaves the
module hash while its PTLib runtime stays mapped, so afterwards there is nothing for stage 1 to
observe at all.

Immediately after stage 1, therefore, each module claims one process-global reservation with a
compare-and-swap. `switch_core_set_var_conditional()` holds `runtime.global_var_rwlock` in **write**
mode across its whole test-and-set, so with the empty string as `val2` it succeeds only if the
variable did not exist or was empty, and returns `SWITCH_FALSE` having changed nothing otherwise. A
second call with the module's own name as `val2` follows, so a module that **already** holds the
reservation re-claims it rather than being refused by its own earlier claim; refusing therefore
needs both calls to fail, which happens exactly when the holder is the other endpoint. Exactly one
of two concurrent claimants can win, whatever the loader is doing with its own locks, and the loser
refuses. No core API was added: the function is already exported to modules and already used by
`mod_commands` and `mod_v8`.

| | `mod_h323.cpp` | `mod_opal.cpp` |
|---|---|---|
| Reservation name (`#define`) | `:162` `H323_PTLIB_RESERVATION` | `:109` `OPAL_PTLIB_RESERVATION` |
| Claim site (the CAS pair: claim, then owner re-claim) | `:221-222` | `:169-170` |
| Refusal ERROR naming the holder | `:225` | `:173` |
| `return SWITCH_STATUS_FALSE` on refusal | `:233` | `:182` |
| Release — load failure, no module interface | `:246` | `:199` |
| Release — load failure, no `FSProcess` | `:255` | `:205` |
| Released in `*_shutdown()` | **no — deliberately never released** (`:274`) | **no — deliberately never released** (`:222`) |

Both modules spell the reservation as the identical string `_fs_ptlib_endpoint_reservation`,
because the whole mechanism is that they contend for one entry in the core's global variable
table. The two release sites are the **only** ones, and both sit before `new FSProcess()`: they
exist because the core never calls a module's shutdown for a load that returned failure, so a
claim left on those edges would reserve a runtime that was never constructed and would refuse
every later retry. Each release passes the module's own name as the expected value, so it can
never free another module's claim.

**The claim is sticky past shutdown, and that is load-bearing.** This build defines
`HAVE_FAKE_DLCLOSE`, so `switch_dso.c:94` skips `dlclose()` and an unloaded module keeps its PTLib
runtime **mapped** for the life of the process. Residency, not registration, is what makes a second
`PProcess` fatal, so `mod_h323_shutdown()` and `mod_opal_shutdown()` deliberately leave the claim in
place: `load mod_h323; unload mod_h323; load mod_opal` would otherwise find an empty module hash —
nothing for stage 1 to see — and a released reservation, and walk straight into the crash. Every
edge *after* the construction therefore keeps the claim, and the variable is unset — `global_getvar`
answering `-ERR no reply` — only in a fresh process in which neither endpoint has been loaded yet.
Re-loading the **same** endpoint stays allowed, because it re-claims its own reservation and
because destroying and rebuilding one `FSProcess` on one PTLib runtime is safe; §7.3 records that
measured in both orders. The operator-facing consequences — how to read the variable, and why a
non-empty value with neither module loaded must not be cleared — are in
`README.response-format.md` §7.

**What that means for the extent of the edit, stated plainly.** Stage 2's release bookkeeping is
why **two** lines per module sit **outside** the "top of `switch_module_load`" region the refine
directive authorised — the two load-failure edges above. Nothing is added to `*_shutdown()`, the
function the directive froze; the sticky claim is precisely why that function is untouched. That
remains a real departure and is escalated as such (Project Guide §1.4 and H-3): reverting to stage 1
alone would reintroduce both the race described above and the post-unload crash, so the recommended
disposition is ratification of the widened carve-out rather than a narrower guard. Beyond the two
stages and those two release sites per module, the only other change in either file is a set of
pre-existing compiler-warning fixes unrelated to the guard — `mod_h323.cpp` at `:2171-2178` and
`:2249-2255` (a log format string that dropped its codec name, and two `uint32_t` fields printed
with `%lu`) and `mod_opal.cpp` at `:261-267` and `:388-392` (three string literals abutting an
identifier, which C++11 lexes as a user-defined literal). Those six sites — three per file — are
what make the build warning-clean in these files; they are itemised in Project Guide §3 and fall
under the same ratification. `mod_h323.h` and `mod_opal.h` are 0-diff.

**One description, not two.** The operator-facing account of the same guard lives in
`src/mod/xml_int/mod_xml_curl/README.response-format.md` §7, and it is the authoritative one for
anything an operator does with it — inspecting the reservation with
`fs_cli -x 'global_getvar _fs_ptlib_endpoint_reservation'`, the consequence of setting it by hand,
and what a refusal looks like from the outside. This section is the authoritative one for *why*
the guard has the shape it has. The two are cross-referenced rather than duplicated so they cannot
drift into disagreeing.

### 7.3 What was measured against the shipped binaries

`oos9-coload-evidence/guard-refusal-runtime-proof.txt` was re-captured against the binaries this
branch installs — its banner reads the shipped revision and version string, and its log anchors are
the shipped `mod_h323.cpp:190` / `mod_opal.cpp:137` (stage 1) and `mod_h323.cpp:225` /
`mod_opal.cpp:173` (stage 2). Five scenarios, one fresh disposable instance each:

1. **stage 1, order 1** — `mod_h323` loads, `mod_opal` is refused;
2. **stage 1, order 2** — `mod_opal` loads, `mod_h323` is refused;
3. **stage 2** — the reservation is planted under the sibling's name while the sibling is
   **absent from the module hash**, which is asserted in the capture, so the refusal cannot be
   attributed to stage 1; `mod_opal` is then refused by the CAS, naming the holder;
4. **sticky claim, order 1** — `mod_h323` loads, is **unloaded**, and with `module_exists` false
   for *both* endpoints the reservation still answers `mod_h323`; `mod_opal` is refused by stage 2
   naming that holder, and `mod_h323` then reloads and re-registers `endpoint,h323,mod_h323`;
5. **sticky claim, order 2** — the same with the roles swapped: the reservation still answers
   `mod_opal`, `mod_h323` is refused, `mod_opal` reloads and re-registers `endpoint,opal,mod_opal`.

In all five: every refused `load` answers `-ERR [module load file routine returned an error]`,
`module_exists` reports the refused module absent, the log carries the guard's `ERROR` plus the
core's `CRIT Error Loading module …`, the process is still alive afterwards, and `fs_cli -x status`
reports `UP … is ready`. The refused module's `switch_module_load` entry log line never appears,
because the guard returns ahead of it.

Each endpoint suite carries three covering cases, one per behaviour.
`coload_guard_refuses_when_sibling_is_loaded` (`test_mod_h323.cpp:3528`, `test_mod_opal.cpp:3680`)
asserts stage 1 through the module-load API against a sibling registered with
`switch_loadable_module_build_dynamic()`. `coload_reservation_refuses_a_reserved_ptlib_runtime`
(`test_mod_h323.cpp:3595`, `test_mod_opal.cpp:3747`) asserts stage 2 by planting the reservation
under the sibling's name while the sibling is *not* in the hash — the same distinguishing control
scenario 3 above uses, so the case cannot pass because of stage 1. And
`coload_reservation_outlives_the_unloaded_module` (`test_mod_h323.cpp:3458`,
`test_mod_opal.cpp:3610`) asserts the sticky claim in-process: shutdown leaves the reservation
naming this module, and the owner may re-claim it — the unit-level counterpart of scenarios 4 and 5.
All three indirections are deliberate and are explained in each suite: a binary that loaded the real
sibling would die of exactly the crash under discussion, which is why the end-to-end refusal lives
in the archived runtime proof instead.

**Stage 1 is requested work; stage 2 exceeds the authorised carve-out and is escalated.** The
refine directive authorises the guard under Outcome A and supersedes the plan's
zero-production-edit rule for these blocks — but it scoped the edit to the top of
`switch_module_load`, and §7.2 records exactly where stage 2 goes beyond that and why reverting it
would be worse than ratifying it.

## 8. Evidence index

| File | Contents |
|---|---|
| `oos9-coload-evidence/order1-h323-then-opal.gdb.txt` | Full gdb batch capture, order 1: stop reason, faulting frame, backtrace, `info sharedlibrary` |
| `oos9-coload-evidence/order1-h323-then-opal.freeswitch-log.txt` | Module load lines and the final 40 log lines of the crashed instance, order 1. Redacted: see the token-rotation record below |
| `oos9-coload-evidence/order2-opal-then-h323.gdb.txt` | Same capture, order 2 |
| `oos9-coload-evidence/order2-opal-then-h323.freeswitch-log.txt` | Same log extract, order 2. Redacted as above |
| `oos9-coload-evidence/staticinit-probe.c` | The `dlopen(RTLD_NOW\|RTLD_LOCAL)`-only probe |
| `oos9-coload-evidence/staticinit-probe.txt` | Its output for both orders |
| `oos9-coload-evidence/guard-refusal-runtime-proof.txt` | Runtime proof of the guard **as shipped**, re-captured against the installed binaries: five scenarios — stage 1 in both load orders, stage 2 reached with the sibling provably absent from the module hash, and the sticky claim across an unload in both orders (reservation still held, sibling still refused, owner-matched reload succeeds) — each with its four labelled observables |
| `oos9-coload-evidence/signalwire-token-rotation.txt` | The remediation record for the adoption token the two `*.freeswitch-log.txt` captures above originally carried in plaintext: what it was, the measured rotation, why rewriting history would not have remediated it, and the post-rotation sweep. Digests only, no secret |

Every instance started for this determination was disposable and was confirmed gone afterwards
(only pids captured at start were ever signalled); no shipped configuration under `conf/`, no
installed configuration under `/usr/local/freeswitch/conf`, and no tracked file was modified by the
experiment. The runtime proof ran against a private copy of the configuration on its own
event-socket port, so it also left a concurrently running instance owned by another clone alone.

## 9. Two gaps found while doing this, and how each was handled

| Gap | Handling |
|---|---|
| The refine directive says the backtrace is "archived under blitzy/". It was not — `blitzy/` held only `documentation/Project Guide.md` and untracked screenshots, and §9.8 of that guide is a prior *observation* of the symptom, not a backtrace. | Produced first-hand under gdb and archived here, in both load orders, with a corroborating static-initialisation probe. `gdb` itself was absent despite the guide's tool table claiming otherwise, and was installed for the determination. |
| The directive's Outcome-B branch names `conf/vanilla/autoload_configs/h323.conf.xml` as a shipped sample to annotate. That file does not exist in this tree; the only shipped H.323 sample is `src/mod/endpoints/mod_h323/h323.conf.xml`, while `opal.conf.xml` exists three times (`conf/curl`, `conf/insideout`, `conf/vanilla`). | Recorded rather than invented. It is moot for the delivered work: the determination selected Outcome A, so no conf sample is annotated and all four candidate files stay byte-identical to `323d52c88a`. |
