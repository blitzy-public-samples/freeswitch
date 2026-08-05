#!/usr/bin/env bash

### shfmt -w -s -ci -sr -kp -fn build/provision_endpoint_toolkits.sh

#------------------------------------------------------------------------------
# Reproducible provisioning of the optional H.323 and OPAL endpoint toolkits
#------------------------------------------------------------------------------
#
# WHY THIS SCRIPT EXISTS
#
# endpoints/mod_h323 and endpoints/mod_opal are the only two FreeSWITCH modules
# whose build inputs cannot be obtained from a distribution package: Debian
# ships no libopal-dev, and libopenh323-dev / libh323plus-dev / libpt-dev
# resolve to no installable candidate either, even though
# debian/control-modules:355 and :364 declare all four.  Every host that
# carries these toolkits therefore carries a HAND INSTALLED toolkit, which is
# not reproducible: a rebuilt CI image or a fresh production host comes up
# without them, and the capability guards in ci.sh then quietly leave both
# modules disabled.  This script closes that gap.  It installs the exact
# versions this project was validated against, from pinned immutable refs, it
# is safe to re-run, and it refuses to install onto a PTLib that cannot be
# cleared of CVE-2013-1864.
#
# WHAT IS PINNED  (Appendix D of blitzy/documentation/Project Guide.md)
#
#   PTLib 2.10.9      + H323Plus 1.28.0      ->  the mod_h323 stack
#   PTLib 2.12-beta10 + OPAL     3.12.10     ->  the mod_opal stack
#
# Every component is pinned to an immutable COMMIT as well as to a
# human-readable ref.  A branch tip is not a version: both SourceForge branches
# below still move, and H323Plus publishes no version tag at all, so the commit
# is the only honest pin.
#
# WHY NOT build/buildopal.sh
#
# build/buildopal.sh is the MODEL for this script's flow - locate the tree from
# $0, export PKG_CONFIG_PATH, ./configure --disable-plugins --prefix=..., make,
# sudo make install - and it is left byte for byte as it is.  It cannot serve as
# reproducible provisioning itself, for three reasons.  It checks out
# https://svn.code.sf.net/p/opalvoip/code/{ptlib,opal} (build/buildopal.sh:45
# and :53) and that SourceForge Subversion service no longer answers - the
# `svn' client the script demands is beside the point.  Its version pins are
# commented out (build/buildopal.sh:29-30), so it resolves to `trunk' and
# installs whatever upstream happens to be that day.  And it knows nothing of
# H323Plus, which mod_h323 needs in addition to PTLib (-lopenh323, and
# /usr/include/openh323/h323.h).  The sources that actually produced this
# project's toolkits were the SourceForge GIT mirrors plus two GitHub mirrors,
# which is what the table below pins.
#
# PREFIXES, AND A DIVERGENCE THAT IS DELIBERATE
#
# The defaults are the prefixes this project's documentation names:
#
#   --ptlib-prefix  /usr/local     PTLib 2.10.9 + H323Plus 1.28.0
#   --opal-prefix   /opt/opalvoip  PTLib 2.12-beta10 + OPAL 3.12.10
#
# Both are overridable, and they have to be, because the host this was written
# against diverges from the first one: its H.323 stack is installed under /usr
# (/usr/lib/libpt.so.2.10.9, /usr/lib/libopenh323.so, /usr/include/openh323,
# /usr/lib/pkgconfig/ptlib.pc) while Appendix D records /usr/local/lib.  A
# script that decided "already provisioned" by testing for a hardcoded prefix
# would reinstall a toolkit that is present, correct and in use.  Idempotence is
# therefore decided by CAPABILITY DETECTION - can mod_h323's own compile and
# link inputs be satisfied, does pkg-config answer for an OPAL new enough for
# the #error in src/mod/endpoints/mod_opal/mod_opal.h:41 - which makes this
# script exit 0 on that host, and on any other layout that genuinely works.
#
# Reusing ci.sh's probe instead of reimplementing it has one consequence worth
# stating plainly: that probe compiles against /usr/include/openh323 and links
# -L/usr/lib (ci.sh:69 and :91) because that is where mod_h323 itself looks
# (src/mod/endpoints/mod_h323/Makefile.am:6 and :10).  An H.323 stack therefore
# has to be reachable on the compiler's and the loader's default search paths
# before it counts as provisioned - which is precisely why this project's host
# installed it under /usr.  Provision with --ptlib-prefix=/usr to reproduce that
# layout, or add the chosen prefix to /etc/ld.so.conf.d and to the compiler's
# include path; the post-install verification names this rather than failing
# mutely.
#
# THE CVE-2013-1864 GATE
#
# PTLib's PXML parser expanded internal entities with no ceiling before 2.10.10,
# so a "billion laughs" document makes any consumer allocate until it is killed.
# ci.sh already owns the behavioural probe for this (ci.sh:94-140, inside
# h323_toolkit_available), and a second copy of a security probe is a second
# thing to get wrong: this script SOURCES that probe rather than reimplementing
# it, and refuses to install when it reports a PTLib that does not bound entity
# expansion or a linkage that cannot be established at all.
#
# MODES
#
#   (default)                    provision what is missing, then verify
#   --uninstall-check, --dry-run report what WOULD be fetched, built, installed
#                                and where, plus what an uninstall would have to
#                                remove; change nothing
#   --help                       usage
#
# EXIT STATUS
#
#   0  provisioned, or already provisioned, or report produced
#   2  usage error
#   3  REFUSED - the CVE-2013-1864 gate did not clear the PTLib that would be
#      linked, or a post-install verification regressed
#   4  provisioning failed (fetch, configure, build or install)
#
# This script never prompts.  It is safe to run non-interactively, it removes
# every scratch directory it creates through a trap, and it touches nothing
# outside the two prefixes and the source directory it reports.
#------------------------------------------------------------------------------

# No `set -e'.  The probe helpers below deliberately run commands that are
# EXPECTED to fail (that is what a probe is), and an errexit shell turns the
# expected failure of a guard into an unexplained exit before the guard can
# report its verdict.  Every command that can fail is checked explicitly
# instead.  `set -u' stays on: an unset variable in a path that is about to be
# handed to rm or install is the one class of bug this script must not have.
set -u

readonly PROG='provision_endpoint_toolkits'

readonly EX_OK=0
readonly EX_USAGE=2
readonly EX_REFUSED=3
readonly EX_PROVISION=4

# Defaults.  Environment equivalents exist so the script can be driven from a
# Dockerfile or a CI job without argument plumbing; command line arguments win.
PTLIB_PREFIX="${PTLIB_PREFIX:-/usr/local}"
OPAL_PREFIX="${OPAL_PREFIX:-/opt/opalvoip}"
# Retained sources live outside the FreeSWITCH working tree on purpose.
# build/buildopal.sh checks out into $FS_DIR/libs, which leaves two large
# untracked source trees inside the repository; the host this pins keeps them in
# /opt/src instead, and a provisioning host wants them retained so a rebuild
# does not refetch.
SRC_ROOT="${PROVISION_SRC_ROOT:-/opt/src}"

