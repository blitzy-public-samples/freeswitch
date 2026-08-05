#!/usr/bin/env bash

### shfmt -w -s -ci -sr -kp -fn ci.sh

#------------------------------------------------------------------------------
# CI Script
# Helper script for running CI jobs
#------------------------------------------------------------------------------

# Function to display usage information
display_usage()
{
	echo "Usage: $0 -t <type> -a <action> -c <code> -p <path>"
	echo "Options:"
	echo "  -t  Type (unit-test, scan-build)"
	echo "  -a  Action (configure, build, install, validate)"
	echo "  -c  Code (sofia-sip, freeswitch)"
	echo "  -p  Path to code"
	echo "Modes:"
	echo "  --guard-self-test  Assert both capability guards and the unconditional"
	echo "                     mod_xml_curl arm against a scratch module list, then exit"
	exit 1
}

# Decide ONCE whether this file is being executed or merely sourced.
#
# Two places in this file ACT rather than define - the argument parsing immediately
# below and the job dispatcher at the end - and neither may run in a shell that only
# wanted the functions.  "$0" is this file's path when it is executed and the invoking
# shell's name when it is sourced, so the two are compared rather than trusting a
# caller-supplied flag.  One determination instead of one per site, because two copies
# of the same test are two things that can come to disagree.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	CI_SH_EXECUTED=true
else
	CI_SH_EXECUTED=false
fi

# Parse the command line - but only when this file is being EXECUTED.
#
# A sourcing caller keeps its own arguments.  build/provision_endpoint_toolkits.sh is
# invoked with long options of its own, so handing "$@" to the getopts loop below would
# read the caller's "--uninstall-check" as the unknown option "-", take the "?)" arm and
# call display_usage() - which ends in exit, terminating the CALLER rather than a
# subshell.  Skipping the whole block also leaves the caller's positional parameters,
# OPTIND and TYPE/ACTION/CODE/PATH_TO_CODE exactly as they were, so sourcing this file
# has no effect beyond defining its functions.  Nothing an executed run does changes:
# the block below is the original parsing, one indent level deeper.
if [ "$CI_SH_EXECUTED" = true ]; then
	# Claim the long-option modes before getopts can reject them.
	#
	# getopts understands short options only: it reads "--guard-self-test" as the unknown
	# option "-", falls into the "?)" arm below and exits 1 through display_usage().  The
	# mode is therefore recognised here and REMOVED from the positional parameters, which
	# leaves every existing invocation - all of them short-option only, as in
	# .github/workflows/unit-test.yml - parsed exactly as it is today, "-h" and an unknown
	# option included.  Recognition is separated from execution because the mode's function
	# is defined further down the file and would not yet exist at this point.
	GUARD_SELF_TEST=false
	guard_self_test_argv=()

	for guard_self_test_arg in "$@"; do
		case "$guard_self_test_arg" in
			--guard-self-test) GUARD_SELF_TEST=true ;;
			*) guard_self_test_argv+=("$guard_self_test_arg") ;;
		esac
	done

	set -- "${guard_self_test_argv[@]}"

	# Parse command line arguments
	while getopts "t:p:a:c:h" opt; do
		case $opt in
			t) TYPE="$OPTARG" ;;
			a) ACTION="$OPTARG" ;;
			c) CODE="$OPTARG" ;;
			p) PATH_TO_CODE="$OPTARG" ;;
			h) display_usage ;;
			?) display_usage ;;
		esac
	done
fi

# Function to handle sofia-sip configuration
configure_sofia_sip()
{
	./autogen.sh && ./configure.gnu || exit 1
}

# Function to handle sofia-sip build
build_sofia_sip()
{
	make -j$(nproc) || exit 1
}

# Function to handle sofia-sip installation
install_sofia_sip()
{
	make install || exit 1
}

# Function to handle sofia-sip validation
validate_sofia_sip()
{
	exit 0
}

