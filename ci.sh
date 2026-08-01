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