MODE='provision'

# The minimum OPAL that mod_opal will compile against at all - the module's own
# header stops the build below it (src/mod/endpoints/mod_opal/mod_opal.h:41-42),
# and ci.sh:276 gates enablement on the same number.  Kept identical to both on
# purpose: a provisioning script that installs an OPAL the module then rejects
# has provisioned nothing.
readonly OPAL_MIN_VERSION='3.12.8'

#------------------------------------------------------------------------------
# The pinned component table
#
# One record per component, in install order.  The order is a dependency order,
# not a preference: H323Plus configures against an installed PTLib, and OPAL
# configures against its own bundled PTLib 2.12, so each pair must be installed
# before the component that consumes it.
#
# CONFIGURE flags are not invented here.  They are the flags that produced the
# toolkits this project was validated against, recovered from the config.log /
# config.status of the retained sources, which is why they are unusually
# specific.  Two of them carry weight beyond taste:
#
#   --disable-expat  removes PTLib's PXML parser altogether, and with it the
#                    entity expander CVE-2013-1864 is about.  It is pinned as
#                    defence in depth, not as a substitute for the gate: the
#                    behavioural probe still has to clear whatever is installed.
#   --disable-plugins  matches build/buildopal.sh:48 and :55, and keeps the OPAL
#                    stack from pulling device plugins a server does not want.
#------------------------------------------------------------------------------

readonly COMPONENT_ORDER=(ptlib_h323 h323plus ptlib_opal opal)

declare -A COMPONENT_NAME=(
	[ptlib_h323]='PTLib'
	[h323plus]='H323Plus'
	[ptlib_opal]='PTLib (bundled with OPAL)'
	[opal]='OPAL'
)

declare -A COMPONENT_VERSION=(
	[ptlib_h323]='2.10.9'
	[h323plus]='1.28.0'
	[ptlib_opal]='2.12-beta10'
	[opal]='3.12.10'
)

declare -A COMPONENT_REPO=(
	[ptlib_h323]='https://github.com/willamowius/ptlib.git'
	[h323plus]='https://github.com/willamowius/h323plus.git'
	[ptlib_opal]='https://git.code.sf.net/p/opalvoip/ptlib'
	[opal]='https://git.code.sf.net/p/opalvoip/opal'
)

# Human-readable ref, purely for the report.  COMPONENT_COMMIT is what is
# actually checked out.
declare -A COMPONENT_REF=(
	[ptlib_h323]='tag v2_10_9_6'
	[h323plus]='branch master'
	[ptlib_opal]='branch v2_12'
	[opal]='branch v3_12'
)

declare -A COMPONENT_COMMIT=(
	[ptlib_h323]='c01afdc78cc4fb56e497b04aea69ef575fc53bf0'
	[h323plus]='ea2072978f0334583b550dbfc8b5f6eb7303cbef'
	[ptlib_opal]='10503462abe31a6c288a39f6f894a5ceb367abbc'
	[opal]='a9091f39fcb16f66da2b1134646f341a741a1d15'
)

# Which prefix each component installs into - resolved at run time, because both
# prefixes are overridable.
declare -A COMPONENT_STACK=(
	[ptlib_h323]='h323'
	[h323plus]='h323'
	[ptlib_opal]='opal'
	[opal]='opal'
)

declare -A COMPONENT_SRCDIR=(
	[ptlib_h323]='ptlib-h323plus'
	[h323plus]='h323plus'
	[ptlib_opal]='ptlib-opal'
	[opal]='opal'
)

declare -A COMPONENT_CONFIGURE=(
	[ptlib_h323]='--disable-openldap --disable-sasl --disable-odbc --disable-sdl --disable-lua --disable-expat --enable-v4l=no --enable-avc=no --enable-dc=no'
	[h323plus]=''
	[ptlib_opal]='--disable-plugins --disable-openldap --disable-sasl --disable-openssl --disable-sdl --disable-lua --disable-expat --disable-odbc --disable-pcap --disable-alsa --disable-esd --disable-oss --disable-pulse --disable-shmaudio --disable-v4l --disable-v4l2'
	[opal]='--disable-plugins'
)

# H323Plus builds its shared library under the `opt' target and its own install
# rule depends on it (its Makefile:99 reads `install: opt'), so the default
# target is not enough.  PTLib and OPAL build with the bare default target, as
# build/buildopal.sh:49 and :56 do.
declare -A COMPONENT_BUILD_TARGET=(
	[ptlib_h323]=''
	[h323plus]='opt'
	[ptlib_opal]=''
	[opal]=''
)

# Prefix-relative artifacts each component owns, used by --uninstall-check to
# report what an uninstall would have to remove.  Globs are expanded at report
# time, and an entry that does not exist is reported as absent rather than
# silently skipped: a half-installed toolkit produces the most confusing build
# failures of all, so it is worth seeing.
declare -A COMPONENT_ARTIFACTS=(
	[ptlib_h323]='lib/libpt.so* lib/libpt_s.a lib/pkgconfig/ptlib.pc bin/ptlib-config include/ptlib.h include/ptlib include/ptclib share/ptlib'
	[h323plus]='lib/libh323_*.so* lib/libopenh323.so include/openh323'
	[ptlib_opal]='lib/libpt.so* lib/libpt_s.a lib/pkgconfig/ptlib.pc include/ptbuildopts.h include/ptlib.h include/ptlib include/ptclib'
	[opal]='lib/libopal.so* lib/libopal_s.a lib/pkgconfig/opal.pc include/opal'
)

#------------------------------------------------------------------------------
# Output helpers.  Everything the operator has to act on goes to stderr so that
# a caller can capture the report on stdout without losing the diagnosis.
#------------------------------------------------------------------------------

say()
{
	printf '%s\n' "$*"
}

note()
{
	printf '%s: %s\n' "$PROG" "$*"
}

warn()
{
	printf '%s: %s\n' "$PROG" "$*" >&2
}

# The named refusal.  The identifier and the advisory are both in the first
# line, deliberately: this string is what an image build log gets grepped for,
# so it has to be stable and it has to say which advisory stopped the install.
refuse()
{
	printf '%s: REFUSED (CVE-2013-1864): %s\n' "$PROG" "$1" >&2
	shift
	while [ "$#" -gt 0 ]; do
		printf '%s:   %s\n' "$PROG" "$1" >&2
		shift
	done
}

#------------------------------------------------------------------------------
# Scratch directories.  Every one this script creates is registered here and
# removed by the trap, including on the refusal paths - a probe that leaves a
# compiled artifact behind in /tmp on every CI run is a slow leak, and a probe
# that leaves one behind on the FAILURE path is the one nobody notices.
#------------------------------------------------------------------------------

