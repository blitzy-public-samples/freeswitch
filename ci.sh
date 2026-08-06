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
	echo "  --guard-self-test  Assert both capability guards - the CVE-2013-1864"
	echo "                     verdict matrix included - and the unconditional"
	echo "                     mod_xml_curl arm against scratch copies, then exit"
	echo "                     REQUIRES A HOST THAT CARRIES BOTH ENDPOINT TOOLKITS."
	echo "                     Two of the five arms assert the guards' POSITIVE"
	echo "                     branch, which cannot be asserted where the toolkit is"
	echo "                     absent, so on a toolkit-free host - including the"
	echo "                     project's own Debian bookworm CI image as it ships -"
	echo "                     this mode fails by design rather than passing"
	echo "                     vacuously. Provision the host first with"
	echo "                     build/provision_endpoint_toolkits.sh, then run it."
	echo "                     The unit-test arm itself needs none of this: it gates"
	echo "                     each endpoint on the toolkit and enables mod_xml_curl"
	echo "                     unconditionally, so it succeeds either way."
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

#------------------------------------------------------------------------------
# Scratch directories
#------------------------------------------------------------------------------
#
# Every probe below compiles in a temporary directory, and a probe that leaks one on
# an interrupted run leaks one on every interrupted run.  Two properties are wanted
# from one place rather than from each probe: a parent that has been VALIDATED, so a
# TMPDIR that is unset, relative or not a writable directory can never turn scratch
# creation into a write somewhere unintended; and a REGISTRY, so a caller that owns a
# signal trap - guard_self_test() below - can remove every directory this file created
# and not merely the root it created itself.

CI_SCRATCH_PARENT=''
CI_SCRATCH_LAST=''
declare -a CI_SCRATCH_DIRS=()

# Resolve and validate the one parent every scratch directory is created under.
#
# Resolved once and cached: the answer cannot change during a run, and a second
# resolution is a second thing that can disagree with the first.
ci_scratch_parent()
{
	local parent

	if [ -n "$CI_SCRATCH_PARENT" ]; then
		printf '%s\n' "$CI_SCRATCH_PARENT"
		return 0
	fi

	if ! parent=$(cd -- "${TMPDIR:-/tmp}" > /dev/null 2>&1 && pwd); then
		echo "Error: ${TMPDIR:-/tmp} is not a usable directory, so no temporary directory can be created" >&2
		return 1
	fi

	# cd+pwd has already made this absolute; the check is against the ONE absolute path
	# that must never become the parent of an rm -rf target
	case "$parent" in
		/)
			echo "Error: refusing to create temporary directories directly under '/'" >&2
			return 1
			;;
	esac

	if [ ! -w "$parent" ]; then
		echo "Error: '$parent' is not writable, so no temporary directory can be created" >&2
		return 1
	fi

	CI_SCRATCH_PARENT="$parent"

	printf '%s\n' "$parent"

	return 0
}

# Create one scratch directory, inside a caller-owned parent when one is given.
#
# Sets CI_SCRATCH_LAST rather than printing the path: a command substitution would run
# this in a subshell, where the registration below would be lost and the trap in the
# calling shell would then have nothing to remove.
ci_scratch_dir()
{
	local parent="${1:-}"
	local dir

	if [ -z "$parent" ]; then
		parent=$(ci_scratch_parent) || return 1
	fi

	if [ ! -d "$parent" ]; then
		echo "Error: '$parent' is not a directory, so no temporary directory can be created in it" >&2
		return 1
	fi

	dir=$(mktemp -d -p "$parent") || return 1

	CI_SCRATCH_DIRS+=("$dir")
	CI_SCRATCH_LAST="$dir"

	return 0
}

# Remove one registered scratch directory and forget it.
#
# The path is checked rather than trusted even though this file created it: an empty or
# top-level value here would make this an rm -rf of something else entirely.
ci_scratch_remove()
{
	local dir="$1"
	local -a kept=()
	local entry

	for entry in "${CI_SCRATCH_DIRS[@]}"; do
		if [ "$entry" != "$dir" ]; then
			kept+=("$entry")
		fi
	done

	CI_SCRATCH_DIRS=("${kept[@]}")

	case "$dir" in
		/*/*)
			rm -rf -- "$dir" || return 1
			;;
		*)
			echo "Error: refusing to remove unexpected scratch path '$dir'" >&2
			return 1
			;;
	esac

	return 0
}

# Remove every scratch directory this file still owns.
#
# Reachable from the normal path and from a signal trap, so it tolerates a directory
# that is already gone, and it reports a removal that did not happen instead of
# swallowing it.
ci_scratch_cleanup()
{
	local dir
	local status=0

	for dir in "${CI_SCRATCH_DIRS[@]}"; do
		case "$dir" in
			/*/*)
				rm -rf -- "$dir" || status=1
				;;
			*)
				echo "Error: refusing to remove unexpected scratch path '$dir'" >&2
				status=1
				;;
		esac
	done

	CI_SCRATCH_DIRS=()

	return "$status"
}

#------------------------------------------------------------------------------
# The mod_h323 capability and CVE-2013-1864 verdict: ONE implementation, two consumers
#------------------------------------------------------------------------------
#
# Two questions have to be answered before endpoints/mod_h323 may be built, and both
# are answered here so that they are answered the same way everywhere.
#
#   CAPABILITY  Can this host compile and link mod_h323's own inputs?  PTLib alone is
#               not that capability: the module also needs the OpenH323/H323Plus
#               headers and -lopenh323, and OPAL installs a ptlib.pc of its own.  The
#               probe therefore mirrors the module's compile and link inputs instead of
#               standing in for them, and it builds outside the tree.
#   ADVISORY    Can the PTLib that would be LOADED be cleared of CVE-2013-1864?
#
# The verdict is published as variables - H323_GUARD_VERDICT and its three companions -
# and as key=value lines for a consumer in another process.  That contract exists for a
# reason worth stating: build/provision_endpoint_toolkits.sh has to reach the same
# conclusion as CI about the same toolkit, and a consumer that re-derived the verdict by
# reading these diagnostics would be a second copy of this decision.  Two copies of a
# security verdict are two things that can come to disagree, and a disagreement here
# means a toolkit one script calls provisioned while the other silently excludes it.
#
# Every outcome is fail-closed except the two that are affirmatively safe.