# Probe the COMPLETE mod_h323 toolkit, so the optional module stays disabled unless
# it can actually be built.
#
# PTLib alone is not that capability: mod_h323 also needs the OpenH323/H323Plus
# headers and -lopenh323, and OPAL installs a ptlib.pc of its own.  The probe
# therefore mirrors the module's own compile and link inputs instead of standing in
# for them, and it builds in a temporary directory outside the tree.  Any failure -
# compiler, header, library or mktemp - leaves mod_h323 disabled.
h323_toolkit_available()
{
	local -a compiler
	local -a flags=(-I/usr/include/openh323 -DPTRACING=1 -D_REENTRANT -fno-exceptions)
	local probe_dir
	local status=1
	local libdir
	local linked

	pkg-config --exists ptlib || return 1

	# Match configure.ac's IS64BITLINUX conditional; a probe built with flags the
	# module does not use proves nothing
	if [ "$(uname -m)" = "x86_64" ]; then
		flags+=(-DP_64BIT)
	fi

	read -ra compiler <<< "${CXX:-g++}"

	probe_dir=$(mktemp -d) || return 1

	# ptlib.h has to come first, exactly as mod_h323.h includes the two headers
	printf '#include <ptlib.h>\n#include <h323.h>\nint main(void) { return 0; }\n' > "$probe_dir/probe.cpp"

	if "${compiler[@]}" "${flags[@]}" "$probe_dir/probe.cpp" -o "$probe_dir/probe" \
		-L/usr/lib -lopenh323 -lpt -lrt > /dev/null 2>&1; then
		status=0

		# CVE-2013-1864: PTLib's PXML parser expanded internal entities with no
		# ceiling before 2.10.10, so a "billion laughs" document makes any consumer
		# allocate until it is killed.  The check below is behavioural instead of a
		# version comparison, because a version comparison is wrong in both
		# directions here: distributions and vendors backport the fix without
		# renaming the library, and a .pc file can advertise a version that is not
		# the one the loader resolves.  Asking the library to honour a deliberately
		# tiny entity ceiling answers the only question that matters, and answers it
		# with a benign document rather than a hostile one.
		libdir=$(pkg-config --variable=libdir ptlib 2> /dev/null)

		cat > "$probe_dir/entity.cpp" <<- 'PROBE'
			#include <ptlib.h>
			#include <ptclib/pxml.h>
			int main(void)
			{
			  PXML xml;
			  xml.SetMaxEntityLength(8);
			  /* Four expansions of a ten character entity are far past a ceiling of
			     eight, so a library that bounds expansion has to refuse this document.
			     One with no ceiling parses it and reports success, which is the
			     vulnerable behaviour. */
			  return xml.Load(PString("<?xml version='1.0'?><!DOCTYPE d [<!ENTITY e '0123456789'>]><d>&e;&e;&e;&e;</d>")) ? 1 : 0;
			}
		PROBE

		if ! "${compiler[@]}" "${flags[@]}" "$probe_dir/entity.cpp" -o "$probe_dir/entity" \
			-L/usr/lib -lpt -lrt > /dev/null 2>&1; then
			echo "ci.sh: the PTLib being linked exposes no XML entity ceiling, so CVE-2013-1864 cannot be ruled out; endpoints/mod_h323 stays disabled" >&2
			status=1
		elif ! LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 60 "$probe_dir/entity" > /dev/null 2>&1; then
			echo "ci.sh: the PTLib being linked ignores its XML entity ceiling (CVE-2013-1864); endpoints/mod_h323 stays disabled" >&2
			status=1
		fi

		# The advisory is about the library that is actually loaded, so record which
		# libpt this link resolved and refuse a linkage that cannot be established:
		# an unverifiable linkage cannot be cleared of the advisory either.
		linked=$(LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$probe_dir/probe" 2> /dev/null |
			sed -n 's|^[[:space:]]*libpt\.so[^[:space:]]*[[:space:]]*=>[[:space:]]*\(/[^[:space:]]*\).*|\1|p')

		if [ -z "$linked" ]; then
			echo "ci.sh: cannot establish which libpt endpoints/mod_h323 would load; it stays disabled" >&2
			status=1
		elif [ "$status" = 0 ]; then
			echo "ci.sh: endpoints/mod_h323 will link $linked (ptlib $(pkg-config --modversion ptlib 2> /dev/null)), which bounds XML entity expansion"
		fi
	fi

	rm -rf "$probe_dir"

	return "$status"
}

# Print how many ACTIVE - uncommented - lines the generated modules.conf carries for
# one module path, or fail if the list cannot be read.
#
# The expression is anchored on the whole line so a module path only ever matches its
# own entry, never a longer one that contains it.  A count rather than a yes/no
# verdict, because the only acceptable answer for a module that has to be built is
# exactly one: zero means it was never activated, more than one means a duplicate
# whose effective state this script cannot reason about.
#
# A failed READ is never rendered as a count.  "The list could not be read" and "the
# module is not listed" are different facts, and only the second is safe to act on.
modules_conf_active_count()
{
	local module="$1"
	local count
	local status

	if [ ! -f modules.conf ]; then
		echo "Error: modules.conf is missing, cannot determine whether '$module' is enabled" >&2
		return 1
	fi

	# grep's status is the only thing separating "nothing matched" (1, a legitimate
	# zero) from "the read never completed" (above 1), so it is captured rather than
	# discarded: require_module_disabled_for_tests() treats a count of zero as PROOF
	# that a module will not be built.
	count=$(grep -c -E "^[[:space:]]*${module}[[:space:]]*$" modules.conf)
	status=$?

	if [ "$status" -gt 1 ]; then
		echo "Error: could not read modules.conf (grep exited $status), cannot determine whether '$module' is enabled" >&2
		return 1
	fi

	if [ "$status" -eq 1 ]; then
		count=0
	fi

	# Both callers compare this against a literal, so a non-numeric result is a
	# meaningless comparison and is treated as a failed read
	case "$count" in
		'' | *[![:digit:]]*)
			echo "Error: modules.conf produced an unusable active-line count '$count' for '$module'" >&2
			return 1
			;;
	esac

	echo "$count"

	return 0
}

# Activate one module in the generated modules.conf and verify the activation.
#
# Verification is the point: sed exits 0 whether or not its address matched, and a
# module that failed to activate contributes nothing to `print_tests' and nothing to
# `check' - src/mod/Makefile.am wraps each module's entire recipe in a test on
# CONF_MODULES, which configure.ac derives by stripping comments from modules.conf.
# The failure mode is a green build that silently ran none of that module's tests, so
# the edit is followed by a postcondition and the caller aborts when it does not hold.
#
# The substitution is anchored on the whole line, so it cannot disturb a neighbouring
# entry, and it is idempotent - an already-active line does not match the address.
enable_module_for_tests()
{
	local module="$1"
	local active

	sed -i -e "s%^[[:space:]]*#[[:space:]]*${module}[[:space:]]*$%${module}%" modules.conf || return 1

	active=$(modules_conf_active_count "$module") || return 1

	if [ "$active" != "1" ]; then
		echo "Error: expected exactly 1 active '$module' line in modules.conf, found ${active:-0}" >&2
		return 1
	fi

	return 0
}