declare -a SCRATCH_DIRS=()

# Sets SCRATCH_LAST rather than printing the path, because a command
# substitution would register the directory in a subshell and the trap in THIS
# shell would then never remove it.
SCRATCH_LAST=''

new_scratch_dir()
{
	local dir

	dir=$(mktemp -d 2> /dev/null) || return 1

	SCRATCH_DIRS+=("$dir")
	SCRATCH_LAST="$dir"

	return 0
}

# shellcheck disable=SC2317  # reached only through the traps installed below
remove_scratch_dirs()
{
	local dir

	# bash 4.4 and later expand an empty array under `set -u' without error, so
	# this loop needs no guard against SCRATCH_DIRS being empty.
	for dir in "${SCRATCH_DIRS[@]}"; do
		# Guarded rather than trusted: an empty or relative element here would
		# make this an `rm -rf' of something else entirely.
		case "$dir" in
			/tmp/*) rm -rf "$dir" ;;
			/var/tmp/*) rm -rf "$dir" ;;
			*) warn "refusing to remove unexpected scratch path '$dir'" ;;
		esac
	done

	SCRATCH_DIRS=()
}

trap 'remove_scratch_dirs' EXIT
trap 'remove_scratch_dirs; exit 130' INT
trap 'remove_scratch_dirs; exit 143' TERM

#------------------------------------------------------------------------------
# Usage
#------------------------------------------------------------------------------

usage()
{
	cat <<- USAGE
		Usage: $0 [--dry-run | --uninstall-check] [--ptlib-prefix DIR] [--opal-prefix DIR]
		       $0 --help

		Provision the pinned H.323 and OPAL toolkits that endpoints/mod_h323 and
		endpoints/mod_opal build against, idempotently, and refuse to do so on a
		PTLib that cannot be cleared of CVE-2013-1864.

		Modes:
		  (default)           Provision whatever is missing, then verify.  Exits 0
		                      without installing anything when both stacks are
		                      already present and usable.
		  --uninstall-check   Report what WOULD be fetched, built and installed and
		  --dry-run           where, and what an uninstall would have to remove.
		                      Changes nothing.  The two spellings are aliases.
		  --help, -h          This text.

		Options:
		  --ptlib-prefix DIR  Install prefix for PTLib ${COMPONENT_VERSION[ptlib_h323]} + H323Plus ${COMPONENT_VERSION[h323plus]}
		                      (default: /usr/local, env PTLIB_PREFIX)
		  --opal-prefix DIR   Install prefix for OPAL ${COMPONENT_VERSION[opal]} and its bundled
		                      PTLib ${COMPONENT_VERSION[ptlib_opal]}
		                      (default: /opt/opalvoip, env OPAL_PREFIX)
		  --src-root DIR      Where pinned sources are checked out and retained
		                      (default: /opt/src, env PROVISION_SRC_ROOT)

		Pinned components:
		$(component_pin_lines '  ')

		Exit status:
		  0  provisioned, already provisioned, or report produced
		  2  usage error
		  3  REFUSED - the CVE-2013-1864 gate did not clear the PTLib that would be
		     linked, or a post-install verification regressed
		  4  provisioning failed (fetch, configure, build or install)
	USAGE
}

# One line per pinned component, shared by --help and by the reports so the two
# can never disagree about what is pinned.
component_pin_lines()
{
	local indent="$1"
	local id

	for id in "${COMPONENT_ORDER[@]}"; do
		printf '%s%-26s %-13s %s @ %s\n' \
			"$indent" \
			"${COMPONENT_NAME[$id]}" \
			"${COMPONENT_VERSION[$id]}" \
			"${COMPONENT_REPO[$id]}" \
			"${COMPONENT_COMMIT[$id]}"
	done
}

#------------------------------------------------------------------------------
# Argument parsing.  getopts is not used because every mode here is a LONG
# option, which getopts does not implement: it would take `--dry-run' apart a
# character at a time and land in its invalid-option arm.  Both spellings of each
# option are accepted, `--opt VALUE' and `--opt=VALUE', because a caller writing
# either has been unambiguous.  An unrecognised or empty-valued argument is a
# usage error rather than something to skip over: skipping it would run a
# PROVISIONING pass, with installs, that the caller did not ask for.
#------------------------------------------------------------------------------

parse_args()
{
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--dry-run | --uninstall-check)
				MODE='report'
				;;
			--ptlib-prefix)
				if [ "$#" -lt 2 ] || [ -z "$2" ]; then
					warn "option '$1' requires a directory"
					return 1
				fi
				PTLIB_PREFIX="$2"
				shift
				;;
			--ptlib-prefix=*)
				PTLIB_PREFIX="${1#*=}"
				if [ -z "$PTLIB_PREFIX" ]; then
					warn "option '--ptlib-prefix' requires a directory"
					return 1
				fi
				;;
			--opal-prefix)
				if [ "$#" -lt 2 ] || [ -z "$2" ]; then
					warn "option '$1' requires a directory"
					return 1
				fi
				OPAL_PREFIX="$2"
				shift
				;;
			--opal-prefix=*)
				OPAL_PREFIX="${1#*=}"
				if [ -z "$OPAL_PREFIX" ]; then
					warn "option '--opal-prefix' requires a directory"
					return 1
				fi
				;;
			--src-root)
				if [ "$#" -lt 2 ] || [ -z "$2" ]; then
					warn "option '$1' requires a directory"
					return 1
				fi
				SRC_ROOT="$2"
				shift
				;;
			--src-root=*)
				SRC_ROOT="${1#*=}"
				if [ -z "$SRC_ROOT" ]; then
					warn "option '--src-root' requires a directory"
					return 1
				fi
				;;
			--help | -h)
				MODE='help'
				;;
			*)
				warn "unrecognised argument '$1'"
				return 1
				;;
		esac
		shift
	done

	return 0
}

#------------------------------------------------------------------------------
# Locate the FreeSWITCH tree from the script's own path, the way
# build/buildopal.sh:17-19 does.  ci.sh has to be found relative to the script
# rather than relative to the caller's working directory: the CVE probe is
# sourced from it, and a provisioning script that silently skipped the gate
# because it was invoked from another directory would be worse than one that
# refused.
#------------------------------------------------------------------------------

FS_DIR=''
CI_SCRIPT=''

resolve_tree()
{
	local script_dir

	script_dir=$(cd -- "$(dirname -- "$0")" > /dev/null 2>&1 && pwd) || return 1
	FS_DIR=$(cd -- "$script_dir/.." > /dev/null 2>&1 && pwd) || return 1
	CI_SCRIPT="$FS_DIR/ci.sh"

	return 0
}

#------------------------------------------------------------------------------
# PKG_CONFIG_PATH
#
# Both stacks ship a ptlib.pc, so the ORDER decides which PTLib the OPAL build
# resolves, and the OPAL stack's own 2.12 has to win there or mod_opal links one
# PTLib against headers from another.  The OPAL prefix therefore comes first.
#
# The value is exported for this script's own children (the sourced probe runs
# pkg-config, and so does every ./configure below) AND echoed, because the
# caller's shell is the one that has to carry it afterwards.
#
# On the host this pins, nothing has to carry it at all: the two OPAL .pc files
# are symlinked into /usr/local/lib/pkgconfig, which is already on pkg-config's
# default search path, and /opt/opalvoip/lib is on the loader's path through
# /etc/ld.so.conf.d/opalvoip.conf.  That arrangement is what lets
# src/mod/endpoints/mod_opal/Makefile.am:4 hardcode PKG_DIR=/usr/local/lib/pkgconfig
# and still find OPAL, so a provisioning host wants to reproduce it; the report
# prints the exact commands.
#------------------------------------------------------------------------------

PKG_CONFIG_PATH_VALUE=''

compose_pkg_config_path()
{
	PKG_CONFIG_PATH_VALUE="$OPAL_PREFIX/lib/pkgconfig:$PTLIB_PREFIX/lib/pkgconfig"

	export PKG_CONFIG_PATH="$PKG_CONFIG_PATH_VALUE"

	return 0
}

#------------------------------------------------------------------------------
# The CVE-2013-1864 probe, reused from ci.sh by SOURCING it
#
# ci.sh is a CI driver, not a library, and sourcing it is hostile in three
# specific ways.  Each is neutralised deliberately, inside a subshell, so none of
# it can leak into the provisioning pass:
#
#   1. ci.sh:23-32 runs getopts over "$@".  Sourcing with an explicit `--' ends
#      its option parsing immediately, so this script's own arguments -
#      --dry-run, --ptlib-prefix and the rest - can never reach it and can never
#      trip its `?)' arm, which calls display_usage and exits.
#
#   2. ci.sh:426-468 is a dispatcher and EVERY arm of it exits: an unset $CODE
#      falls through to `*) exit 1', and a recognised one runs a CI action.
#      CODE, ACTION, TYPE and PATH_TO_CODE are unset before sourcing - not for
#      tidiness, but because a caller whose environment happens to carry
#      CODE=freeswitch ACTION=configure would otherwise have this script run
#      ./bootstrap.sh and ./configure over the tree as a side effect of asking
#      whether PTLib is safe.
#
#   3. That dispatcher exit would still end the shell that sourced the file.  A
#      shell FUNCTION named `exit' is therefore defined for the duration of the
#      source: bash resolves functions ahead of the exit builtin, so the
#      dispatcher's exit becomes a no-op, the source completes, and every ci.sh
#      function is defined.  `unset -f exit' restores the builtin before the
#      probe is called, so nothing else runs with exit stubbed out.  This was
#      chosen over an EXIT trap because it leaves the probe call in ordinary
#      control flow, where its status and its output are trivially captured.
#
# The mechanism does not care whether ci.sh has grown a source guard: with one,
# the file returns before the dispatcher and the `exit' function is simply never
# called; without one, it is what keeps the source alive.  Both were exercised.
#
# The subshell's last line is a sentinel carrying the probe's status.  A missing
# sentinel means the probe never completed - ci.sh absent, unreadable, renamed,
# the function gone, or an exit this script failed to neutralise - and that is
# an UNVERIFIABLE PTLib, never a pass.
#------------------------------------------------------------------------------

readonly PROBE_SENTINEL='__PROVISION_CVE_PROBE_STATUS__'

H323_PROBE_OUTPUT=''
H323_PROBE_STATUS=''

run_ci_h323_probe()
{
	local captured
	local wrapper_status
	local sentinel

	H323_PROBE_OUTPUT=''
	H323_PROBE_STATUS=''

	if [ ! -r "$CI_SCRIPT" ]; then
		warn "cannot read $CI_SCRIPT, so the CVE-2013-1864 probe cannot be sourced"
		return 1
	fi

	captured=$(
		set +u
		unset CODE ACTION TYPE PATH_TO_CODE

		# shellcheck disable=SC2317  # called indirectly, by ci.sh's dispatch tail
		exit()
		{
			return 0
		}

		# shellcheck source=/dev/null
		. "$CI_SCRIPT" --

		unset -f exit

		if ! declare -F h323_toolkit_available > /dev/null 2>&1; then
			exit 127
		fi

		h323_toolkit_available 2>&1
		printf '%s=%s\n' "$PROBE_SENTINEL" "$?"
	)
	# Only meaningful when the sentinel is missing: it then says how the wrapper
	# died rather than what the probe decided.
	wrapper_status=$?

	sentinel=$(printf '%s\n' "$captured" |
		sed -n "s/^${PROBE_SENTINEL}=\\([0-9][0-9]*\\)\$/\\1/p" | tail -n 1)

	# Hand the caller only what the probe itself said.
	H323_PROBE_OUTPUT=$(printf '%s\n' "$captured" | grep -v "^${PROBE_SENTINEL}=")

	if [ -z "$sentinel" ]; then
		warn "the CVE-2013-1864 probe in $CI_SCRIPT did not complete (wrapper exited $wrapper_status)"
		return 1
	fi

	H323_PROBE_STATUS="$sentinel"

	return 0
}

#------------------------------------------------------------------------------
# Is PTLib's XML parser reachable at all?
#
# One of ci.sh's verdicts is genuinely indeterminate.  "exposes no XML entity
# ceiling, so CVE-2013-1864 cannot be ruled out" (ci.sh:122) is emitted when the
# entity probe does not COMPILE, and that happens for two opposite reasons:
# either the toolkit is a pre-2.10.10 PTLib whose PXML parser has no
# SetMaxEntityLength, which is exactly the vulnerable configuration, or it was
# built without expat, in which case ptclib/pxml.h declares PXML as a NAMESPACE
# holding a single string helper and the library contains no XML parser - and so
# no entity expander - at all.
#
# Collapsing those two together would either refuse a toolkit that cannot be
# vulnerable or clear one that is, so they are separated by a question ci.sh does
# not ask: does this SDK hand a consumer a PXML TYPE?  The check compiles a
# translation unit that needs PXML to be complete, with the flags mod_h323 is
# itself built with (src/mod/endpoints/mod_h323/Makefile.am:6, :8 and :13, which
# ci.sh's probe mirrors) - it has to see the SAME SDK the probe saw or its answer
# means nothing.  It is not a second copy of the behavioural probe: no entity
# ceiling is set and no entity document is parsed.
#
# Returns 0  the parser is reachable, so an absent ceiling API is the unbounded
#            pre-2.10.10 expander
#         1  the parser is not reachable from this SDK, so CVE-2013-1864's code
#            path cannot be entered by anything built against it
#         2  undecidable - no compiler, no scratch directory - which the caller
#            turns into a refusal
#------------------------------------------------------------------------------

ptlib_xml_parser_reachable()
{
	local -a compiler
	local -a flags=(-I/usr/include/openh323 -DPTRACING=1 -D_REENTRANT -fno-exceptions)
	local probe_dir

	read -ra compiler <<< "${CXX:-g++}"

	command -v "${compiler[0]}" > /dev/null 2>&1 || return 2

	# Match configure.ac's IS64BITLINUX conditional, as ci.sh:79-81 does; a probe
	# built with flags the module does not use proves nothing.
	if [ "$(uname -m)" = 'x86_64' ]; then
		flags+=(-DP_64BIT)
	fi

	new_scratch_dir || return 2
	probe_dir="$SCRATCH_LAST"

	# sizeof needs a COMPLETE type, so this compiles only when PXML is the parser
	# class and fails when it is the namespace an expat-less PTLib ships.
	printf '#include <ptlib.h>\n#include <ptclib/pxml.h>\nint main(void) { return (int) sizeof(PXML); }\n' \
		> "$probe_dir/parser.cpp" || return 2

	if "${compiler[@]}" "${flags[@]}" -fsyntax-only "$probe_dir/parser.cpp" > /dev/null 2>&1; then
		return 0
	fi

	return 1
}

#------------------------------------------------------------------------------
# The gate itself.  Sets CVE_VERDICT to exactly one of:
#
#   clear            the probe ran the entity document and the library bounded it
#   clear_no_parser  the library has no XML parser to be vulnerable with
#   vulnerable       the library ignored the ceiling it was given, or exposes the
#                    parser with no ceiling API at all
#   unverifiable     the probe could not be sourced, could not resolve which
#                    libpt would load, or could not be decided - fail closed
#   absent           there is no H.323 PTLib here to judge, which is what this
#                    script exists to fix rather than something to refuse
#------------------------------------------------------------------------------

CVE_VERDICT=''
CVE_DETAIL=''
H323_TOOLKIT_LINKABLE='no'
H323_RESOLVED_LIBPT=''

cve_gate()
{
	local reachable

	CVE_VERDICT=''
	CVE_DETAIL=''
	H323_TOOLKIT_LINKABLE='no'
	H323_RESOLVED_LIBPT=''

	if ! run_ci_h323_probe; then
		CVE_VERDICT='unverifiable'
		CVE_DETAIL="the probe could not be sourced from $CI_SCRIPT"
		return 0
	fi

	# A structural inference, not a guess: every message h323_toolkit_available
	# can print is printed only AFTER its compile-and-link probe has succeeded
	# (ci.sh:90-140), and it returns 1 in silence when pkg-config has no ptlib,
	# when mktemp fails, or when that compile and link fails.  Output therefore
	# means mod_h323's own compile and link inputs are satisfied - which is
	# precisely the capability this script must detect to be idempotent - and
	# silence with a non-zero status means the H.323 stack is absent or partial.
	if [ "$H323_PROBE_STATUS" = '0' ] || [ -n "$H323_PROBE_OUTPUT" ]; then
		H323_TOOLKIT_LINKABLE='yes'
	fi

	# ci.sh prints the resolved libpt on its success line; keep it for the summary
	# so the report names the library that was actually cleared.
	H323_RESOLVED_LIBPT=$(printf '%s\n' "$H323_PROBE_OUTPUT" |
		sed -n 's|^.*will link \(/[^ ]*\).*$|\1|p' | tail -n 1)

	if [ "$H323_PROBE_STATUS" = '0' ]; then
		CVE_VERDICT='clear'
		CVE_DETAIL='the probe loaded a bounded-entity document and the library refused it, as a fixed PTLib must'
		return 0
	fi

	case "$H323_PROBE_OUTPUT" in
		*'ignores its XML entity ceiling'*)
			CVE_VERDICT='vulnerable'
			CVE_DETAIL='the library parsed a document that exceeds the entity ceiling it was given'
			;;
		*'cannot establish which libpt'*)
			CVE_VERDICT='unverifiable'
			CVE_DETAIL='the libpt this toolkit would load could not be resolved, and an unverifiable linkage cannot be cleared of the advisory'
			;;
		*'exposes no XML entity ceiling'*)
			ptlib_xml_parser_reachable
			reachable=$?

			case "$reachable" in
				0)
					CVE_VERDICT='vulnerable'
					CVE_DETAIL='the toolkit exposes PTLib PXML parser but no entity ceiling API, which is the unbounded pre-2.10.10 expander'
					;;
				1)
					CVE_VERDICT='clear_no_parser'
					CVE_DETAIL='this PTLib carries no PXML parser (built without expat), so the advisory has no code path in it'
					;;
				*)
					CVE_VERDICT='unverifiable'
					CVE_DETAIL='whether this PTLib exposes an XML parser at all could not be determined'
					;;
			esac
			;;
		*)
			CVE_VERDICT='absent'
			CVE_DETAIL='no H.323 PTLib could be compiled and linked against, so there is nothing here to clear'
			;;
	esac

	return 0
}

# Enforce the gate.  Returns non-zero when provisioning must not proceed, and
# says so with the named refusal.  Called BEFORE any fetch, build or install.
enforce_cve_gate()
{
	case "$CVE_VERDICT" in
		vulnerable)
			refuse 'the PTLib that would be linked does not bound XML entity expansion' \
				"$CVE_DETAIL" \
				'nothing was fetched, built or installed' \
				"remedy: install PTLib 2.10.10 or newer, a build carrying the backported fix, or one built --disable-expat (which this script pins for ${COMPONENT_NAME[ptlib_h323]} ${COMPONENT_VERSION[ptlib_h323]}), then re-run"
			return 1
			;;
		unverifiable)
			refuse 'the PTLib that would be linked cannot be cleared of the advisory' \
				"$CVE_DETAIL" \
				'an indeterminate probe is treated as a refusal, never as a pass' \
				'nothing was fetched, built or installed'
			return 1
			;;
	esac

	return 0
}

#------------------------------------------------------------------------------
# The OPAL side of capability detection.
#
# There is no compile probe here because there does not need to be one: OPAL is
# discoverable through pkg-config on every layout this project has seen, and the
# module's own header settles what "usable" means - mod_opal.h:41-42 stops the
# build below 3.12.8.  ci.sh:276 gates enablement on exactly that number, and
# this script uses the same one so a toolkit it calls provisioned is a toolkit
# ci.sh will then enable.
#------------------------------------------------------------------------------

OPAL_USABLE='no'
OPAL_VERSION=''
OPAL_LIBDIR=''
PTLIB_PKG_VERSION=''
PTLIB_PKG_LIBDIR=''

observe_pkg_config_state()
{
	OPAL_USABLE='no'
	OPAL_VERSION=''
	OPAL_LIBDIR=''
	PTLIB_PKG_VERSION=''
	PTLIB_PKG_LIBDIR=''

	if ! command -v pkg-config > /dev/null 2>&1; then
		warn 'pkg-config is not installed, so neither toolkit can be discovered'
		return 1
	fi

	if pkg-config --exists ptlib > /dev/null 2>&1; then
		PTLIB_PKG_VERSION=$(pkg-config --modversion ptlib 2> /dev/null)
		PTLIB_PKG_LIBDIR=$(pkg-config --variable=libdir ptlib 2> /dev/null)
	fi

	if pkg-config --exists opal > /dev/null 2>&1; then
		OPAL_VERSION=$(pkg-config --modversion opal 2> /dev/null)
		OPAL_LIBDIR=$(pkg-config --variable=libdir opal 2> /dev/null)

		if pkg-config --atleast-version="$OPAL_MIN_VERSION" opal > /dev/null 2>&1; then
			OPAL_USABLE='yes'
		fi
	fi

	return 0
}

# Which prefix a component installs into.
prefix_for()
{
	case "${COMPONENT_STACK[$1]}" in
		h323) printf '%s\n' "$PTLIB_PREFIX" ;;
		*) printf '%s\n' "$OPAL_PREFIX" ;;
	esac
}

# Does the configured PTLib prefix actually carry the H.323 stack's library?
#
# Asked only so the report can SAY when it does not.  It is never allowed to
# decide anything: on the layout this script pins, the answer is no - the stack
# lives under /usr while the documented prefix is /usr/local - and a script that
# reinstalled on that answer would rebuild a working toolkit on every run.
h323_prefix_carries_ptlib()
{
	local path

	for path in "$PTLIB_PREFIX"/lib/libpt.so*; do
		if [ -e "$path" ]; then
			return 0
		fi
	done

	return 1
}

# Is the stack a component belongs to already satisfied?  Per component rather
# than per file, because idempotence here is a capability question (see the
# header): the H.323 stack is judged by whether mod_h323's compile and link
# inputs are satisfied, wherever they live, and the OPAL stack by whether
# pkg-config answers with a version the module accepts.
stack_satisfied()
{
	case "${COMPONENT_STACK[$1]}" in
		h323) [ "$H323_TOOLKIT_LINKABLE" = 'yes' ] ;;
		*) [ "$OPAL_USABLE" = 'yes' ] ;;
	esac
}

#------------------------------------------------------------------------------
# Reports
#------------------------------------------------------------------------------

report_observed_state()
{
	say '-- observed state --'
	say ''
	say "  H.323 stack (endpoints/mod_h323), configured prefix $PTLIB_PREFIX"
	say "    mod_h323 compile+link inputs satisfied : $H323_TOOLKIT_LINKABLE"
	say "    CVE-2013-1864 verdict                  : $CVE_VERDICT"
	say "                                             $CVE_DETAIL"

	if [ -n "$H323_RESOLVED_LIBPT" ]; then
		say "    libpt that would be loaded             : $H323_RESOLVED_LIBPT"
	fi

	if [ "$H323_TOOLKIT_LINKABLE" = 'yes' ] && ! h323_prefix_carries_ptlib; then
		say "    prefix divergence                      : satisfied from OUTSIDE $PTLIB_PREFIX"
		say '                                             idempotence is decided by capability,'
		say '                                             not by prefix, so nothing is reinstalled'
	fi

	say ''
	say "  OPAL stack (endpoints/mod_opal), configured prefix $OPAL_PREFIX"
	say "    opal via pkg-config                    : ${OPAL_VERSION:-not found}"
	say "    usable for mod_opal (>= $OPAL_MIN_VERSION)        : $OPAL_USABLE"

	if [ -n "$OPAL_LIBDIR" ]; then
		say "    opal libdir                            : $OPAL_LIBDIR"
	fi

	say ''
	say "  ptlib via pkg-config                     : ${PTLIB_PKG_VERSION:-not found}${PTLIB_PKG_LIBDIR:+ ($PTLIB_PKG_LIBDIR)}"

	# Worth stating rather than leaving to be discovered: on the layout this
	# script pins, pkg-config answers for the OPAL stack's PTLib 2.12 while
	# mod_h323 links the 2.10.9 one from its own prefix.  Two PTLibs, one
	# pkg-config name.  It is also why the two modules must never be co-loaded.
	if [ -n "$PTLIB_PKG_VERSION" ] && [ -n "$H323_RESOLVED_LIBPT" ]; then
		case "$H323_RESOLVED_LIBPT" in
			"$PTLIB_PKG_LIBDIR"/*) ;;
			*)
				say "  note: two PTLib runtimes are installed - pkg-config resolves ${PTLIB_PKG_VERSION}"
				say "        in ${PTLIB_PKG_LIBDIR}, while mod_h323 links $H323_RESOLVED_LIBPT"
				;;
		esac
	fi

	say ''
}

report_pkg_config_path()
{
	say '-- pkg-config environment --'
	say ''
	say '  Export this so both stacks resolve, OPAL first so its own PTLib wins:'
	say ''
	say "    export PKG_CONFIG_PATH=$PKG_CONFIG_PATH_VALUE"
	say ''
	say '  Or make it unnecessary, which is what the pinned layout does:'
	say ''
	say '    mkdir -p /usr/local/lib/pkgconfig'
	say "    ln -sf $OPAL_PREFIX/lib/pkgconfig/opal.pc  /usr/local/lib/pkgconfig/opal.pc"
	say "    ln -sf $OPAL_PREFIX/lib/pkgconfig/ptlib.pc /usr/local/lib/pkgconfig/ptlib.pc"
	say "    echo $OPAL_PREFIX/lib > /etc/ld.so.conf.d/opalvoip.conf && ldconfig"
	say ''
	say '  /usr/local/lib/pkgconfig is already on pkg-config default search path, and'
	say '  src/mod/endpoints/mod_opal/Makefile.am:4 hardcodes it as PKG_DIR, so with the'
	say '  symlinks in place mod_opal builds with no PKG_CONFIG_PATH at all.'
	say ''
}

# What a provisioning run WOULD do.  Every line is derived from the same tables
# the provisioning path uses, so the report cannot drift from the behaviour.
report_plan()
{
	local id
	local prefix
	local srcdir
	local target

	say '-- plan (nothing is changed in this mode) --'
	say ''

	for id in "${COMPONENT_ORDER[@]}"; do
		prefix=$(prefix_for "$id")
		srcdir="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"
		target="${COMPONENT_BUILD_TARGET[$id]}"

		say "  ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]}"

		if stack_satisfied "$id"; then
			say "    action    : skip - the ${COMPONENT_STACK[$id]} stack is already present and usable"
		else
			say "    action    : fetch, build and install"
		fi

		say "    fetch     : git clone ${COMPONENT_REPO[$id]} $srcdir"
		say "                git -C $srcdir checkout --detach ${COMPONENT_COMMIT[$id]}   (${COMPONENT_REF[$id]})"
		say "    configure : ./configure --prefix=$prefix${COMPONENT_CONFIGURE[$id]:+ ${COMPONENT_CONFIGURE[$id]}}"
		say "    build     : $MAKE${target:+ $target}"
		say "    install   : ${SUDO:+$SUDO }$MAKE install   (into $prefix)"
		say ''
	done

	return 0
}

# What an uninstall would have to remove.  This is the other half of
# --uninstall-check: it answers "is this host carrying these toolkits, and where"
# without touching any of it.
report_uninstall_inventory()
{
	local id
	local prefix
	local pattern
	local path
	local found
	local srcdir

	say '-- uninstall inventory (nothing is removed in this mode) --'
	say ''

	for id in "${COMPONENT_ORDER[@]}"; do
		prefix=$(prefix_for "$id")

		say "  ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} under $prefix"

		for pattern in ${COMPONENT_ARTIFACTS[$id]}; do
			found='no'

			# Unquoted on purpose: these entries are globs.
			for path in $prefix/$pattern; do
				if [ -e "$path" ]; then
					# Symlink targets are resolved in the report because the two
					# stacks cross here: /usr/local/lib/pkgconfig/ptlib.pc is a
					# symlink into the OPAL prefix on the pinned layout, and an
					# uninstall that treated it as a PTLib 2.10.9 file of its own
					# would break the OPAL stack instead.
					if [ -L "$path" ]; then
						say "    present : $path -> $(readlink "$path")"
					else
						say "    present : $path"
					fi

					found='yes'
				fi
			done

			if [ "$found" = 'no' ]; then
				say "    absent  : $prefix/$pattern"
			fi
		done

		srcdir="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"

		if [ -d "$srcdir" ]; then
			say "    retained source : $srcdir"
		else
			say "    retained source : $srcdir (not present)"
		fi

		say ''
	done

	say '  Host integration an uninstall would also have to undo:'

	for path in /usr/local/lib/pkgconfig/opal.pc /usr/local/lib/pkgconfig/ptlib.pc /etc/ld.so.conf.d/opalvoip.conf; do
		if [ -e "$path" ]; then
			say "    present : $path"
		else
			say "    absent  : $path"
		fi
	done

	say ''

	return 0
}

# Name the libraries that actually resolved, which is the only claim about an
# install worth making: a .pc file says what was installed, the loader says what
# will be used.
report_resolved_libraries()
{
	local line

	say '-- resolved libraries --'
	say ''

	if ! command -v ldconfig > /dev/null 2>&1; then
		say '  ldconfig is not available, so loader resolution could not be listed'
		say ''
		return 0
	fi

	line=$(ldconfig -p 2> /dev/null |
		grep -E 'libpt\.so|libopenh323\.so|libh323_|libopal\.so')

	if [ -z "$line" ]; then
		say '  the loader cache lists none of libpt, libopenh323 or libopal'
	else
		printf '%s\n' "$line" | sed 's/^[[:space:]]*/  /'
	fi

	say ''

	return 0
}

