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
**unguarded** sources, so its line numbers are those of `323d52c88a`. The guard delivered
afterwards (§7) inserts 25 lines at the top of `mod_h323_load` and 26 at the top of
`mod_opal_load`, which shifts every subsequent line in those two files by the same
constant: `mod_h323.cpp:167` here is `:192` in the guarded tree, `mod_opal.cpp:116` is
`:142`, and the two entry log lines move from `:157` and `:104` to `:182` and `:130`. The
same constant applies to the `mod_h323.cpp:<line>` and `mod_opal.cpp:<line>` anchors cited
in the two endpoint suites' comments, which were deliberately left as they were rather
than mass-rewritten.

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

Outcome A was delivered: a mutual-exclusion guard is the **first** thing each load function does,
before `switch_loadable_module_create_module_interface` and before `new FSProcess()`:

- `mod_h323_load` refuses when `switch_loadable_module_exists("mod_opal") == SWITCH_STATUS_SUCCESS`
  (`src/mod/endpoints/mod_h323/mod_h323.cpp`);
- `mod_opal_load` refuses when `switch_loadable_module_exists("mod_h323") == SWITCH_STATUS_SUCCESS`
  (`src/mod/endpoints/mod_opal/mod_opal.cpp`).

Each logs `SWITCH_LOG_ERROR` naming the `PProcess` singleton conflict and both PTLib runtimes, then
returns `SWITCH_STATUS_FALSE`, which `switch_loadable_module_load_file()` reports as
`Error Loading module … **Module load routine returned an error**` while the process keeps running.
Everything else in `mod_h323.cpp`/`.h` and `mod_opal.cpp`/`.h` is byte-identical to `323d52c88a`.

Measured afterwards, in both load orders, in a fresh disposable instance per order
(`oos9-coload-evidence/guard-refusal-runtime-proof.txt`): the second `load` answers
`-ERR [module load file routine returned an error]` and `module_exists` reports the refused module
absent; the log carries the guard's `ERROR` line naming the conflict plus the core's `CRIT
Error Loading module …`; the process is still alive afterwards; and `fs_cli -x status` reports
`UP … is ready`. The `switch_module_load` entry log line of the refused module never appears,
because the guard returns ahead of it.

Each endpoint suite also carries one case — `coload_guard_refuses_when_sibling_is_loaded` — that
asserts the refusal through the module-load API against a sibling registered with
`switch_loadable_module_build_dynamic()`, so the negative branch is exercised without linking a
second PTLib into a test binary. That indirection is deliberate and is explained in each suite: a
binary that loaded the real sibling would die of exactly the crash under discussion, which is why
the end-to-end refusal lives in this archived runtime proof instead.

**This guard is requested work, not an AAP deviation.** The refine directive authorises exactly this
edit under Outcome A and supersedes the plan's zero-production-edit rule for these two blocks only.

## 8. Evidence index

| File | Contents |
|---|---|
| `oos9-coload-evidence/order1-h323-then-opal.gdb.txt` | Full gdb batch capture, order 1: stop reason, faulting frame, backtrace, `info sharedlibrary` |
| `oos9-coload-evidence/order1-h323-then-opal.freeswitch-log.txt` | Module load lines and the final 40 log lines of the crashed instance, order 1 |
| `oos9-coload-evidence/order2-opal-then-h323.gdb.txt` | Same capture, order 2 |
| `oos9-coload-evidence/order2-opal-then-h323.freeswitch-log.txt` | Same log extract, order 2 |
| `oos9-coload-evidence/staticinit-probe.c` | The `dlopen(RTLD_NOW\|RTLD_LOCAL)`-only probe |
| `oos9-coload-evidence/staticinit-probe.txt` | Its output for both orders |
| `oos9-coload-evidence/guard-refusal-runtime-proof.txt` | Post-guard runtime proof: refusal, `ERROR` log line, process survival, `fs_cli -x status` UP |

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