# Verify that a module whose capability probe failed will NOT be built.
#
# An endpoint module that is active while its toolkit is missing does not degrade, it
# fails the build - the one thing the capability guards exist to prevent - so the
# absent-toolkit outcome is asserted rather than assumed.
require_module_disabled_for_tests()
{
	local module="$1"
	local active

	active=$(modules_conf_active_count "$module") || return 1

	if [ "$active" != "0" ]; then
		echo "Error: '$module' is active in modules.conf but its toolkit capability probe failed; leave the entry commented out" >&2
		return 1
	fi

	return 0
}

# Function to handle freeswitch configuration
configure_freeswitch()
{
	local type="$1"

	./bootstrap.sh -j || exit 1

	case "$type" in
		"unit-test")
			echo 'codecs/mod_openh264' >> modules.conf
			sed -i \
				-e '/applications\/mod_http_cache/s/^#//g' \
				-e '/formats\/mod_opusfile/s/^#//g' \
				-e '/languages\/mod_lua/s/^#//g' \
				modules.conf

			# mod_xml_curl carries a module-local test suite and depends on no
			# optional toolkit, so it is enabled unconditionally
			enable_module_for_tests 'xml_int/mod_xml_curl' || exit 1

			# Enable the optional endpoint modules only when their toolkit can build
			# them, so an absent, incomplete or too-old H.323/OPAL toolkit leaves them
			# disabled rather than failing the build.  OPAL is gated on the version its
			# own header demands (mod_opal.h #errors below 3.12.8).
			#
			# Both outcomes of each probe are asserted - a passing probe must leave the
			# module active, a failing probe must leave it commented out - which is why
			# these are if/else blocks rather than `probe && sed' compounds.
			if pkg-config --atleast-version=3.12.8 opal; then
				enable_module_for_tests 'endpoints/mod_opal' || exit 1
			else
				require_module_disabled_for_tests 'endpoints/mod_opal' || exit 1
			fi

			if h323_toolkit_available; then
				enable_module_for_tests 'endpoints/mod_h323' || exit 1
			else
				require_module_disabled_for_tests 'endpoints/mod_h323' || exit 1
			fi

			export ASAN_OPTIONS=log_path=stdout:disable_coredump=0:unmap_shadow_on_exit=1:fast_unwind_on_malloc=0

			./configure \
				--enable-address-sanitizer \
				--enable-fake-dlclose ||
				exit 1

			;;
		"scan-build")
			cp build/modules.conf.most modules.conf

			# "Enable"/"Uncomment" mods
			echo 'codecs/mod_openh264' >> modules.conf
			sed -i \
				-e '/mod_mariadb/s/^#//g' \
				-e '/mod_v8/s/^#//g' \
				modules.conf

			# "Disable"/"Comment out" mods
			sed -i \
				-e '/mod_ilbc/s/^/#/g' \
				-e '/mod_mongo/s/^/#/g' \
				-e '/mod_pocketsphinx/s/^/#/g' \
				-e '/mod_siren/s/^/#/g' \
				-e '/mod_avmd/s/^/#/g' \
				-e '/mod_basic/s/^/#/g' \
				-e '/mod_cv/s/^/#/g' \
				-e '/mod_erlang_event/s/^/#/g' \
				-e '/mod_perl/s/^/#/g' \
				-e '/mod_rtmp/s/^/#/g' \
				-e '/mod_unimrcp/s/^/#/g' \
				-e '/mod_xml_rpc/s/^/#/g' \
				modules.conf

			./configure || exit 1

			;;
		*)
			exit 1
			;;
	esac
}

# Function to handle freeswitch build
build_freeswitch()
{
	local type="$1"

	set -o pipefail

	case "$type" in
		"unit-test")
			make --no-keep-going -j$(nproc --all) |& tee ./unit-tests-build-result.txt
			build_status=${PIPESTATUS[0]}
			if [[ $build_status != "0" ]]; then
				exit $build_status
			fi

			;;
		"scan-build")
			if ! command -v scan-build-14 > /dev/null 2>&1; then
				echo "Error: scan-build-14 command not found. Please ensure clang static analyzer is installed." >&2
				exit 1
			fi

			mkdir -p scan-build

			scan-build-14 \
				--force-analyze-debug-code \
				--status-bugs \
				-o ./scan-build/ \
				make --no-keep-going -j$(nproc --all) |& tee ./scan-build-result.txt
			build_status=${PIPESTATUS[0]}

			if ! grep -siq "scan-build: No bugs found" ./scan-build-result.txt; then
				echo "scan-build: bugs found!"
				exit 1
			fi

			if [[ $build_status != "0" ]]; then
				echo "scan-build: compilation failed!"
				exit $build_status
			fi

			;;
		*)
			exit 1
			;;
	esac
}

# Function to handle freeswitch installation
install_freeswitch()
{
	make install || exit 1
}