print_summary()
{
	local id
	local prefix

	say '-- summary --'
	say ''
	printf '  %-26s %-13s %-14s %-40s %s\n' 'COMPONENT' 'VERSION' 'PREFIX' 'PINNED REF' 'COMMIT'

	for id in "${COMPONENT_ORDER[@]}"; do
		prefix=$(prefix_for "$id")
		printf '  %-26s %-13s %-14s %-40s %s\n' \
			"${COMPONENT_NAME[$id]}" \
			"${COMPONENT_VERSION[$id]}" \
			"$prefix" \
			"${COMPONENT_REF[$id]} of ${COMPONENT_REPO[$id]##*/}" \
			"${COMPONENT_COMMIT[$id]}"
	done

	say ''
	report_resolved_libraries

	return 0
}

#------------------------------------------------------------------------------
# Provisioning
#------------------------------------------------------------------------------

MAKE='make'
SUDO=''

select_tools()
{
	# build/buildopal.sh:14 picks gmake on BSD.  Same choice, spelled as an if so
	# that a failing uname cannot silently select the wrong one.
	if uname -a 2> /dev/null | grep -qi bsd; then
		MAKE='gmake'
	fi

	# Installing into /usr/local or /opt needs root.  build/buildopal.sh:50 runs
	# `sudo make install' unconditionally; here sudo is used only when this
	# process is not already root, and only with -n, because a provisioning
	# script that blocks on a password prompt hangs an image build instead of
	# failing it.
	if [ "$(id -u)" != '0' ]; then
		SUDO='sudo -n'

		# Said now rather than discovered halfway through a build: the report has
		# to show the install command the way it would really run, and a provision
		# run that will fail at the install step should say so before it spends
		# twenty minutes compiling.
		if ! command -v sudo > /dev/null 2>&1 || ! sudo -n true > /dev/null 2>&1; then
			if [ "$MODE" = 'provision' ]; then
				warn 'not running as root and passwordless sudo is unavailable, so the install steps would fail'
			fi
		fi
	fi

	return 0
}