# Where the probes look for the H.323 SDK.
#
# The defaults are mod_h323's own inputs, byte for byte, from
# src/mod/endpoints/mod_h323/Makefile.am:6 and :10 - a probe that searched somewhere
# else would answer a question nobody asked, because what CI needs to know is whether
# THE MODULE can be built and the module's paths are hardcoded.  They are variables
# rather than literals for exactly one reason: a provisioning run installs the toolkit
# into a prefix of its own choosing and then has to interrogate the stack it just
# installed, which is not necessarily the stack on the default search paths.  Nothing
# resets them, because nothing has to: the only consumer that redirects them does so in
# a subshell that sourced this file, so the next consumer starts from these values.
H323_PROBE_INCLUDE_DIRS=(/usr/include/openh323)
H323_PROBE_LIB_DIRS=(/usr/lib)

# The scratch parent every probe below creates its temporary directory inside.
#
# Empty means "create one under the validated default parent and remove it again", which
# is what a CI run does.  A caller that already owns a signal trap - guard_self_test()
# below - points this at the root it cleans up, so a run interrupted mid-compile leaves
# no probe directory behind either.  A variable rather than a parameter so that
# configure_freeswitch()'s call site stays exactly what it has always been.
H323_PROBE_SCRATCH_PARENT=''