# Function to handle freeswitch validation
validate_freeswitch()
{
	local type="$1"

	case "$type" in
		"unit-test")
			exit 0
			;;
		"scan-build")
			REPORT_PATH=$(find scan-build* -mindepth 1 -type d)
			if [ -n "$REPORT_PATH" ]; then
				echo "Found analysis report at: $REPORT_PATH"

				if command -v html2text > /dev/null 2>&1; then
					echo "Report contents:"
					html2text "$REPORT_PATH"/*.html || true
				fi

				echo "Number of issues found:"
				grep -c "<!--BUGDESC" "$REPORT_PATH"/*.html || true

				exit $([ -d "$REPORT_PATH" ])
			else
				echo "No analysis report found"
				exit 0
			fi

			;;
		*)
			exit 1
			;;
	esac
}

#------------------------------------------------------------------------------
# --guard-self-test: exercise BOTH branches of both capability guards on any host
#------------------------------------------------------------------------------
#
# The guards above are fail-closed, and on a host that has the H.323 and OPAL toolkits
# installed only their POSITIVE branch ever runs.  The branch that has never run is the
# one that matters: a toolkit that cannot build a module has to leave that module
# commented out, because an active module whose toolkit is missing does not degrade - it
# fails the build - while a module that is silently INACTIVE contributes no tests at all
# (src/mod/Makefile.am wraps each module's entire recipe in a test on CONF_MODULES,
# which configure.ac derives by stripping comments from modules.conf).  Both failure
# modes look green from the outside, so neither is caught by running the suite.
#
# The negative branch is reached by ISOLATING THE ENVIRONMENT ONLY - a PATH-shadowed
# compiler wrapper that cannot compile anything, and PKG_CONFIG_LIBDIR pointed at an
# empty directory - inside a subshell, so no toolkit is uninstalled, no .pc file is
# edited and no compiler is replaced.  Every module-list edit lands on a scratch copy of
# the module-list template inside a temporary directory, so build/modules.conf.in - the
# default-build contract - and the working tree's generated modules.conf are never
# touched.  What is asserted is not new behaviour but the postconditions this script
# already declares: exactly one active line after an enablement, zero after a refusal.

# The module-list template every arm copies.  It is read and never written: all three
# modules the guards decide about ship commented out in it, which is exactly the
# starting state each arm needs, and that commented state is the default-build contract.
GUARD_SELF_TEST_TEMPLATE='build/modules.conf.in'

# Print one arm's verdict in a fixed, greppable shape.
#
# A CI log interleaves these lines with the guards' own diagnostics, so the verdict
# carries the arm's name and what it asserted rather than a bare PASS - otherwise a
# failure has to be reconstructed from whatever happened to be printed above it.
guard_self_test_report()
{
	local status="$1"
	local arm="$2"
	local asserted="$3"

	if [ "$status" -eq 0 ]; then
		echo "PASS: $arm - $asserted"
	else
		echo "FAIL: $arm - $asserted" >&2
	fi

	return "$status"
}

# Give one arm its own scratch module list, copied from the template.
#
# Per-arm copies rather than one shared list: an arm that inherited another arm's
# uncommented entries would be asserting against a state no build ever starts from, and
# the byte-identity checks below would be comparing the wrong pair of files.  An
# unreadable template is a failure, never an empty list that trivially satisfies every
# "this module is disabled" assertion.
guard_self_test_scratch_list()
{
	local dir="$1"

	if [ ! -r "$GUARD_SELF_TEST_TEMPLATE" ]; then
		echo "Error: cannot read $GUARD_SELF_TEST_TEMPLATE, so the guard self-test has no module list to work on" >&2
		return 1
	fi

	mkdir -p "$dir" || return 1
	cp "$GUARD_SELF_TEST_TEMPLATE" "$dir/modules.conf" || return 1

	return 0
}

# Checksum one file, refusing to render a failed read as a digest.
#
# The byte-identity assertions below are the only proof that a refused module was left
# strictly alone rather than edited and then edited back - both leave the same grep
# result - so a checksum that could not be taken must never be allowed to compare equal
# to anything.
guard_self_test_checksum()
{
	local file="$1"
	local digest

	digest=$(sha256sum "$file" 2> /dev/null | awk '{ print $1 }')

	if [ "${#digest}" -ne 64 ]; then
		echo "Error: could not checksum '$file', so the module list cannot be proven unchanged" >&2
		return 1
	fi

	case "$digest" in
		*[!0-9a-f]*)
			echo "Error: '$file' produced an unusable sha256 digest '$digest'" >&2
			return 1
			;;
	esac

	printf '%s\n' "$digest"

	return 0
}

# Assert that enable_module_for_tests() REFUSES a module list its exactly-one-active-line
# postcondition cannot hold for, and says why.
#
# That postcondition is the whole reason the helper exists - sed exits 0 whether or not
# its address matched, so a failed activation is otherwise indistinguishable from a
# successful one - and a guard rail is only proven by the load it turns away.  A silent
# refusal is not accepted either: the diagnostic naming the observed count is what
# separates "never activated" from "activated twice" in a CI log.
guard_self_test_expect_enable_refusal()
{
	local dir="$1"
	local module="$2"
	local expected="$3"
	local log="$dir/refusal.log"
	local status

	(
		cd "$dir" || exit 1

		enable_module_for_tests "$module"
	) 2> "$log"
	status=$?

	if [ "$status" -eq 0 ]; then
		echo "Error: enable_module_for_tests accepted a module list carrying $expected active '$module' lines" >&2
		return 1
	fi

	if ! grep -qF "expected exactly 1 active '$module' line in modules.conf, found $expected" "$log"; then
		echo "Error: enable_module_for_tests refused '$module' without reporting its exactly-one-active-line postcondition" >&2
		cat "$log" >&2
		return 1
	fi

	return 0
}

# Arm 1: the mod_h323 guard's NEGATIVE branch, reached by making its compile+link probe
# fail.
#
# h323_toolkit_available() resolves its compiler through the PATH, so a wrapper placed
# first on the PATH under exactly the name the probe invokes turns that probe's compile
# and link step into a guaranteed failure - which is what an H.323 toolkit too broken or
# too incomplete to build the module looks like from the probe's point of view.  The real
# compiler, the real headers and the real libraries are untouched: the shadow lives in
# one subshell and dies with it.
#
# The assertions are the ones the unit-test arm makes for a failed probe: the guard
# refuses, the module is never activated, and require_module_disabled_for_tests() finds
# zero active lines.  The module list is then checksummed against the pristine template,
# because "left commented out" and "uncommented and then commented again" produce the
# same grep result but are not the same edit.
guard_self_test_h323_negative()
{
	local dir="$1/h323-negative"
	local bin="$dir/shadow-bin"
	local -a compiler
	local wrapper
	local template
	local after

	guard_self_test_scratch_list "$dir" || return 1
	mkdir -p "$bin" || return 1

	# Read the compiler exactly as the probe does, so the wrapper carries the name the
	# probe will actually look up rather than an assumed g++.  A CXX that names a path
	# bypasses the PATH altogether, so its directory part is dropped for the duration of
	# the arm - still nothing but this subshell's environment.
	read -ra compiler <<< "${CXX:-g++}"

	if [ "${#compiler[@]}" -eq 0 ]; then
		echo "Error: CXX names no compiler, so the H.323 compile+link probe cannot be shadowed" >&2
		return 1
	fi

	wrapper="${compiler[0]##*/}"
	compiler[0]="$wrapper"

	printf '#!/usr/bin/env bash\nexit 1\n' > "$bin/$wrapper" || return 1
	chmod +x "$bin/$wrapper" || return 1

	template=$(guard_self_test_checksum "$GUARD_SELF_TEST_TEMPLATE") || return 1

	(
		cd "$dir" || exit 1

		PATH="$bin:$PATH"
		CXX="${compiler[*]}"
		export PATH CXX

		if h323_toolkit_available; then
			echo "Error: h323_toolkit_available reported the H.323 toolkit usable while its probe could not compile anything" >&2
			exit 1
		fi

		require_module_disabled_for_tests 'endpoints/mod_h323' || exit 1
	) || return 1

	after=$(guard_self_test_checksum "$dir/modules.conf") || return 1

	if [ "$template" != "$after" ]; then
		echo "Error: the refused endpoints/mod_h323 guard changed the module list (sha256 $template -> $after)" >&2
		return 1
	fi

	return 0
}