require_build_tools()
{
	local tool

	for tool in "$MAKE" git pkg-config; do
		if ! command -v "$tool" > /dev/null 2>&1; then
			warn "$tool is required to provision the toolkits and is not installed"
			return 1
		fi
	done

	return 0
}

# Fetch a component at its pinned commit.  A detached checkout of a COMMIT, never
# a branch name: build/buildopal.sh installs whatever `trunk' is on the day it
# runs (build/buildopal.sh:29-34), which is the specific non-reproducibility this
# script exists to remove.
fetch_component()
{
	local id="$1"
	local dir="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"

	if [ ! -d "$SRC_ROOT" ]; then
		if ! mkdir -p "$SRC_ROOT" 2> /dev/null; then
			if ! $SUDO mkdir -p "$SRC_ROOT"; then
				warn "cannot create the source root $SRC_ROOT"
				return 1
			fi
		fi
	fi

	if [ ! -w "$SRC_ROOT" ]; then
		warn "the source root $SRC_ROOT is not writable by this user"
		return 1
	fi

	if [ ! -d "$dir/.git" ]; then
		note "cloning ${COMPONENT_NAME[$id]} from ${COMPONENT_REPO[$id]}"

		if ! git clone --quiet "${COMPONENT_REPO[$id]}" "$dir"; then
			warn "could not clone ${COMPONENT_REPO[$id]} into $dir"
			return 1
		fi
	else
		note "reusing the retained source at $dir"

		if ! git -C "$dir" fetch --quiet --tags origin; then
			warn "could not fetch ${COMPONENT_REPO[$id]} in $dir"
			return 1
		fi
	fi

	if ! git -C "$dir" checkout --quiet --detach "${COMPONENT_COMMIT[$id]}"; then
		warn "could not check out ${COMPONENT_COMMIT[$id]} (${COMPONENT_REF[$id]}) in $dir"
		return 1
	fi

	note "${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} is at ${COMPONENT_COMMIT[$id]}"

	return 0
}