# Point the probes at one install prefix instead: PTLib's headers land in
# $prefix/include, H323Plus' in $prefix/include/openh323, both libraries in $prefix/lib.
# The layout lives here so that no consumer has to know it.
h323_probe_search_prefix()
{
	local prefix="$1"

	case "$prefix" in
		/*) ;;
		*)
			echo "Error: '$prefix' is not an absolute path, so it cannot be an H.323 SDK prefix" >&2
			return 1
			;;
	esac

	H323_PROBE_INCLUDE_DIRS=("$prefix/include" "$prefix/include/openh323")
	H323_PROBE_LIB_DIRS=("$prefix/lib")

	return 0
}

declare -a H323_PROBE_CFLAGS=()
declare -a H323_PROBE_LDFLAGS=()

# Compose the flags all three probes share from the search paths in force.
h323_probe_compose_flags()
{
	local dir

	H323_PROBE_CFLAGS=()

	for dir in "${H323_PROBE_INCLUDE_DIRS[@]}"; do
		H323_PROBE_CFLAGS+=("-I$dir")
	done

	H323_PROBE_CFLAGS+=(-DPTRACING=1 -D_REENTRANT -fno-exceptions)

	# Match configure.ac's IS64BITLINUX conditional; a probe built with flags the
	# module does not use proves nothing
	if [ "$(uname -m)" = "x86_64" ]; then
		H323_PROBE_CFLAGS+=(-DP_64BIT)
	fi

	H323_PROBE_LDFLAGS=()

	for dir in "${H323_PROBE_LIB_DIRS[@]}"; do
		H323_PROBE_LDFLAGS+=("-L$dir")
	done

	return 0
}

# The loader path the probes run and are inspected under.
#
# The SDK's own library directories come first because they are the SDK under test, then
# the libdir pkg-config reports, then whatever the caller already had.  On a host
# carrying two PTLibs under one pkg-config name - which is the layout this project's
# own host has - those are not the same directory, and the advisory is about the library
# that is actually loaded.
h323_probe_library_path()
{
	local libdir="$1"
	local value=''
	local dir

	for dir in "${H323_PROBE_LIB_DIRS[@]}"; do
		value="${value:+$value:}$dir"
	done

	if [ -n "$libdir" ]; then
		value="${value:+$value:}$libdir"
	fi

	printf '%s\n' "${value}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

	return 0
}

# Is PTLib's PXML PARSER reachable from this SDK at all?
#
# The question exists because one compile failure carries two opposite meanings.  The
# entity-ceiling probe below fails to compile against a pre-2.10.10 PTLib, whose PXML
# parser has no SetMaxEntityLength - the vulnerable configuration - and it fails just
# the same against a PTLib built without expat, where ptclib/pxml.h declares PXML as a
# NAMESPACE holding one string helper and the library contains no XML parser, hence no
# entity expander, at all.  Collapsing the two would either refuse a toolkit that cannot
# be vulnerable or clear one that is.
#
# Both answers therefore require POSITIVE evidence, which is what makes this fail
# closed: "the parser is there" is proven by a translation unit that needs PXML to be a
# complete type, and "the parser is absent" is proven by one that needs PXML to be the
# namespace an expat-less PTLib ships.  Neither compiling is not an answer.  Both probes
# see the same SDK the capability probe saw, with the same flags the module is built
# with, or their answer would be about something else.
#
# Returns 0 the parser is reachable, so an absent ceiling API is the unbounded
#           pre-2.10.10 expander
#         1 the parser is provably absent, so CVE-2013-1864 has no code path here
#         2 undecidable - no compiler, no scratch directory, or neither form compiled
h323_pxml_parser_state()
{
	local -a compiler
	local dir
	local state=2

	read -ra compiler <<< "${CXX:-g++}"

	if [ "${#compiler[@]}" -eq 0 ] || ! command -v "${compiler[0]}" > /dev/null 2>&1; then
		return 2
	fi

	ci_scratch_dir "$H323_PROBE_SCRATCH_PARENT" || return 2
	dir="$CI_SCRATCH_LAST"

	# sizeof needs a COMPLETE type, so this compiles only when PXML is the parser class
	printf '#include <ptlib.h>\n#include <ptclib/pxml.h>\nint main(void) { return (int) sizeof(PXML); }\n' \
		> "$dir/parser.cpp" || {
		ci_scratch_remove "$dir"
		return 2
	}

	# ... and taking the address of the namespace-scope helper compiles only when PXML
	# is the namespace, with the signature ptclib/pxml.h declares in that configuration
	printf '#include <ptlib.h>\n#include <ptclib/pxml.h>\nint main(void) { PString (*fn)(const PString &) = &PXML::EscapeSpecialChars; return fn != 0; }\n' \
		> "$dir/namespace.cpp" || {
		ci_scratch_remove "$dir"
		return 2
	}

	if "${compiler[@]}" "${H323_PROBE_CFLAGS[@]}" -fsyntax-only "$dir/parser.cpp" > /dev/null 2>&1; then
		state=0
	elif "${compiler[@]}" "${H323_PROBE_CFLAGS[@]}" -fsyntax-only "$dir/namespace.cpp" > /dev/null 2>&1; then
		state=1
	fi

	ci_scratch_remove "$dir"

	return "$state"
}

# The verdict itself.
#
# H323_GUARD_VERDICT is set to exactly one of:
#
#   clear            the probe ran the entity document and the library bounded it
#   clear_no_parser  the library provably has no XML parser to be vulnerable with
#   vulnerable       the library ignored the ceiling it was given, or exposes the parser
#                    with no ceiling API at all
#   unverifiable     the advisory could not be decided, or the libpt that would load
#                    could not be resolved - an unverifiable linkage cannot be cleared
#   absent           there is no H.323 PTLib here to judge at all
#
# H323_GUARD_LINKABLE says whether mod_h323's own compile and link inputs are satisfied,
# H323_GUARD_LIBPT names the libpt the link resolved, and H323_GUARD_DETAIL carries one
# line of prose for a report.  The return status is 0 for the two affirmatively safe
# verdicts and non-zero for every other, including every unexpected one.
H323_GUARD_VERDICT=''
H323_GUARD_DETAIL=''
H323_GUARD_LINKABLE='no'
H323_GUARD_LIBPT=''

h323_guard_evaluate()
{
	local -a compiler
	local dir
	local libdir
	local ldpath
	local linked
	local parser

	H323_GUARD_VERDICT='absent'
	H323_GUARD_DETAIL='no H.323 PTLib could be compiled and linked against, so there is nothing here to judge'
	H323_GUARD_LINKABLE='no'
	H323_GUARD_LIBPT=''

	if ! pkg-config --exists ptlib; then
		H323_GUARD_DETAIL='pkg-config resolves no ptlib, so the H.323 toolkit is absent'
		echo "ci.sh: pkg-config resolves no ptlib, so endpoints/mod_h323 stays disabled" >&2

		return 1
	fi

	read -ra compiler <<< "${CXX:-g++}"

	if [ "${#compiler[@]}" -eq 0 ]; then
		H323_GUARD_VERDICT='unverifiable'
		H323_GUARD_DETAIL='CXX names no compiler, so nothing about this toolkit can be established'
		echo "ci.sh: CXX names no compiler, so endpoints/mod_h323 stays disabled" >&2

		return 1
	fi

	h323_probe_compose_flags

	if ! ci_scratch_dir "$H323_PROBE_SCRATCH_PARENT"; then
		H323_GUARD_VERDICT='unverifiable'
		H323_GUARD_DETAIL='no scratch directory could be created, so this toolkit could not be probed'
		echo "ci.sh: no scratch directory could be created, so endpoints/mod_h323 stays disabled" >&2

		return 1
	fi

	dir="$CI_SCRATCH_LAST"

	# ptlib.h has to come first, exactly as mod_h323.h includes the two headers
	printf '#include <ptlib.h>\n#include <h323.h>\nint main(void) { return 0; }\n' > "$dir/probe.cpp"

	if ! "${compiler[@]}" "${H323_PROBE_CFLAGS[@]}" "$dir/probe.cpp" -o "$dir/probe" \
		"${H323_PROBE_LDFLAGS[@]}" -lopenh323 -lpt -lrt > /dev/null 2>&1; then
		echo "ci.sh: this host cannot compile and link mod_h323's own inputs, so endpoints/mod_h323 stays disabled" >&2
		ci_scratch_remove "$dir"

		return 1
	fi

	H323_GUARD_LINKABLE='yes'

	libdir=$(pkg-config --variable=libdir ptlib 2> /dev/null)
	ldpath=$(h323_probe_library_path "$libdir")

	# CVE-2013-1864: PTLib's PXML parser expanded internal entities with no ceiling
	# before 2.10.10, so a "billion laughs" document makes any consumer allocate until
	# it is killed.  The check below is behavioural instead of a version comparison,
	# because a version comparison is wrong in both directions here: distributions and
	# vendors backport the fix without renaming the library, and a .pc file can
	# advertise a version that is not the one the loader resolves.  Asking the library
	# to honour a deliberately tiny entity ceiling answers the only question that
	# matters, and answers it with a benign document rather than a hostile one.
	cat > "$dir/entity.cpp" <<- 'PROBE'
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

	if "${compiler[@]}" "${H323_PROBE_CFLAGS[@]}" "$dir/entity.cpp" -o "$dir/entity" \
		"${H323_PROBE_LDFLAGS[@]}" -lpt -lrt > /dev/null 2>&1; then
		if LD_LIBRARY_PATH="$ldpath" timeout 60 "$dir/entity" > /dev/null 2>&1; then
			H323_GUARD_VERDICT='clear'
			H323_GUARD_DETAIL='the library refused a document that exceeds the entity ceiling it was given, as a fixed PTLib must'
		else
			H323_GUARD_VERDICT='vulnerable'
			H323_GUARD_DETAIL='the library parsed a document that exceeds the entity ceiling it was given'
			echo "ci.sh: the PTLib being linked ignores its XML entity ceiling (CVE-2013-1864); endpoints/mod_h323 stays disabled" >&2
		fi
	else
		# No ceiling API.  Which of the two things that means is decided by positive
		# evidence, never by assumption - see h323_pxml_parser_state().
		h323_pxml_parser_state
		parser=$?

		case "$parser" in
			0)
				H323_GUARD_VERDICT='vulnerable'
				H323_GUARD_DETAIL='the toolkit exposes PTLib PXML parser but no entity ceiling API, which is the unbounded pre-2.10.10 expander'
				echo "ci.sh: the PTLib being linked exposes a PXML parser with no entity ceiling API, which is the unbounded pre-2.10.10 expander (CVE-2013-1864); endpoints/mod_h323 stays disabled" >&2
				;;
			1)
				H323_GUARD_VERDICT='clear_no_parser'
				H323_GUARD_DETAIL='this PTLib carries no PXML parser at all (built without expat), so the advisory has no code path in it'
				;;
			*)
				H323_GUARD_VERDICT='unverifiable'
				H323_GUARD_DETAIL='the toolkit exposes no XML entity ceiling and whether it exposes an XML parser at all could not be determined'
				echo "ci.sh: the PTLib being linked exposes no XML entity ceiling and whether it carries an XML parser at all could not be determined, so CVE-2013-1864 cannot be ruled out; endpoints/mod_h323 stays disabled" >&2
				;;
		esac
	fi

	# The advisory is about the library that is actually loaded, so record which libpt
	# this link resolved and refuse a linkage that cannot be established: an
	# unverifiable linkage cannot be cleared of the advisory either.
	linked=$(LD_LIBRARY_PATH="$ldpath" ldd "$dir/probe" 2> /dev/null |
		sed -n 's|^[[:space:]]*libpt\.so[^[:space:]]*[[:space:]]*=>[[:space:]]*\(/[^[:space:]]*\).*|\1|p')

	H323_GUARD_LIBPT="$linked"

	if [ -z "$linked" ]; then
		case "$H323_GUARD_VERDICT" in
			clear | clear_no_parser)
				H323_GUARD_VERDICT='unverifiable'
				H323_GUARD_DETAIL='the libpt this toolkit would load could not be resolved, and an unverifiable linkage cannot be cleared of the advisory'
				;;
		esac

		echo "ci.sh: cannot establish which libpt endpoints/mod_h323 would load; it stays disabled" >&2
	else
		case "$H323_GUARD_VERDICT" in
			clear)
				echo "ci.sh: endpoints/mod_h323 will link $linked (ptlib $(pkg-config --modversion ptlib 2> /dev/null)), which bounds XML entity expansion"
				;;
			clear_no_parser)
				echo "ci.sh: endpoints/mod_h323 will link $linked (ptlib $(pkg-config --modversion ptlib 2> /dev/null)), which carries no PXML parser at all, so CVE-2013-1864 has no code path in it"
				;;
		esac
	fi

	ci_scratch_remove "$dir"

	case "$H323_GUARD_VERDICT" in
		clear | clear_no_parser)
			return 0
			;;
	esac

	return 1
}

# Publish the verdict as fixed key=value lines, for a consumer in another process.
#
# build/provision_endpoint_toolkits.sh sources this file and runs the guard in a
# subshell, so the verdict has to cross a process boundary; it crosses as keys rather
# than as the diagnostics above so that the consumer reads the decision instead of
# re-deriving it.
h323_guard_verdict_lines()
{
	printf 'H323_GUARD_VERDICT=%s\n' "$H323_GUARD_VERDICT"
	printf 'H323_GUARD_LINKABLE=%s\n' "$H323_GUARD_LINKABLE"
	printf 'H323_GUARD_LIBPT=%s\n' "$H323_GUARD_LIBPT"
	printf 'H323_GUARD_DETAIL=%s\n' "$H323_GUARD_DETAIL"

	return 0
}

# The CI-facing guard: the optional module stays disabled unless it can actually be
# built AND the PTLib it would load can be cleared of CVE-2013-1864.
#
# Name, call shape and fail-closed contract are unchanged - zero when
# endpoints/mod_h323 may be built, non-zero otherwise - so configure_freeswitch()'s
# unit-test arm calls it exactly as it always has.  Where the probes compile is
# H323_PROBE_SCRATCH_PARENT's business, and which SDK they interrogate is
# h323_probe_search_prefix()'s, so neither concern reaches this call site.
h323_toolkit_available()
{
	h323_guard_evaluate
}

# Build a scratch PTLib SDK exhibiting one chosen entity-expansion behaviour.
#
# The verdicts above have five outcomes and any one real host only ever demonstrates
# one of them, so the rest would be unreachable by any test - including the fail-closed
# refusal, which is the branch that matters most and the branch a green run on a healthy
# host never exercises.  This is a complete but empty stand-in: headers declaring only
# what the probes reference, and two shared objects carrying the sonames the module
# links, so the capability probe compiles, links AND resolves a libpt exactly as it does
# against a real toolkit.  Both consumers use this one builder, because a second copy of
# a security fixture is a second thing to get wrong.
#
# Nothing is installed and nothing outside the given directory is touched: the SDK is
# selected only by pointing h323_probe_search_prefix() and PKG_CONFIG_LIBDIR at it, so
# the real toolchain, the real .pc files and the real toolkits are untouched.
#
# Flavour, and the verdict it must produce:
#
#   bounded         a parser whose Load() honours the ceiling        -> clear
#   unbounded       a parser whose Load() ignores the ceiling        -> vulnerable
#   no_ceiling_api  a parser with no SetMaxEntityLength at all       -> vulnerable
#   no_parser       PXML as the namespace an expat-less PTLib ships  -> clear_no_parser
h323_probe_stub_sdk()
{
	local dir="$1"
	local flavour="$2"
	local -a compiler
	local loads='false'

	read -ra compiler <<< "${CXX:-g++}"

	if [ "${#compiler[@]}" -eq 0 ]; then
		echo "Error: CXX names no compiler, so no stub PTLib SDK can be built" >&2
		return 1
	fi

	case "$dir" in
		/*/*) ;;
		*)
			echo "Error: '$dir' is not a usable directory for a stub PTLib SDK" >&2
			return 1
			;;
	esac

	mkdir -p "$dir/include/ptclib" "$dir/include/openh323" "$dir/lib/pkgconfig" "$dir/src" || return 1

	# PStubLink is declared here and DEFINED in the stub libpt on purpose.  Every probe
	# translation unit includes this header, so every one of them takes a genuine
	# DT_NEEDED on libpt - a library nothing references is dropped by the linker's
	# --as-needed default, and the guard's linkage identity check would then have no
	# libpt to resolve and would fail for the wrong reason.
	cat > "$dir/include/ptlib.h" <<- 'STUB'
		#ifndef CI_SH_PTLIB_STUB_H
		#define CI_SH_PTLIB_STUB_H
		/* A stand-in for PTLib carrying only what ci.sh's probes reference. */
		class PString
		{
		  public:
		    PString(const char *) { }
		};
		class PStubLink
		{
		  public:
		    PStubLink();
		};
		static PStubLink ci_sh_ptlib_stub_link;
		#endif
	STUB

	cat > "$dir/include/openh323/h323.h" <<- 'STUB'
		#ifndef CI_SH_H323_STUB_H
		#define CI_SH_H323_STUB_H
		#include <ptlib.h>
		class H323StubLink
		{
		  public:
		    H323StubLink();
		};
		static H323StubLink ci_sh_h323_stub_link;
		#endif
	STUB

	case "$flavour" in
		bounded)
			loads='false'
			;;
		unbounded)
			loads='true'
			;;
		no_ceiling_api | no_parser) ;;
		*)
			echo "Error: '$flavour' is not a stub PTLib SDK flavour" >&2
			return 1
			;;
	esac

	case "$flavour" in
		bounded | unbounded)
			# A complete parser type with the ceiling API.  Load() decides the verdict:
			# refusing the document is what a fixed PTLib does, accepting it is the
			# advisory's behaviour.
			cat > "$dir/include/ptclib/pxml.h" <<- STUB
				#ifndef CI_SH_PXML_STUB_H
				#define CI_SH_PXML_STUB_H
				#include <ptlib.h>
				class PXML
				{
				  public:
				    void SetMaxEntityLength(unsigned) { }
				    bool Load(const PString &) { return $loads; }
				};
				#endif
			STUB
			;;
		no_ceiling_api)
			# The pre-2.10.10 shape: the parser is there, the ceiling API is not.
			cat > "$dir/include/ptclib/pxml.h" <<- 'STUB'
				#ifndef CI_SH_PXML_STUB_H
				#define CI_SH_PXML_STUB_H
				#include <ptlib.h>
				class PXML
				{
				  public:
				    bool Load(const PString &) { return true; }
				};
				#endif
			STUB
			;;
		no_parser)
			# The expat-less shape, mirroring ptclib/pxml.h's own #ifndef P_EXPAT
			# branch: PXML is a namespace holding one string helper and there is no
			# parser, hence no entity expander, to be vulnerable with.
			cat > "$dir/include/ptclib/pxml.h" <<- 'STUB'
				#ifndef CI_SH_PXML_STUB_H
				#define CI_SH_PXML_STUB_H
				#include <ptlib.h>
				namespace PXML {
				extern PString EscapeSpecialChars(const PString & str);
				};
				#endif
			STUB
			;;
	esac

	printf 'class PStubLink { public: PStubLink(); };\nPStubLink::PStubLink() { }\n' > "$dir/src/libpt.cpp" || return 1
	printf 'class H323StubLink { public: H323StubLink(); };\nH323StubLink::H323StubLink() { }\n' > "$dir/src/libopenh323.cpp" || return 1

	if ! "${compiler[@]}" -shared -fPIC -o "$dir/lib/libpt.so" -Wl,-soname,libpt.so \
		"$dir/src/libpt.cpp" > /dev/null 2>&1; then
		echo "Error: could not build the stub libpt for a '$flavour' PTLib SDK" >&2
		return 1
	fi

	if ! "${compiler[@]}" -shared -fPIC -o "$dir/lib/libopenh323.so" -Wl,-soname,libopenh323.so \
		"$dir/src/libopenh323.cpp" > /dev/null 2>&1; then
		echo "Error: could not build the stub libopenh323 for a '$flavour' PTLib SDK" >&2
		return 1
	fi

	# pkg-config has to answer for this SDK, because the guard asks it which libdir the
	# PTLib being judged lives in.  Reached by PKG_CONFIG_LIBDIR, which REPLACES
	# pkg-config's search path, so no installed .pc file is read, moved or edited.
	cat > "$dir/lib/pkgconfig/ptlib.pc" <<- PC
		prefix=$dir
		exec_prefix=\${prefix}
		libdir=\${exec_prefix}/lib
		includedir=\${prefix}/include

		Name: ptlib
		Description: PTLib stand-in built by ci.sh for the $flavour CVE-2013-1864 verdict
		Version: 2.10.9
		Libs: -L\${libdir} -lpt
		Cflags: -I\${includedir}
	PC

	return 0
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