# Arm 2: the mod_opal guard's NEGATIVE branch, reached by taking away pkg-config's
# ability to resolve anything.
#
# The unit-test arm gates mod_opal on the OPAL version its own header demands, and that
# gate is a pkg-config query, so a pkg-config that cannot find a .pc file is the faithful
# simulation of "no usable OPAL toolkit here".  PKG_CONFIG_LIBDIR REPLACES pkg-config's
# built-in search path, so pointing it at an empty directory does exactly that;
# PKG_CONFIG_PATH is cleared as well because it is searched IN ADDITION to that path and
# would otherwise let opal.pc back in.  Both changes live in one subshell - the installed
# .pc files are neither moved nor edited - and arm 4 pins the unit-test arm to this exact
# gate expression, so the simulation cannot drift away from the guard it exercises.
guard_self_test_opal_negative()
{
	local dir="$1/opal-negative"
	local pkgdir="$dir/empty-pkgconfig"
	local template
	local after

	guard_self_test_scratch_list "$dir" || return 1
	mkdir -p "$pkgdir" || return 1

	template=$(guard_self_test_checksum "$GUARD_SELF_TEST_TEMPLATE") || return 1

	(
		cd "$dir" || exit 1

		PKG_CONFIG_LIBDIR="$pkgdir"
		export PKG_CONFIG_LIBDIR
		unset PKG_CONFIG_PATH

		if pkg-config --atleast-version=3.12.8 opal; then
			echo "Error: the OPAL version gate resolved opal against an empty pkg-config search path" >&2
			exit 1
		fi

		require_module_disabled_for_tests 'endpoints/mod_opal' || exit 1
	) || return 1

	after=$(guard_self_test_checksum "$dir/modules.conf") || return 1

	if [ "$template" != "$after" ]; then
		echo "Error: the refused endpoints/mod_opal guard changed the module list (sha256 $template -> $after)" >&2
		return 1
	fi

	return 0
}