build_and_install_component()
{
	local id="$1"
	local dir="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"
	local prefix
	local target="${COMPONENT_BUILD_TARGET[$id]}"
	local -a configure_args
	local -a extra_args=()

	prefix=$(prefix_for "$id")

	if [ -n "${COMPONENT_CONFIGURE[$id]}" ]; then
		read -ra extra_args <<< "${COMPONENT_CONFIGURE[$id]}"
	fi

	configure_args=(--prefix="$prefix" "${extra_args[@]}")

	note "configuring ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} for $prefix"

	if ! (cd "$dir" && ./configure "${configure_args[@]}"); then
		warn "configure failed for ${COMPONENT_NAME[$id]} in $dir"
		return 1
	fi

	note "building ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]}"

	if ! (cd "$dir" && if [ -n "$target" ]; then "$MAKE" "$target"; else "$MAKE"; fi); then
		warn "build failed for ${COMPONENT_NAME[$id]} in $dir"
		return 1
	fi

	note "installing ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} into $prefix"

	if ! (cd "$dir" && $SUDO "$MAKE" install); then
		warn "install failed for ${COMPONENT_NAME[$id]} into $prefix"
		return 1
	fi

	return 0
}

# Sets PROVISION_INSTALLED rather than printing its answer: every step below
# reports progress on stdout, so a caller capturing this function's output would
# capture the progress log with it.
PROVISION_INSTALLED='no'

