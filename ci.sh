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
	exit 1
}

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

# Function to report whether the COMPLETE mod_h323 toolkit is installed
#
# PTLib on its own is not the capability mod_h323 needs: its Makefile.am compiles
# against the OpenH323/H323Plus headers (-I/usr/include/openh323) and links
# -lopenh323 -lpt -lrt, and OPAL installs a ptlib.pc of its own, so a PTLib-only
# or OPAL-only host would otherwise be told to build a module whose H.323
# implementation is absent - turning a missing optional toolkit into a hard build
# failure instead of leaving the module disabled.  This probe therefore mirrors the
# module's own compile and link inputs rather than a proxy for them.  It is
# non-mutating: it builds a throw-away translation unit inside a temporary
# directory and removes it again, touching nothing in the tree.  It is also
# fail-closed - a missing compiler, header or library, or a failed mktemp, all
# leave mod_h323 disabled, which is the intended degradation.
h323_toolkit_available()
{
	local -a compiler
	local -a flags=(-I/usr/include/openh323 -DPTRACING=1 -D_REENTRANT -fno-exceptions)
	local probe_dir
	local status=1

	pkg-config --exists ptlib || return 1

	# configure.ac's IS64BITLINUX conditional adds -DP_64BIT to the module on
	# x86_64, and a probe built with flags the module does not use proves nothing
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
	fi

	rm -rf "$probe_dir"

	return "$status"
}

# Function to report how many ACTIVE - that is, uncommented - lines the generated
# modules.conf carries for one module path
#
# The expression is anchored on the whole line, so a module path can only ever
# match its own entry and never a longer one that merely contains it -
# `xml_int/mod_xml_curl' cannot be answered by `xml_int/mod_xml_curl_extra', and
# `endpoints/mod_opal' cannot be answered by a neighbouring endpoint.  A count is
# printed rather than a yes/no verdict because the only acceptable answer for a
# module that has to be built is exactly one: zero means the entry was never
# activated, and more than one means the generated file carries a duplicate whose
# effective state this script cannot reason about.
#
# The count is always a number on success, so a caller can compare it directly.
# An absent modules.conf is not reported as a count of zero, because "the file
# does not exist" and "the module is not listed" are different facts and only the
# second one is safe to act on: it is diagnosed and reported as a failure so that
# a caller asserting a module is disabled cannot be satisfied by a missing file.
modules_conf_active_count()
{
	local module="$1"
	local count

	if [ ! -f modules.conf ]; then
		echo "Error: modules.conf is missing, cannot determine whether '$module' is enabled" >&2
		return 1
	fi

	# grep -c prints 0 and exits 1 when nothing matched, which is a valid answer
	# here rather than an error, so the status is discarded and the value kept.
	count=$(grep -c -E "^[[:space:]]*${module}[[:space:]]*$" modules.conf)

	echo "${count:-0}"

	return 0
}

# Function to activate one module in the generated modules.conf and prove it worked
#
# The proof is the point.  sed cannot report an address that never matched - it
# exits 0 whether it changed a line or not - so an entry that was renamed,
# removed or duplicated upstream would be skipped in silence.  That silence is
# expensive here: src/mod/Makefile.am wraps each module's ENTIRE recipe in a test
# on whether the module appears in CONF_MODULES, which configure.ac derives by
# stripping comments from modules.conf, so a module that failed to activate
# contributes nothing to `print_tests' and nothing to `check' - no warning, no
# error, and a green build that silently ran none of that module's tests.  The
# edit is therefore followed by a postcondition that counts what the edit was
# supposed to produce, and the caller aborts the run when it does not hold.
#
# The substitution is anchored on the whole line and replaces it outright, so it
# cannot disturb a neighbouring entry, and it is idempotent - an already-active
# line does not match the address and is left exactly as it is.
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

# Function to prove that a module whose capability probe failed will NOT be built
#
# The absent-toolkit outcome needs verifying just as positively as the present
# one.  An endpoint module that is somehow active while its toolkit is missing
# does not degrade, it fails the build - which is the single thing the capability
# guards exist to prevent - so the guard's negative branch asserts the entry is
# still commented out instead of assuming it.
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

			# mod_xml_curl owns one of the module-local test suites this arm exists to
			# collect and depends on no optional toolkit, so it is enabled
			# unconditionally - and the activation is verified rather than assumed,
			# because a silently skipped uncomment would drop its whole suite from
			# `check' without failing anything
			enable_module_for_tests 'xml_int/mod_xml_curl' || exit 1

			# Enable optional endpoint modules only when their toolkit can actually
			# build them, so an absent, incomplete or too-old H.323/OPAL toolkit
			# leaves them disabled rather than failing the build.  OPAL is gated on
			# the version its own header demands - mod_opal.h #errors below 3.12.8 -
			# and H.323 on the complete PTLib plus OpenH323/H323Plus provider set,
			# because a bare ptlib.pc is installed by OPAL too and cannot by itself
			# build mod_h323.
			#
			# Both outcomes of each probe are asserted, which is why these are
			# if/else blocks rather than `probe && sed' compounds: a compound leaves
			# the enabling edit unchecked, and leaves the absent-toolkit branch with
			# no postcondition at all.  Here a passing probe must end with the module
			# active, and a failing probe must end with it still commented out, and
			# anything else aborts this arm before ./configure runs.
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

# Change to the code directory
if [ -n "$PATH_TO_CODE" ]; then
	cd "$PATH_TO_CODE" || exit 1
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