# Arm 3: both guards' POSITIVE branch on this host, with no environment manipulation at
# all.
#
# The negative arms prove that a refusal is respected; this one proves the guards still
# say yes when the toolkit really is installed, and that the enablement they then perform
# satisfies its own postcondition - exactly one active line per endpoint, because zero
# means the module was never activated and more than one is a duplicate whose effective
# state this script cannot reason about.  h323_toolkit_available() is called whole, so the
# CVE-2013-1864 behavioural probe runs inside this arm rather than being reimplemented
# beside it.
#
# The verdict on the H.323 side belongs to the host, not to this arm: the advisory probe
# can only clear a PTLib that exposes an XML entity ceiling, and on a host whose PTLib
# does not, the guard refuses.  The arm then asserts the OTHER postcondition the unit-test
# arm declares for that case - fail-closed, zero active lines - and additionally that the
# refusal was DIAGNOSED, because a guard that goes quiet has stopped being a guard.
guard_self_test_guards_positive()
{
	local dir="$1/guards-positive"
	local enablement="$1/guards-positive-enablement"
	local log
	local active
	local module

	guard_self_test_scratch_list "$dir" || return 1
	guard_self_test_scratch_list "$enablement" || return 1

	log="$dir/h323-guard.log"

	if ! pkg-config --atleast-version=3.12.8 opal; then
		echo "Error: this host does not satisfy the OPAL version gate, so the positive branch of the mod_opal guard cannot be asserted here" >&2
		return 1
	fi

	if ! pkg-config --exists ptlib; then
		echo "Error: this host has no ptlib.pc, so the positive branch of the mod_h323 guard cannot be asserted here" >&2
		return 1
	fi

	(
		cd "$dir" || exit 1

		enable_module_for_tests 'endpoints/mod_opal' || exit 1

		if h323_toolkit_available 2> "$log"; then
			enable_module_for_tests 'endpoints/mod_h323' || exit 1

			echo "ci.sh --guard-self-test: the mod_h323 guard cleared this host, CVE-2013-1864 advisory probe included"
		else
			require_module_disabled_for_tests 'endpoints/mod_h323' || exit 1

			if ! grep -q '^ci\.sh: ' "$log"; then
				echo "Error: h323_toolkit_available refused the H.323 toolkit without saying why, so the refusal cannot be attributed" >&2
				exit 1
			fi

			cat "$log" >&2
			echo "ci.sh --guard-self-test: NOTE - the mod_h323 guard refuses this host for the reason above, so its fail-closed branch is the branch that runs here and endpoints/mod_h323 is left out of the build; that outcome is asserted, not tolerated" >&2
		fi
	) || return 1

	# The enablement postcondition itself, for BOTH endpoints and independently of the
	# host's verdict: either guard's positive branch is only correct if the enablement it
	# performs leaves exactly one active line, so that is asserted on its own scratch list
	# rather than inferred from whichever branch this particular host took.
	(
		cd "$enablement" || exit 1

		for module in 'endpoints/mod_opal' 'endpoints/mod_h323'; do
			enable_module_for_tests "$module" || exit 1

			active=$(modules_conf_active_count "$module") || exit 1

			if [ "$active" != "1" ]; then
				echo "Error: expected exactly 1 active '$module' line after enablement, found ${active:-0}" >&2
				exit 1
			fi
		done
	) || return 1

	return 0
}

# Assert the SHAPE of configure_freeswitch()'s unit-test arm: mod_xml_curl enabled with no
# capability gate at all, each endpoint enabled only inside one.
#
# The functional arms cannot see this.  They prove the guards behave correctly, not that
# mod_xml_curl - which depends on no optional toolkit and must therefore never be gated -
# has stayed outside both gates, and not that the two gate expressions still read the way
# arms 1 to 3 assume when they simulate them.  Both would regress in silence: a gated
# mod_xml_curl simply stops contributing its suite, and a rewritten gate leaves the
# simulations exercising something that no longer exists.
#
# Nesting is read from the arm's own if/fi structure rather than from indentation, so the
# check does not depend on how the file is formatted, and the if/fi count has to balance -
# an unbalanced parse is a failed read, not a pass.
guard_self_test_unit_test_arm_shape()
{
	local script="${BASH_SOURCE[0]}"

	if [ ! -r "$script" ]; then
		echo "Error: cannot read '$script', so the shape of the unit-test arm cannot be checked" >&2
		return 1
	fi

	awk '
		BEGIN {
			quote = sprintf("%c", 39)
			xml_curl_call = "enable_module_for_tests " quote "xml_int/mod_xml_curl" quote " || exit 1"
			opal_call = "enable_module_for_tests " quote "endpoints/mod_opal" quote " || exit 1"
			h323_call = "enable_module_for_tests " quote "endpoints/mod_h323" quote " || exit 1"
			opal_gate = "if pkg-config --atleast-version=3.12.8 opal; then"
			h323_gate = "if h323_toolkit_available; then"
		}
		{
			line = $0
			sub(/^\t+/, "", line)
		}
		# build_freeswitch() and validate_freeswitch() carry a "unit-test") label of
		# their own, so the search is confined to configure_freeswitch() - the function
		# that actually decides which modules get built - rather than keyed on the first
		# matching label in the file
		$0 == "configure_freeswitch()" {
			fn = 1
			found = 1
			next
		}
		fn && $0 == "}" {
			fn = 0
			next
		}
		fn && !seen && line == "\"unit-test\")" {
			seen = 1
			arm = 1
			next
		}
		arm && line == ";;" {
			arm = 0
			closed = 1
			balance = depth
			next
		}
		arm {
			# Comments carry prose, not structure, and prose contains words like "if"
			if (line ~ /^#/) {
				next
			}

			# Count the if/fi KEYWORDS on the line rather than assuming one construct per
			# line, because shfmt preserves a one-line "if x; then y; fi" and a scan that
			# counted lines would read that as a block left open
			tokens = split(line, token, /[ \t;&|()]+/)

			for (i = 1; i <= tokens; i++) {
				if (token[i] == "if") {
					depth++
				} else if (token[i] == "fi") {
					depth--
				}
			}

			if (line == xml_curl_call) {
				xml_curl_seen++
				xml_curl_depth = depth
			} else if (line == opal_call) {
				opal_seen++
				opal_depth = depth
			} else if (line == h323_call) {
				h323_seen++
				h323_depth = depth
			} else if (line == opal_gate) {
				opal_gate_seen++
			} else if (line == h323_gate) {
				h323_gate_seen++
			}
		}
		END {
			problems = 0

			if (!found) {
				print "Error: could not find configure_freeswitch() in " FILENAME ", so its unit-test arm cannot be checked" > "/dev/stderr"
				problems++
			}

			if (!closed) {
				print "Error: could not find the unit-test arm of configure_freeswitch() in " FILENAME > "/dev/stderr"
				problems++
			} else if (balance != 0) {
				print "Error: the if/fi keywords in the unit-test arm do not balance (net " balance "), so its shape cannot be read" > "/dev/stderr"
				problems++
			}

			if (xml_curl_seen != 1) {
				print "Error: expected exactly 1 xml_int/mod_xml_curl enablement in the unit-test arm, found " xml_curl_seen + 0 > "/dev/stderr"
				problems++
			} else if (xml_curl_depth != 0) {
				print "Error: xml_int/mod_xml_curl is enabled inside a conditional (if depth " xml_curl_depth "); it depends on no optional toolkit and must never be gated" > "/dev/stderr"
				problems++
			}

			if (opal_gate_seen != 1 || h323_gate_seen != 1) {
				print "Error: the unit-test arm no longer carries exactly one OPAL version gate and one H.323 toolkit gate (found " opal_gate_seen + 0 " and " h323_gate_seen + 0 ")" > "/dev/stderr"
				problems++
			}

			if (opal_seen != 1 || opal_depth + 0 < 1) {
				print "Error: endpoints/mod_opal must be enabled exactly once and only inside its capability gate (seen " opal_seen + 0 " times at if depth " opal_depth + 0 ")" > "/dev/stderr"
				problems++
			}

			if (h323_seen != 1 || h323_depth + 0 < 1) {
				print "Error: endpoints/mod_h323 must be enabled exactly once and only inside its capability gate (seen " h323_seen + 0 " times at if depth " h323_depth + 0 ")" > "/dev/stderr"
				problems++
			}

			if (problems > 0) {
				exit 1
			}
		}
	' "$script" || return 1

	return 0
}