# Say out loud whether tests/unit/switch_rtp_pcap will be part of this run.
#
# It is the one collected test whose presence is decided by the host rather than by
# modules.conf: tests/unit/Makefile.am wraps it in `if HAVE_PCAP', and configure.ac
# sets that conditional from whether pcap-config is on PATH.  Without libpcap's
# development package the test is not built, is not collected, and `print_tests'
# simply returns one fewer entry - with no warning anywhere, because nothing in the
# build treats an absent optional dependency as an error.
#
# That silence is the problem this addresses.  A collected-test count is only a
# meaningful acceptance figure if the reason it moved is visible, so the probe is
# reported rather than left to be inferred from a diff of two test lists.  It is
# deliberately NOT a gate: libpcap is optional, an absent optional dependency must
# degrade rather than fail, and that is the same posture the H.323 and OPAL toolkit
# guards above take.  Nothing here installs anything or edits any file.
report_pcap_capability()
{
	if command -v pcap-config > /dev/null 2>&1; then
		echo "Capability: pcap-config found; HAVE_PCAP will be true and tests/unit/switch_rtp_pcap will be built and collected"
	else
		echo "Capability: pcap-config NOT found; HAVE_PCAP will be false, so tests/unit/switch_rtp_pcap is not built and the collected-test count is one lower. Install libpcap's development package to collect it."
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

			report_pcap_capability

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
# one that matters, and the two wrong outcomes it stands between fail DIFFERENTLY.
# Activating a module whose toolkit is missing makes the build RED: a missing toolkit
# does not degrade, it breaks the compile.  Leaving a module INACTIVE whose toolkit is
# present keeps the build GREEN and contributes no tests at all (src/mod/Makefile.am
# wraps each module's entire recipe in a test on CONF_MODULES, which configure.ac
# derives by stripping comments from modules.conf) - so only the second is invisible,
# and it is invisible precisely because nothing fails.  Neither branch is reached on a
# fully provisioned host, which is why running the suite catches neither of them.
#
# The negative branch is reached by ISOLATING THE ENVIRONMENT ONLY - a PATH-shadowed
# compiler wrapper that cannot compile anything, PKG_CONFIG_LIBDIR pointed at an empty
# directory, and for the advisory verdict a scratch stub SDK that PKG_CONFIG_LIBDIR and
# h323_probe_search_prefix() point the probes at - inside a subshell, so no toolkit is
# uninstalled, no .pc file is edited and no compiler is replaced.  Every module-list edit
# lands on a scratch copy of the module-list template inside a temporary directory, so
# build/modules.conf.in - the default-build contract - and the working tree's generated
# modules.conf are never touched.  What is asserted is not new behaviour but the
# postconditions this script already declares: exactly one active line after an
# enablement, zero after a refusal.
#
# The five arms, and what each one is the only proof of:
#
#   1  the mod_h323 guard refuses a toolkit it cannot compile and link against
#   2  the mod_opal guard refuses a host pkg-config cannot resolve OPAL on
#   3  BOTH guards CLEAR this host and enable both endpoints - a refusal fails this arm,
#      because an arm that accepted one would certify a build the module is missing from
#   4  mod_xml_curl is enabled outside both gates, idempotently, and its postcondition
#      refuses a module list it cannot hold for
#   5  every CVE-2013-1864 verdict, including the two a healthy host can never show: a
#      vulnerable PTLib and one whose parser predates the ceiling API are refused by
#      name, and a PTLib built without expat - which has no parser to be vulnerable
#      with - is cleared

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
	local marker
	local wrapper
	local template
	local after

	# h323_toolkit_available() returns 1 in SILENCE when pkg-config resolves no ptlib and when
	# no scratch directory can be created, both of them BEFORE the compile-and-link step this arm
	# exists to exercise.  Without this precondition an early refusal for one of those reasons
	# satisfies the arm while the shadowed compiler was never consulted, so the arm would report a
	# pass for a step it never reached.
	if ! pkg-config --exists ptlib; then
		echo "Error: this host has no ptlib.pc, so h323_toolkit_available refuses before it reaches the compile+link probe this arm has to exercise" >&2
		echo "Error: install an H.323 toolkit - see build/provision_endpoint_toolkits.sh - or run --guard-self-test on a host that carries one; the unit-test arm itself does not need it, because it gates mod_h323 on exactly this probe" >&2
		return 1
	fi

	guard_self_test_scratch_list "$dir" || return 1
	mkdir -p "$bin" || return 1

	marker="$dir/shadow-invocations"

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

	# One line per invocation, carrying that invocation's whole argument list, so the marker says
	# WHICH translation unit the probe asked for rather than merely that something ran.  The heredoc
	# delimiter is unquoted so $marker is expanded here, while \$* stays an expansion the wrapper
	# performs when it is called.
	cat > "$bin/$wrapper" <<- WRAPPER || return 1
		#!/usr/bin/env bash
		printf '%s\n' "\$*" >> '$marker'
		exit 1
	WRAPPER

	chmod +x "$bin/$wrapper" || return 1

	template=$(guard_self_test_checksum "$GUARD_SELF_TEST_TEMPLATE") || return 1

	(
		cd "$dir" || exit 1

		PATH="$bin:$PATH"
		CXX="${compiler[*]}"
		export PATH CXX

		# Compile inside this arm's own directory, which the self-test root already
		# owns, so an interrupt during the probe leaves nothing behind
		H323_PROBE_SCRATCH_PARENT="$dir"

		if h323_toolkit_available; then
			echo "Error: h323_toolkit_available reported the H.323 toolkit usable while its probe could not compile anything" >&2
			exit 1
		fi

		require_module_disabled_for_tests 'endpoints/mod_h323' || exit 1
	) || return 1

	if [ ! -s "$marker" ]; then
		echo "Error: the shadowed compiler was never invoked, so the H.323 compile+link probe is not what refused and this arm proved nothing" >&2
		return 1
	fi

	if ! grep -q 'probe\.cpp' "$marker"; then
		echo "Error: the shadowed compiler was invoked but never on the probe's own translation unit, so the compile+link step was not reached" >&2
		cat "$marker" >&2
		return 1
	fi

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

# Arm 3: run both guards against the REAL host, with no simulated environment at all.
#
# The negative arms prove that a refusal is respected; this one proves the guards still
# say yes when the toolkit really is installed, and that the enablement they then perform
# satisfies its own postcondition - exactly one active line per endpoint, because zero
# means the module was never activated and more than one is a duplicate whose effective
# state this script cannot reason about.  h323_toolkit_available() is called whole, so the
# CVE-2013-1864 verdict runs inside this arm rather than being reimplemented beside it.
#
# A REFUSAL FAILS THIS ARM.  That is the whole point of it: the arm exists to assert that
# both guards clear a host carrying both toolkits, so accepting a refusal here would
# certify a state in which endpoints/mod_h323 is silently excluded from the build - which
# is precisely the failure the guards are supposed to make visible, and which looks green
# from the outside because a module that is not enabled contributes no tests at all.  The
# fail-closed branch of the advisory verdict is asserted, hermetically and against every
# outcome, by arm 5; it is never counted as this arm.
guard_self_test_guards_positive()
{
	local dir="$1/guards-positive"
	local log
	local active
	local module

	guard_self_test_scratch_list "$dir" || return 1

	log="$dir/h323-guard.log"

	if ! pkg-config --atleast-version=3.12.8 opal; then
		echo "Error: this host does not satisfy the OPAL version gate, so the positive branch of the mod_opal guard cannot be asserted here" >&2
		echo "Error: provision OPAL with build/provision_endpoint_toolkits.sh, or run --guard-self-test on a host that carries it; the unit-test arm needs neither, because it gates mod_opal on exactly this check" >&2
		return 1
	fi

	if ! pkg-config --exists ptlib; then
		echo "Error: this host has no ptlib.pc, so the positive branch of the mod_h323 guard cannot be asserted here" >&2
		echo "Error: provision PTLib with build/provision_endpoint_toolkits.sh, or run --guard-self-test on a host that carries it; the unit-test arm needs neither, because it gates mod_h323 on exactly this check" >&2
		return 1
	fi

	(
		cd "$dir" || exit 1

		# The probes compile inside this arm's own directory, which the self-test root
		# already owns, so an interrupt during a compile leaves nothing behind
		H323_PROBE_SCRATCH_PARENT="$dir"

		if ! h323_toolkit_available 2> "$log"; then
			echo "Error: the mod_h323 guard refuses this host, so its positive branch cannot be asserted here; the guard's own diagnosis follows" >&2
			cat "$log" >&2
			echo "Error: install an H.323 toolkit this guard clears - see build/provision_endpoint_toolkits.sh - or run this mode on a host that carries one" >&2
			exit 1
		fi

		case "$H323_GUARD_VERDICT" in
			clear | clear_no_parser) ;;
			*)
				echo "Error: the mod_h323 guard cleared this host with the verdict '$H323_GUARD_VERDICT', which is not one of the two verdicts that permit a build" >&2
				exit 1
				;;
		esac

		if [ "$H323_GUARD_LINKABLE" != 'yes' ] || [ -z "$H323_GUARD_LIBPT" ]; then
			echo "Error: the mod_h323 guard cleared this host without establishing that mod_h323 links (linkable '$H323_GUARD_LINKABLE', libpt '${H323_GUARD_LIBPT:-none}')" >&2
			exit 1
		fi

		for module in 'endpoints/mod_opal' 'endpoints/mod_h323'; do
			enable_module_for_tests "$module" || exit 1

			active=$(modules_conf_active_count "$module") || exit 1

			if [ "$active" != "1" ]; then
				echo "Error: expected exactly 1 active '$module' line after enablement, found ${active:-0}" >&2
				exit 1
			fi
		done

		echo "ci.sh --guard-self-test: both guards cleared this host - the OPAL version gate passed, and the mod_h323 guard returned '$H323_GUARD_VERDICT' for $H323_GUARD_LIBPT"
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

# Arm 5: the CVE-2013-1864 verdict, against every outcome it can reach.
#
# Arms 1 to 4 are all a healthy host can show.  This one covers what it cannot: a host
# whose PTLib is actually vulnerable, a host whose PTLib predates the ceiling API, and -
# the outcome this project's own hosts depend on - a PTLib built without expat, which
# carries no PXML parser and therefore no entity expander for the advisory to be about.
# Only one of those five verdicts is ever observable at a time, so the other four would
# be dead code proven by nothing, and the FAIL-CLOSED branch would be the one never
# exercised while being the one whose failure is a vulnerable toolkit entering a build.
#
# Each outcome is reached with a scratch stub SDK and environment isolation only:
# h323_probe_search_prefix() points the probes at the stub and PKG_CONFIG_LIBDIR REPLACES
# pkg-config's search path so no installed .pc file is read.  Nothing is installed, no
# compiler is replaced and no real toolkit is touched; the stub lives in the self-test
# root and dies with it.  The arm additionally asserts that the guard resolved the STUB's
# libpt, because a verdict about the host's own library would be an accident that happened
# to agree.
#
# The two safe verdicts must ENABLE the module and the two unsafe ones must leave the
# module list byte-identical and say CVE-2013-1864 while doing it - the same pair of
# postconditions the unit-test arm declares, asserted against a verdict this host cannot
# produce on its own.
guard_self_test_cve_verdict()
{
	local root="$1"
	local flavour="$2"
	local expected="$3"
	local expected_status="$4"
	local dir="$root/cve-$flavour"
	local sdk="$dir/sdk"
	local log="$dir/guard.log"
	local template
	local after

	guard_self_test_scratch_list "$dir" || return 1
	mkdir -p "$sdk" || return 1

	h323_probe_stub_sdk "$sdk" "$flavour" || return 1

	template=$(guard_self_test_checksum "$GUARD_SELF_TEST_TEMPLATE") || return 1

	(
		cd "$dir" || exit 1

		PKG_CONFIG_LIBDIR="$sdk/lib/pkgconfig"
		export PKG_CONFIG_LIBDIR
		unset PKG_CONFIG_PATH

		H323_PROBE_SCRATCH_PARENT="$dir"

		h323_probe_search_prefix "$sdk" || exit 1

		h323_toolkit_available 2> "$log"
		status=$?

		if [ "$H323_GUARD_VERDICT" != "$expected" ]; then
			echo "Error: the mod_h323 guard called a '$flavour' PTLib '$H323_GUARD_VERDICT', expected '$expected'" >&2
			cat "$log" >&2
			exit 1
		fi

		if [ "$status" != "$expected_status" ]; then
			echo "Error: the mod_h323 guard returned $status for verdict '$H323_GUARD_VERDICT', expected $expected_status" >&2
			cat "$log" >&2
			exit 1
		fi

		# Every verdict below 'absent' is only meaningful if the capability probe really
		# built and linked against the stub, so that is asserted rather than assumed
		if [ "$H323_GUARD_LINKABLE" != 'yes' ]; then
			echo "Error: the capability probe could not compile and link against the '$flavour' stub SDK, so no advisory verdict was ever reached" >&2
			cat "$log" >&2
			exit 1
		fi

		case "$H323_GUARD_LIBPT" in
			"$sdk"/*) ;;
			*)
				echo "Error: the guard resolved '${H323_GUARD_LIBPT:-no libpt}' rather than the '$flavour' stub SDK under $sdk, so its verdict is about the wrong library" >&2
				exit 1
				;;
		esac

		if [ "$expected_status" = 0 ]; then
			enable_module_for_tests 'endpoints/mod_h323' || exit 1
		else
			require_module_disabled_for_tests 'endpoints/mod_h323' || exit 1

			if ! grep -q '^ci\.sh: .*CVE-2013-1864' "$log"; then
				echo "Error: the guard refused a '$flavour' PTLib without naming CVE-2013-1864, so the refusal cannot be attributed" >&2
				cat "$log" >&2
				exit 1
			fi
		fi
	) || return 1

	# A refusal must leave the list strictly alone, and "left commented out" and
	# "uncommented and then commented again" produce the same grep result
	if [ "$expected_status" != 0 ]; then
		after=$(guard_self_test_checksum "$dir/modules.conf") || return 1

		if [ "$template" != "$after" ]; then
			echo "Error: the refused '$flavour' endpoints/mod_h323 guard changed the module list (sha256 $template -> $after)" >&2
			return 1
		fi
	fi

	return 0
}

guard_self_test_cve_verdicts()
{
	local root="$1"

	guard_self_test_cve_verdict "$root" 'bounded' 'clear' 0 || return 1
	guard_self_test_cve_verdict "$root" 'no_parser' 'clear_no_parser' 0 || return 1
	guard_self_test_cve_verdict "$root" 'unbounded' 'vulnerable' 1 || return 1
	guard_self_test_cve_verdict "$root" 'no_ceiling_api' 'vulnerable' 1 || return 1

	return 0
}

# Run all five arms and report each one.
#
# The first failure stops the run: once a guard has been shown not to behave the way this
# script declares, the remaining verdicts describe a script that is already wrong.  Each
# arm works on its own scratch list under the same root, so no arm can pass or fail
# because of another arm's side effects.
guard_self_test_arms()
{
	local root="$1"

	guard_self_test_h323_negative "$root"
	guard_self_test_report $? 'arm 1/5 mod_h323 guard negative' \
		'a forced-fail compile+link probe leaves endpoints/mod_h323 disabled and the module list byte-identical' || return 1

	guard_self_test_opal_negative "$root"
	guard_self_test_report $? 'arm 2/5 mod_opal guard negative' \
		'an empty PKG_CONFIG_LIBDIR leaves endpoints/mod_opal disabled and the module list byte-identical' || return 1

	guard_self_test_guards_positive "$root"
	guard_self_test_report $? 'arm 3/5 both guards positive on this host' \
		'the OPAL version gate and the complete mod_h323 guard - CVE-2013-1864 verdict included - both CLEAR this host, and each endpoint enablement leaves exactly one active line' || return 1

	guard_self_test_xml_curl_unconditional "$root"
	guard_self_test_report $? 'arm 4/5 mod_xml_curl arm unconditional' \
		'xml_int/mod_xml_curl is enabled outside both capability gates and idempotently, and the exactly-one-active-line postcondition refuses a corrupted list' || return 1

	guard_self_test_cve_verdicts "$root"
	guard_self_test_report $? 'arm 5/5 mod_h323 CVE-2013-1864 verdict matrix' \
		'against scratch stub SDKs, a bounded and an expat-less PTLib are cleared and enable the module, while an unbounded one and one with no ceiling API are refused by name and leave the module list byte-identical' || return 1

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

	# Every directory the capability and advisory probes created as well, not just the
	# root: the arms hand them this root as their scratch parent, so they are normally
	# inside it, and ci_scratch_cleanup() covers the one case where they are not - a
	# probe reached outside an arm, on the default parent.
	ci_scratch_cleanup

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
		echo "ci.sh --guard-self-test: all 5 arms passed"
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