provision_missing()
{
	local id

	PROVISION_INSTALLED='no'

	for id in "${COMPONENT_ORDER[@]}"; do
		if stack_satisfied "$id"; then
			note "skipping ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]}: the ${COMPONENT_STACK[$id]} stack is already present and usable"
			continue
		fi

		fetch_component "$id" || return 1
		build_and_install_component "$id" || return 1

		PROVISION_INSTALLED='yes'
	done

	if [ "$PROVISION_INSTALLED" = 'yes' ]; then
		# The loader cache has to be refreshed or the freshly installed libraries
		# are invisible to the very verification that follows.
		if command -v ldconfig > /dev/null 2>&1; then
			$SUDO ldconfig > /dev/null 2>&1 || warn 'ldconfig did not run, so a fresh install may not be visible to the loader yet'
		fi
	fi

	return 0
}

# Re-observe after installing and refuse to call the run a success if anything
# regressed.  An install that leaves the gate unable to clear the library it just
# put in place is worse than no install at all, because the next build would use
# it.
verify_after_install()
{
	observe_pkg_config_state
	cve_gate

	if ! enforce_cve_gate; then
		return "$EX_REFUSED"
	fi

	if [ "$H323_TOOLKIT_LINKABLE" != 'yes' ]; then
		warn "post-install verification failed: mod_h323 compile and link inputs are still not satisfied after installing into $PTLIB_PREFIX"
		warn "the probe mirrors the module and looks in /usr/include/openh323 and /usr/lib (ci.sh:69 and :91)"

		case "$PTLIB_PREFIX" in
			/usr) ;;
			*)
				warn "so a stack under $PTLIB_PREFIX has to be on the compiler and loader default paths: either re-run with --ptlib-prefix=/usr, or add $PTLIB_PREFIX/lib to /etc/ld.so.conf.d and $PTLIB_PREFIX/include to the include path"
				;;
		esac

		return "$EX_PROVISION"
	fi

	if [ "$OPAL_USABLE" != 'yes' ]; then
		warn "post-install verification failed: pkg-config still does not report an opal >= $OPAL_MIN_VERSION under $OPAL_PREFIX"
		return "$EX_PROVISION"
	fi

	if [ -n "$OPAL_VERSION" ] && [ "$OPAL_VERSION" != "${COMPONENT_VERSION[opal]}" ]; then
		warn "opal resolves to $OPAL_VERSION, which is not the pinned ${COMPONENT_VERSION[opal]}; the pin and the host have drifted"
	fi

	return 0
}