# Arm 4: the mod_xml_curl arm is unconditional, and its postcondition really does fire.
#
# mod_xml_curl needs no optional toolkit, so its enablement carries no gate - which makes
# it the one module whose suite must always be collected, and the one enablement whose
# only protection is enable_module_for_tests()' postcondition.  This arm therefore proves
# three separate things: the shape of the arm, that an ordinary activation leaves exactly
# one active line and stays at one when repeated, and - on deliberately corrupted scratch
# copies - that the postcondition REFUSES a list it cannot hold for instead of waving it
# through.  The corruption never leaves the scratch directory.
guard_self_test_xml_curl_unconditional()
{
	local dir="$1/xml-curl-unconditional"
	local duplicated="$1/xml-curl-duplicated"
	local absent="$1/xml-curl-absent"
	local active

	guard_self_test_unit_test_arm_shape || return 1
	guard_self_test_scratch_list "$dir" || return 1

	(
		cd "$dir" || exit 1

		enable_module_for_tests 'xml_int/mod_xml_curl' || exit 1

		active=$(modules_conf_active_count 'xml_int/mod_xml_curl') || exit 1

		if [ "$active" != "1" ]; then
			echo "Error: expected exactly 1 active 'xml_int/mod_xml_curl' line after enablement, found ${active:-0}" >&2
			exit 1
		fi

		# Idempotent by construction: the substitution is anchored on a COMMENTED line, so
		# an already-active entry does not match it and no second entry can appear
		enable_module_for_tests 'xml_int/mod_xml_curl' || exit 1

		active=$(modules_conf_active_count 'xml_int/mod_xml_curl') || exit 1

		if [ "$active" != "1" ]; then
			echo "Error: enabling 'xml_int/mod_xml_curl' twice left ${active:-0} active lines, so the operation is not idempotent" >&2
			exit 1
		fi
	) || return 1

	# A list that already carries the entry twice: the substitution matches nothing, and
	# the postcondition has to reject the duplicate rather than report a successful
	# enablement
	mkdir -p "$duplicated" || return 1
	grep -v '^[[:space:]]*#[[:space:]]*xml_int/mod_xml_curl[[:space:]]*$' "$GUARD_SELF_TEST_TEMPLATE" > "$duplicated/modules.conf" || return 1
	printf '%s\n%s\n' 'xml_int/mod_xml_curl' 'xml_int/mod_xml_curl' >> "$duplicated/modules.conf" || return 1

	guard_self_test_expect_enable_refusal "$duplicated" 'xml_int/mod_xml_curl' 2 || return 1

	# A list the entry is missing from altogether: the substitution cannot match, and a
	# module that was never activated must not be reported as enabled
	mkdir -p "$absent" || return 1
	grep -v 'xml_int/mod_xml_curl' "$GUARD_SELF_TEST_TEMPLATE" > "$absent/modules.conf" || return 1

	guard_self_test_expect_enable_refusal "$absent" 'xml_int/mod_xml_curl' 0 || return 1

	return 0
}

# Run all four arms and report each one.
#
# The first failure stops the run: once a guard has been shown not to behave the way this
# script declares, the remaining verdicts describe a script that is already wrong.  Each
# arm works on its own scratch list under the same root, so no arm can pass or fail
# because of another arm's side effects.
guard_self_test_arms()
{
	local root="$1"

	guard_self_test_h323_negative "$root"
	guard_self_test_report $? 'arm 1/4 mod_h323 guard negative' \
		'a forced-fail compile+link probe leaves endpoints/mod_h323 disabled and the module list byte-identical' || return 1

	guard_self_test_opal_negative "$root"
	guard_self_test_report $? 'arm 2/4 mod_opal guard negative' \
		'an empty PKG_CONFIG_LIBDIR leaves endpoints/mod_opal disabled and the module list byte-identical' || return 1

	guard_self_test_guards_positive "$root"
	guard_self_test_report $? 'arm 3/4 both guards positive on this host' \
		'the OPAL version gate and the ptlib gate pass, the CVE-2013-1864 probe runs, the verdict drives the postcondition it declares, and each endpoint enablement leaves exactly one active line' || return 1

	guard_self_test_xml_curl_unconditional "$root"
	guard_self_test_report $? 'arm 4/4 mod_xml_curl arm unconditional' \
		'xml_int/mod_xml_curl is enabled outside both capability gates and idempotently, and the exactly-one-active-line postcondition refuses a corrupted list' || return 1

	return 0
}

# Remove the self-test scratch tree.
#
# Cleanup is reachable three ways - the normal path, the EXIT trap and a signal - so it is
# named once instead of repeated, and it tolerates a tree that is already gone because more
# than one of those paths can run in the same shell.  An empty root is ignored rather than
# handed to rm, since a trap that fires before mktemp has answered would otherwise turn
# cleanup into an error message.
guard_self_test_cleanup()
{
	local root="$1"

	if [ -n "$root" ]; then
		rm -rf "$root"
	fi

	return 0
}

# The --guard-self-test mode.
#
# One temporary directory holds every scratch list, every shadow binary and every empty
# pkg-config directory the arms need, so cleanup is a single removal - trapped as well as
# explicit, because an arm that returns early, and a run a CI job times out and kills, must
# leave no scratch tree behind either.  The signal traps exit non-zero rather than resuming,
# because a self-test that was interrupted has not passed.  Nothing outside that directory
# is written, which is what makes the mode safe to run in a working tree.
guard_self_test()
{
	local root
	local status

	if [ ! -r "$GUARD_SELF_TEST_TEMPLATE" ]; then
		echo "Error: $GUARD_SELF_TEST_TEMPLATE is not readable from $PWD; run --guard-self-test from the repository root" >&2
		return 1
	fi

	root=$(mktemp -d) || return 1

	trap 'guard_self_test_cleanup "$root"' EXIT
	trap 'guard_self_test_cleanup "$root"; exit 1' HUP INT TERM

	echo "ci.sh --guard-self-test: asserting both capability guards against scratch copies of $GUARD_SELF_TEST_TEMPLATE under $root"

	guard_self_test_arms "$root"
	status=$?

	guard_self_test_cleanup "$root"
	trap - EXIT HUP INT TERM

	if [ "$status" -eq 0 ]; then
		echo "ci.sh --guard-self-test: all 4 arms passed"
	else
		echo "ci.sh --guard-self-test: failed" >&2
	fi

	return "$status"
}

# Everything above is definitions; everything below runs a job.  Stop here when this
# file is being SOURCED rather than executed.
#
# build/provision_endpoint_toolkits.sh reuses this file's CVE-2013-1864 behavioural
# probe by sourcing it instead of duplicating the probe, and duplicating a security
# check is how the two copies come to disagree.  Without this guard a sourcing shell
# would fall straight through to the "case $CODE" dispatcher below, where an unset or
# unrelated CODE reaches the "*)" arm and calls exit - terminating the CALLER, not a
# subshell.  Together with the argument-parsing guard at the top of the file this makes
# sourcing inert: the caller receives these functions and nothing else happens to it.
if [ "$CI_SH_EXECUTED" != true ]; then
	return 0
fi

# Change to the code directory
if [ -n "$PATH_TO_CODE" ]; then
	cd "$PATH_TO_CODE" || exit 1
fi

# The self-test is a mode of its own, not an action: it asserts the capability guards
# against scratch copies and exits, without configuring, building or installing anything.
# It runs after the directory change above so that the module-list template resolves
# relative to the code directory, exactly as the flows below expect it to.
if [ "$GUARD_SELF_TEST" = true ]; then
	guard_self_test
	exit $?
fi

# Execute appropriate flow based on code, type, and action
case "$CODE" in
	"sofia-sip")
		case "$ACTION" in
			"configure")
				configure_sofia_sip "$TYPE"
				;;
			"build")
				build_sofia_sip "$TYPE"
				;;
			"install")
				install_sofia_sip "$TYPE"
				;;
			"validate")
				validate_sofia_sip "$TYPE"
				;;
			*)
				exit 1
				;;
		esac
		;;
	"freeswitch")
		case "$ACTION" in
			"configure")
				configure_freeswitch "$TYPE"
				;;
			"build")
				build_freeswitch "$TYPE"
				;;
			"install")
				install_freeswitch "$TYPE"
				;;
			"validate")
				validate_freeswitch "$TYPE"
				;;
			*)
				exit 1
				;;
		esac
		;;
	*)
		exit 1
		;;
esac