#------------------------------------------------------------------------------
# main
#------------------------------------------------------------------------------

main()
{
	if ! parse_args "$@"; then
		warn "try '$0 --help'"
		return "$EX_USAGE"
	fi

	if ! resolve_tree; then
		warn 'could not locate the FreeSWITCH tree from this script path'
		return "$EX_PROVISION"
	fi

	if [ "$MODE" = 'help' ]; then
		usage
		return "$EX_OK"
	fi

	compose_pkg_config_path
	select_tools

	say "$PROG: pinned endpoint toolkit provisioning for $FS_DIR"
	say ''
	say "  mode          : $MODE"
	say "  ptlib prefix  : $PTLIB_PREFIX   (PTLib ${COMPONENT_VERSION[ptlib_h323]} + H323Plus ${COMPONENT_VERSION[h323plus]})"
	say "  opal prefix   : $OPAL_PREFIX   (OPAL ${COMPONENT_VERSION[opal]} + PTLib ${COMPONENT_VERSION[ptlib_opal]})"
	say "  source root   : $SRC_ROOT"
	say "  CVE probe     : $CI_SCRIPT (h323_toolkit_available, sourced)"
	say ''

	observe_pkg_config_state
	cve_gate

	# The gate runs BEFORE anything is fetched, built or installed, and it applies
	# in every mode: a report that omitted the refusal would be a report of a plan
	# that cannot legally run.
	if ! enforce_cve_gate; then
		return "$EX_REFUSED"
	fi

	report_observed_state

	if [ "$MODE" = 'report' ]; then
		report_plan
		report_uninstall_inventory
		report_pkg_config_path
		print_summary

		return "$EX_OK"
	fi

	if [ "$H323_TOOLKIT_LINKABLE" = 'yes' ] && [ "$OPAL_USABLE" = 'yes' ]; then
		note 'both toolkits are already present and usable; nothing to install'
		say ''
		report_pkg_config_path
		print_summary

		return "$EX_OK"
	fi

	if ! require_build_tools; then
		return "$EX_PROVISION"
	fi

	provision_missing || return "$EX_PROVISION"

	verify_after_install || return $?

	if [ "$PROVISION_INSTALLED" = 'yes' ]; then
		note 'provisioning completed and verified'
	else
		note 'nothing needed installing; the existing toolkits verified'
	fi

	say ''
	report_pkg_config_path
	print_summary

	return "$EX_OK"
}

main "$@"
exit $?
