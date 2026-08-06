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
# resolve to no installable candidate either, even though the Build-Depends
# stanzas debian/control-modules carries for both endpoints declare all four.
# Every host that carries these toolkits therefore carries a HAND INSTALLED
# toolkit, which is not reproducible: a rebuilt CI image or a fresh production
# host comes up without them, and the capability guards in ci.sh then quietly
# leave both modules disabled.  This script closes that gap.  It installs the
# exact versions this project was validated against, from pinned immutable refs,
# it is safe to re-run, and it refuses to install onto a PTLib that cannot be
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
# A commit is not the whole pin either.  These sources are from 2010 to 2013 and
# they do not compile on a current toolchain as they stand: both PTLib branches
# include <termio.h>, which glibc 2.42 removed, and all four rely on C++ that the
# compiler's default standard no longer accepts.  So the pin also covers the
# compatibility patches under build/patches/endpoint_toolkits - each verified
# against a SHA-256 recorded in this file before it is applied - and the C++
# standard each component is built with.  A build input that is not pinned is a
# build that is not reproducible, and unrecorded local fixes are precisely how the
# toolkits this script replaces became impossible to rebuild.
#
# WHY NOT build/buildopal.sh
#
# build/buildopal.sh is the MODEL for this script's flow - locate the tree from
# $0, export PKG_CONFIG_PATH, ./configure --disable-plugins --prefix=..., make,
# sudo make install - and it is left byte for byte as it is.  It cannot serve as
# reproducible provisioning itself, for three reasons.  Both of its `svn co'
# invocations check out https://svn.code.sf.net/p/opalvoip/code/{ptlib,opal},
# and that SourceForge Subversion service no longer answers - the `svn' client
# the script demands is beside the point.  Its VERSION and PATCH pins are
# commented out, so it resolves to `trunk' and installs whatever upstream
# happens to be that day.  And it knows nothing of H323Plus, which mod_h323
# needs in addition to PTLib (-lopenh323, and /usr/include/openh323/h323.h).
# The sources that actually produced this project's toolkits were the
# SourceForge GIT mirrors plus two GitHub mirrors, which is what the table below
# pins.
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
# mod_opal.h's OPAL_CHECK_VERSION guard and the #error beneath it - which makes
# this script exit 0 on that host, and on any other layout that genuinely works.
#
# Two DIFFERENT questions are asked about a prefix, and keeping them apart is what
# makes the defaults work.  "Is this host provisioned" is asked against the search
# paths mod_h323 itself hardcodes - /usr/include/openh323 and /usr/lib, per
# src/mod/endpoints/mod_h323/Makefile.am:6 and :10 - because those are the only
# paths that decide whether CI can build the module.  "Did the install into the
# chosen prefix work" is asked against THAT prefix, through the same shared guard
# pointed at it, so a correct install into /usr/local verifies as a correct install
# even before it is reachable by default.  When the two answers differ, the host
# integration step bridges the gap and reports every path it creates.
#
# THE CVE-2013-1864 GATE
#
# PTLib's PXML parser expanded internal entities with no ceiling before 2.10.10,
# so a "billion laughs" document makes any consumer allocate until it is killed.
# ci.sh owns the ONE implementation of that verdict (h323_guard_evaluate, which
# publishes H323_GUARD_VERDICT), and a second copy of a security decision is a
# second thing to get wrong: this script SOURCES that implementation and ADOPTS its
# verdict without reinterpreting it.  Refusal is the default for everything except
# the two affirmatively safe verdicts - a library that bounds entity expansion, and
# one built without expat, which carries no PXML parser and therefore no entity
# expander for the advisory to be about.  That second verdict is what --disable-expat
# in the pinned PTLib flags produces, and it is decided by ci.sh, not here, so a
# stack this script calls provisioned is a stack CI will enable.
#
# MODES
#
#   (default)                    provision what is missing, then verify
#   --uninstall-check, --dry-run report what WOULD be fetched, built, installed
#                                and where, plus what an uninstall would have to
#                                remove; change nothing
#   --self-test                  prove the CVE-2013-1864 refusal and clearance
#                                branches against scratch stub PTLib SDKs, with
#                                every host-changing step tripwired; installs
#                                nothing
#   --help                       usage
#
# EXIT STATUS
#
#   0  provisioned, already provisioned, report produced, or self-test passed
#   2  usage error
#   3  REFUSED - the CVE-2013-1864 gate did not clear the PTLib that would be
#      linked, or a post-install verification regressed
#   4  provisioning failed (fetch, configure, build or install)
#   5  self-test failed
#
# This script never prompts.  It is safe to run non-interactively, and it removes
# every scratch directory it creates through a trap.  Apart from those temporary
# probe directories and the loader-cache refresh that follows an installation,
# every persistent write it makes is confined to the selected prefixes and the
# retained-source root it reports.
#------------------------------------------------------------------------------

# No `set -e'.  The probe helpers below deliberately run commands that are
# EXPECTED to fail (that is what a probe is), and an errexit shell turns the
# expected failure of a guard into an unexplained exit before the guard can
# report its verdict.  Every failure that affects a probe, a fetch, a build, an
# install or a verification is checked explicitly instead.  `set -u' stays on: an
# unset variable in a path that is about to be handed to rm or install is the one
# class of bug this script must not have.
set -u

readonly PROG='provision_endpoint_toolkits'

readonly EX_OK=0
readonly EX_USAGE=2
readonly EX_REFUSED=3
readonly EX_PROVISION=4
readonly EX_SELFTEST=5

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
# and ci.sh's unit-test arm gates enablement on the same number.  Kept identical on
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
# The C++ standard each component is compiled with, and it is as much a pin as the
# commit is.
#
# These sources are from 2010 to 2013.  GCC's default standard has moved twice since,
# and the parts of C++ they rely on were removed on the way: PTLib declares
# `operator new(size_t) throw (std::bad_alloc)', which C++17 rejects outright.  Left to
# the compiler's default, the pinned commits do not build at all on a current host - and
# a build input that is "whatever the compiler defaults to this year" is exactly the kind
# of unrecorded input this script exists to remove.  It is expressed through CXX rather
# than CXXFLAGS because PTLib's own make rules use neither CXXFLAGS nor CPPFLAGS on the
# compile line; $(CXX) is the only variable they all pass through.
#
# Empty means "whatever the compiler defaults to", which is correct only for a component
# that has been kept current.
declare -A COMPONENT_CXX_STANDARD=(
	[ptlib_h323]='-std=gnu++98'
	[h323plus]='-std=gnu++98'
	[ptlib_opal]='-std=gnu++98'
	[opal]='-std=gnu++98'
)

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
# Scratch directories
#
# Every one this script creates is registered here and removed by the trap,
# including on the refusal paths - a probe that leaves a compiled artifact behind
# in /tmp on every CI run is a slow leak, and a probe that leaves one behind on
# the FAILURE path is the one nobody notices.
#
# The PARENT they are created under is resolved and validated once, and every
# registered path is checked against that one parent before it is removed.  Two
# hard-coded prefixes would be a guess about where mktemp puts things: a TMPDIR
# that points somewhere else is ordinary on hosts with a private per-service
# temporary directory, and the guess then leaks exactly the directories it was
# written to remove.
#------------------------------------------------------------------------------

declare -a SCRATCH_DIRS=()

SCRATCH_PARENT=''

# Sets SCRATCH_LAST rather than printing the path, because a command
# substitution would register the directory in a subshell and the trap in THIS
# shell would then never remove it.
SCRATCH_LAST=''

# Whether a cleanup ever failed to remove something it owned.  Consulted on the
# normal path so that a leak is reported in the exit status and not only in a
# warning that scrolls past.
SCRATCH_CLEANUP_FAILED='no'

scratch_parent()
{
	local parent

	if [ -n "$SCRATCH_PARENT" ]; then
		printf '%s\n' "$SCRATCH_PARENT"
		return 0
	fi

	if ! parent=$(cd -- "${TMPDIR:-/tmp}" > /dev/null 2>&1 && pwd); then
		warn "${TMPDIR:-/tmp} is not a usable directory, so no scratch directory can be created"
		return 1
	fi

	# cd+pwd has already made this absolute; what is checked is the one path that
	# must never become the parent of an rm -rf target
	if [ "$parent" = '/' ]; then
		warn "refusing to create scratch directories directly under '/'"
		return 1
	fi

	if [ ! -w "$parent" ]; then
		warn "$parent is not writable, so no scratch directory can be created"
		return 1
	fi

	SCRATCH_PARENT="$parent"

	printf '%s\n' "$parent"

	return 0
}

new_scratch_dir()
{
	local parent
	local dir

	parent=$(scratch_parent) || return 1

	dir=$(mktemp -d -p "$parent" 2> /dev/null) || return 1

	SCRATCH_DIRS+=("$dir")
	SCRATCH_LAST="$dir"

	return 0
}

# Remove one scratch directory early and forget it, for a caller that is finished
# with it long before the trap runs.
discard_scratch_dir()
{
	local target="$1"
	local -a kept=()
	local dir

	for dir in "${SCRATCH_DIRS[@]}"; do
		if [ "$dir" != "$target" ]; then
			kept+=("$dir")
		fi
	done

	SCRATCH_DIRS=("${kept[@]}")

	case "$target" in
		"$SCRATCH_PARENT"/?*)
			if ! rm -rf -- "$target"; then
				SCRATCH_CLEANUP_FAILED='yes'
				warn "could not remove the scratch directory '$target'"
				return 1
			fi
			;;
		*)
			SCRATCH_CLEANUP_FAILED='yes'
			warn "refusing to remove unexpected scratch path '$target'"
			return 1
			;;
	esac

	return 0
}

# shellcheck disable=SC2317  # reached only through the traps installed below
remove_scratch_dirs()
{
	local dir

	# bash 4.4 and later expand an empty array under `set -u' without error, so
	# this loop needs no guard against SCRATCH_DIRS being empty.
	for dir in "${SCRATCH_DIRS[@]}"; do
		# Guarded rather than trusted: an element that is not one of ours would
		# make this an `rm -rf' of something else entirely.  The parent is the
		# one this script created the directory under, not a guess about it.
		case "$dir" in
			"$SCRATCH_PARENT"/?*)
				if ! rm -rf -- "$dir"; then
					SCRATCH_CLEANUP_FAILED='yes'
					warn "could not remove the scratch directory '$dir'"
				fi
				;;
			*)
				SCRATCH_CLEANUP_FAILED='yes'
				warn "refusing to remove unexpected scratch path '$dir'"
				;;
		esac
	done

	SCRATCH_DIRS=()
}

# HUP is trapped alongside INT and TERM, and not only for symmetry with ci.sh: bash runs
# the EXIT trap for a normal exit and for a signal it has a trap for, and for nothing
# else.  An untrapped SIGHUP - a disconnecting terminal, which is how a long provisioning
# run usually dies - would therefore kill this script with the scratch tree still on disk.
trap 'remove_scratch_dirs' EXIT
trap 'remove_scratch_dirs; exit 129' HUP
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
		  --self-test         Prove the CVE-2013-1864 refusal and clearance branches
		                      against scratch stub PTLib SDKs, with fetch, build,
		                      install and host integration replaced by tripwires.
		                      Installs nothing and changes no toolkit or toolchain
		                      state; verifies as much before and after.
		  --help, -h          This text.

		Options:
		  --ptlib-prefix DIR  Install prefix for PTLib ${COMPONENT_VERSION[ptlib_h323]} + H323Plus ${COMPONENT_VERSION[h323plus]}
		                      (default: /usr/local, env PTLIB_PREFIX)
		  --opal-prefix DIR   Install prefix for OPAL ${COMPONENT_VERSION[opal]} and its bundled
		                      PTLib ${COMPONENT_VERSION[ptlib_opal]}
		                      (default: /opt/opalvoip, env OPAL_PREFIX)
		  --src-root DIR      Where pinned sources are checked out and retained
		                      (default: /opt/src, env PROVISION_SRC_ROOT)

		Privileges:
		  Installing needs root, or passwordless sudo - sudo is only ever invoked
		  as 'sudo -n', because a password prompt would hang an image build rather
		  than fail it.  Only the install, ldconfig and host-integration steps run
		  privileged; every git, patch and build step is deliberately unprivileged.
		  The source root and the git mirrors retained inside it must therefore be
		  writable by the invoking user.  A source root this script has to create
		  is created WITH that user's ownership, so provisioning never leaves a
		  root-owned source root behind; one that already exists and belongs to
		  somebody else is reported as such rather than worked around with
		  privileged git.

		Pinned components:
		$(component_pin_lines '  ')

		Exit status:
		  0  provisioned, already provisioned, report produced, or self-test passed
		  2  usage error
		  3  REFUSED - the CVE-2013-1864 gate did not clear the PTLib that would be
		     linked, or a post-install verification regressed
		  4  provisioning failed (fetch, configure, build or install)
		  5  self-test failed
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
# Directory options
#
# Three values name places on the filesystem, two of them install prefixes, and all
# three are used to build paths that are created, symlinked at and removed.  They are
# therefore validated and normalised in ONE place before anything reads them, whether
# they arrived on the command line or through the environment.
#
# Absolute only.  A relative prefix would make the install location depend on the
# caller's working directory, so the same command would install somewhere different for
# every caller - and the report would name a path that only means anything to whoever
# happened to run it.
#
# Trailing and duplicate slashes are removed because every comparison in this file is a
# STRING comparison: h323_bridge_table() skips the host bridge for exactly "/usr", and
# "/usr/" would otherwise slip past it and symlink /usr/include/openh323 at itself.
# Symlinks are deliberately NOT resolved - the target may not exist yet, and rewriting
# the prefix an operator asked for is not this script's business.
#------------------------------------------------------------------------------

normalise_directory_option()
{
	local option="$1"
	local value="$2"

	case "$value" in
		'')
			warn "option '$option' requires a directory"
			return 1
			;;
		-*)
			warn "option '$option' was given '$value', which looks like another option rather than a directory"
			return 1
			;;
		/*) ;;
		*)
			warn "option '$option' requires an absolute directory, and '$value' is relative"
			return 1
			;;
	esac

	while :; do
		case "$value" in
			*//*) value="${value//\/\//\/}" ;;
			*) break ;;
		esac
	done

	while :; do
		case "$value" in
			/) break ;;
			*/) value="${value%/}" ;;
			*) break ;;
		esac
	done

	if [ "$value" = '/' ]; then
		warn "option '$option' was given '/', which cannot be an install prefix or a source root"
		return 1
	fi

	# A '.' or '..' SEGMENT is refused rather than rewritten.  Every prefix decision in
	# this file is a string comparison, so "/usr/." would name the same directory as
	# "/usr" while comparing unequal to it - and would therefore slip past the
	# h323_bridge_table() test that exists to stop /usr being symlinked at itself.
	# '..' cannot be collapsed textually without guessing about symlinks, so the
	# caller is asked for the plain path instead.  A leading dot in a NAME, as in
	# /opt/.cache, is not a segment and is left alone.
	case "$value/" in
		*/./* | */../*)
			warn "option '$option' was given '$value', which contains a '.' or '..' path segment; pass the directory by its plain path"
			return 1
			;;
	esac

	printf '%s\n' "$value"

	return 0
}

# Normalise all three, wherever they came from.  Called once, after parsing.
normalise_directory_options()
{
	PTLIB_PREFIX=$(normalise_directory_option '--ptlib-prefix' "$PTLIB_PREFIX") || return 1
	OPAL_PREFIX=$(normalise_directory_option '--opal-prefix' "$OPAL_PREFIX") || return 1
	SRC_ROOT=$(normalise_directory_option '--src-root' "$SRC_ROOT") || return 1

	return 0
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
			--self-test)
				MODE='self-test'
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
# The H.323 verdict, obtained from ci.sh by SOURCING its implementation
#
# ci.sh owns ONE implementation of two questions - can mod_h323's own compile and
# link inputs be satisfied, and can the PTLib that would be loaded be cleared of
# CVE-2013-1864 - and publishes the answer as H323_GUARD_VERDICT plus three
# companion facts.  This script consumes that answer and never re-derives it.
#
# That is not tidiness, it is the correctness property this whole gate rests on.
# A consumer that read ci.sh's diagnostics and applied its own taxonomy would be a
# second copy of a security decision, and two copies come to disagree: the failure
# this arrangement removes was exactly that, a toolkit ci.sh refused while this
# script called it provisioned, so an operator saw a successful provisioning run of
# a stack CI would then silently exclude.  There is now one verdict, and if it is
# wrong it is wrong in both places at once - which is a bug that can be found.
#
# ci.sh is a CI driver, not a library, and sourcing it is hostile in three specific
# ways.  Each is neutralised deliberately, inside a subshell, so none of it can leak
# into the provisioning pass:
#
#   1. ci.sh parses "$@" when it is EXECUTED.  Sourcing with an explicit `--' ends
#      its option parsing immediately, so this script's own arguments - --dry-run,
#      --ptlib-prefix and the rest - can never reach it and can never trip its `?)'
#      arm, which calls display_usage and exits.
#
#   2. ci.sh's job dispatcher exits on EVERY arm: an unset $CODE falls through to
#      `*) exit 1', and a recognised one runs a CI action.  CODE, ACTION, TYPE and
#      PATH_TO_CODE are unset before sourcing - not for tidiness, but because a
#      caller whose environment happens to carry CODE=freeswitch ACTION=configure
#      would otherwise have this script run ./bootstrap.sh and ./configure over the
#      tree as a side effect of asking whether PTLib is safe.
#
#   3. That dispatcher exit would still end the shell that sourced the file.  A
#      shell FUNCTION named `exit' is therefore defined for the duration of the
#      source: bash resolves functions ahead of the exit builtin, so the
#      dispatcher's exit becomes a no-op, the source completes, and every ci.sh
#      function is defined.  `unset -f exit' restores the builtin before the guard
#      is called, so nothing else runs with exit stubbed out.  This was chosen over
#      an EXIT trap because it leaves the call in ordinary control flow, where its
#      status and its output are trivially captured.
#
# The mechanism does not care whether ci.sh has grown a source guard: with one, the
# file returns before the dispatcher and the `exit' function is simply never called;
# without one, it is what keeps the source alive.  Both were exercised.
#
# The subshell emits a sentinel carrying the guard's status followed by ci.sh's own
# key=value verdict lines.  A missing sentinel or a missing verdict means the guard
# never completed - ci.sh absent, unreadable, renamed, the contract gone, or an exit
# this script failed to neutralise - and that is an UNVERIFIABLE PTLib, never a pass.
#------------------------------------------------------------------------------

readonly PROBE_SENTINEL='__PROVISION_CVE_PROBE_STATUS__'

# Which SDK the shared guard interrogates.  Empty means the search paths ci.sh uses
# for CI, which are mod_h323's own hardcoded include and library directories - the
# only paths that answer "will CI be able to build this module".  A prefix is passed
# when the question is narrower: did the install into THAT prefix produce a working
# stack, or - in the self-test - does the guard reach the intended verdict about a
# scratch stub SDK.
H323_PROBE_PREFIX=''

H323_GUARD_VERDICT=''
H323_GUARD_DETAIL=''
H323_GUARD_LINKABLE=''
H323_GUARD_LIBPT=''
H323_GUARD_STATUS=''
H323_GUARD_DIAGNOSTICS=''

# Read one published key out of the captured block.
#
# Anchored on the key and taking the LAST occurrence, so a diagnostic line that
# happened to contain the same text cannot be mistaken for the verdict.
guard_captured_value()
{
	printf '%s\n' "$1" | sed -n "s/^H323_GUARD_$2=//p" | tail -n 1
}

run_ci_h323_guard()
{
	local guard_prefix="$1"
	local captured
	local wrapper_status
	local sentinel
	local scratch

	H323_GUARD_VERDICT=''
	H323_GUARD_DETAIL=''
	H323_GUARD_LINKABLE=''
	H323_GUARD_LIBPT=''
	H323_GUARD_STATUS=''
	H323_GUARD_DIAGNOSTICS=''

	if [ ! -r "$CI_SCRIPT" ]; then
		warn "cannot read $CI_SCRIPT, so the shared H.323 verdict cannot be obtained"
		return 1
	fi

	# The guard's probes compile somewhere, and that somewhere is owned HERE - by this
	# script's traps - rather than left to a directory created inside a command
	# substitution, which no trap of ours would ever see.  ci.sh honours
	# H323_PROBE_SCRATCH_PARENT for exactly this reason.
	if ! new_scratch_dir; then
		warn 'no scratch directory could be created, so the shared H.323 verdict cannot be obtained'
		return 1
	fi

	scratch="$SCRATCH_LAST"

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

		if ! declare -F h323_guard_evaluate > /dev/null 2>&1 ||
			! declare -F h323_guard_verdict_lines > /dev/null 2>&1; then
			printf '%s=%s\n' "$PROBE_SENTINEL" 127
			exit 0
		fi

		if [ -n "$guard_prefix" ] && ! h323_probe_search_prefix "$guard_prefix"; then
			printf '%s=%s\n' "$PROBE_SENTINEL" 126
			exit 0
		fi

		# shellcheck disable=SC2034  # read by the sourced ci.sh, not by this file
		H323_PROBE_SCRATCH_PARENT="$scratch"

		h323_guard_evaluate 2>&1
		printf '%s=%s\n' "$PROBE_SENTINEL" "$?"
		h323_guard_verdict_lines
	)
	# Only meaningful when the sentinel is missing: it then says how the wrapper died
	# rather than what the guard decided.
	wrapper_status=$?

	sentinel=$(printf '%s\n' "$captured" |
		sed -n "s/^${PROBE_SENTINEL}=\\([0-9][0-9]*\\)\$/\\1/p" | tail -n 1)

	H323_GUARD_VERDICT=$(guard_captured_value "$captured" 'VERDICT')
	H323_GUARD_LINKABLE=$(guard_captured_value "$captured" 'LINKABLE')
	H323_GUARD_LIBPT=$(guard_captured_value "$captured" 'LIBPT')
	H323_GUARD_DETAIL=$(guard_captured_value "$captured" 'DETAIL')

	# Whatever ci.sh said in prose, kept for the report only.  Nothing decides on it.
	H323_GUARD_DIAGNOSTICS=$(printf '%s\n' "$captured" |
		grep -v -E "^(${PROBE_SENTINEL}=|H323_GUARD_[A-Z]+=)")

	discard_scratch_dir "$scratch" || return 1

	if [ -z "$sentinel" ]; then
		warn "the shared H.323 verdict in $CI_SCRIPT did not complete (wrapper exited $wrapper_status)"
		return 1
	fi

	if [ "$sentinel" = '127' ]; then
		warn "$CI_SCRIPT no longer publishes h323_guard_evaluate and h323_guard_verdict_lines, so there is no shared verdict to consume"
		return 1
	fi

	if [ "$sentinel" = '126' ]; then
		warn "$CI_SCRIPT refused '$guard_prefix' as an H.323 SDK prefix"
		return 1
	fi

	if [ -z "$H323_GUARD_VERDICT" ] || [ -z "$H323_GUARD_LINKABLE" ]; then
		warn "the shared H.323 verdict in $CI_SCRIPT completed without publishing a verdict"
		return 1
	fi

	H323_GUARD_STATUS="$sentinel"

	return 0
}

#------------------------------------------------------------------------------
# The gate itself.  CVE_VERDICT is ci.sh's verdict, adopted rather than recomputed:
#
#   clear            the guard ran the entity document and the library bounded it
#   clear_no_parser  the library provably has no XML parser to be vulnerable with -
#                    which is what --disable-expat in the pinned PTLib configure
#                    flags below produces, and why that flag is defence in depth
#                    rather than a way around the gate
#   vulnerable       the library ignored the ceiling it was given, or exposes the
#                    parser with no ceiling API at all
#   unverifiable     the verdict could not be obtained, could not be believed, or
#                    named something this script does not know - fail closed
#   absent           there is no H.323 PTLib here to judge, which is what this
#                    script exists to fix rather than something to refuse
#
# Every disagreement between the published facts is resolved as unverifiable.  A
# verdict that does not match the status it came with, or an `absent' verdict from a
# toolkit that reportedly links, means this script and ci.sh no longer understand
# each other, and a security gate that has stopped understanding its own input has
# to refuse.
#------------------------------------------------------------------------------

CVE_VERDICT=''
CVE_DETAIL=''
H323_TOOLKIT_LINKABLE='no'
H323_RESOLVED_LIBPT=''

# What the refusal should say has happened by the time it fires.  The gate runs before
# anything is touched and again after the install, and "nothing was fetched, built or
# installed" is a lie in the second position - an operator who reads it would not go
# looking for a toolkit on disk that must not be used.
CVE_GATE_STAGE='nothing was fetched, built or installed'

cve_gate()
{
	CVE_VERDICT=''
	CVE_DETAIL=''
	H323_TOOLKIT_LINKABLE='no'
	H323_RESOLVED_LIBPT=''

	if ! run_ci_h323_guard "$H323_PROBE_PREFIX"; then
		CVE_VERDICT='unverifiable'
		CVE_DETAIL="the shared verdict could not be obtained from $CI_SCRIPT"
		return 0
	fi

	H323_RESOLVED_LIBPT="$H323_GUARD_LIBPT"

	# The capability answer comes from the guard's own flag, never from whether it
	# happened to print something: a diagnostic is not evidence that mod_h323 links.
	case "$H323_GUARD_LINKABLE" in
		yes) H323_TOOLKIT_LINKABLE='yes' ;;
		no) H323_TOOLKIT_LINKABLE='no' ;;
		*)
			CVE_VERDICT='unverifiable'
			CVE_DETAIL="the shared verdict reported the unusable capability flag '$H323_GUARD_LINKABLE'"
			return 0
			;;
	esac

	case "$H323_GUARD_VERDICT" in
		clear | clear_no_parser | vulnerable | unverifiable | absent)
			CVE_VERDICT="$H323_GUARD_VERDICT"
			CVE_DETAIL="$H323_GUARD_DETAIL"
			;;
		*)
			CVE_VERDICT='unverifiable'
			CVE_DETAIL="$CI_SCRIPT published the unrecognised verdict '$H323_GUARD_VERDICT'"
			return 0
			;;
	esac

	case "$CVE_VERDICT" in
		clear | clear_no_parser)
			if [ "$H323_GUARD_STATUS" != '0' ]; then
				CVE_VERDICT='unverifiable'
				CVE_DETAIL="the shared verdict said '$H323_GUARD_VERDICT' but refused the toolkit (status $H323_GUARD_STATUS), so the two disagree"
				return 0
			fi

			if [ "$H323_TOOLKIT_LINKABLE" != 'yes' ] || [ -z "$H323_RESOLVED_LIBPT" ]; then
				CVE_VERDICT='unverifiable'
				CVE_DETAIL="the shared verdict cleared a toolkit it could not establish a linkage for"
				return 0
			fi
			;;
		absent)
			# `absent' is the compile-and-link miss and nothing else.  A toolkit that
			# links is a toolkit there is something to judge about.
			if [ "$H323_TOOLKIT_LINKABLE" = 'yes' ]; then
				CVE_VERDICT='unverifiable'
				CVE_DETAIL='the shared verdict called the toolkit absent while reporting that mod_h323 links against it'
				return 0
			fi
			;;
		*)
			if [ "$H323_GUARD_STATUS" = '0' ]; then
				CVE_VERDICT='unverifiable'
				CVE_DETAIL="the shared verdict said '$H323_GUARD_VERDICT' but cleared the toolkit anyway, so the two disagree"
				return 0
			fi
			;;
	esac

	return 0
}

# Enforce the gate.  Returns non-zero when provisioning must not proceed, and says so
# with the named refusal.  Called BEFORE any fetch, build or install, and again after.
#
# The two safe verdicts and `absent' are the only ones that continue.  Everything
# else - including a verdict this script has never heard of - refuses, because the
# alternative is deciding a security question by falling off the end of a case.
enforce_cve_gate()
{
	case "$CVE_VERDICT" in
		clear | clear_no_parser | absent)
			return 0
			;;
		vulnerable)
			refuse 'the PTLib that would be linked does not bound XML entity expansion' \
				"$CVE_DETAIL" \
				"$CVE_GATE_STAGE" \
				"remedy: install PTLib 2.10.10 or newer, a build carrying the backported fix, or one built --disable-expat (which this script pins for ${COMPONENT_NAME[ptlib_h323]} ${COMPONENT_VERSION[ptlib_h323]}), then re-run"
			return 1
			;;
		unverifiable)
			refuse 'the PTLib that would be linked cannot be cleared of the advisory' \
				"$CVE_DETAIL" \
				'an indeterminate verdict is treated as a refusal, never as a pass' \
				"$CVE_GATE_STAGE"
			return 1
			;;
	esac

	refuse 'the CVE-2013-1864 gate reached no verdict at all' \
		"the verdict was '${CVE_VERDICT:-empty}', which this script does not recognise" \
		'a gate with no verdict refuses' \
		"$CVE_GATE_STAGE"

	return 1
}

#------------------------------------------------------------------------------
# The OPAL side of capability detection.
#
# There is no compile probe here because there does not need to be one: OPAL is
# discoverable through pkg-config on every layout this project has seen, and the
# module's own header settles what "usable" means - mod_opal.h:41-42 stops the
# build below 3.12.8.  ci.sh's unit-test arm gates enablement on that number, and
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

	# The shared guard's own words, quoted rather than paraphrased: this is the same
	# text a CI log carries for the same toolkit, so an operator comparing the two is
	# comparing identical strings.
	if [ -n "$H323_GUARD_DIAGNOSTICS" ]; then
		say '    what the shared ci.sh guard reported   :'
		printf '%s\n' "$H323_GUARD_DIAGNOSTICS" | sed 's/^/      /'
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
	local assignment
	local index
	local name

	h323_bridge_table

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

		say "    mirror    : git clone ${COMPONENT_REPO[$id]} $srcdir"
		say "    tree      : git -C $srcdir worktree add --detach $(component_tree_dir "$id") ${COMPONENT_COMMIT[$id]}   (${COMPONENT_REF[$id]})"
		say "                created fresh every run, asserted clean, then patched"

		if [ -z "${COMPONENT_PATCHES[$id]}" ]; then
			say "    patch     : none required at this pin"
		else
			for name in ${COMPONENT_PATCHES[$id]}; do
				say "    patch     : $PATCH_DIR_RELATIVE/$name"
				say "                sha256 ${PATCH_SHA256[$name]}"
			done
		fi

		say "    configure : ./configure --prefix=$prefix${COMPONENT_CONFIGURE[$id]:+ ${COMPONENT_CONFIGURE[$id]}}"
		component_build_env "$id"
		component_make_vars "$id"

		say "    build     : $MAKE${COMPONENT_MAKE_VARS[*]:+ ${COMPONENT_MAKE_VARS[*]}}${target:+ $target}"
		say "    install   : ${SUDO:+$SUDO }$MAKE${COMPONENT_MAKE_VARS[*]:+ ${COMPONENT_MAKE_VARS[*]}} install   (into $prefix)"

		for assignment in "${COMPONENT_ENV[@]}"; do
			say "    env       : $assignment"
		done

		say ''
	done

	if [ "${#H323_BRIDGE_TARGET[@]}" -gt 0 ]; then
		say "  Host integration for --ptlib-prefix=$PTLIB_PREFIX"
		say '    mod_h323 hardcodes -I/usr/include/openh323 and -L/usr/lib, so a stack'
		say '    installed elsewhere is bridged onto those paths after installing.  Only'
		say '    paths that do not already exist are created:'
		say ''
		say "    ld.so.conf: $H323_LD_CONF carrying $PTLIB_PREFIX/lib"

		for index in "${!H323_BRIDGE_TARGET[@]}"; do
			say "    symlink   : ${H323_BRIDGE_TARGET[$index]} -> ${H323_BRIDGE_SOURCE[$index]}"
		done

		say ''
	fi

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
	local treedir
	local -a patterns=()
	local -a matches=()
	local -a integration=()

	say '-- uninstall inventory (nothing is removed in this mode) --'
	say ''

	for id in "${COMPONENT_ORDER[@]}"; do
		prefix=$(prefix_for "$id")

		say "  ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} under $prefix"

		read -ra patterns <<< "${COMPONENT_ARTIFACTS[$id]}"

		for pattern in "${patterns[@]}"; do
			found='no'
			matches=()

			# The PATTERN is a glob; the PREFIX is not.  compgen -G expands the one
			# without word-splitting the other, so a prefix carrying a space is still a
			# single path - which an unquoted `for path in $prefix/$pattern' would have
			# split into pieces that exist nowhere.
			while IFS= read -r path; do
				matches+=("$path")
			done < <(compgen -G "$prefix/$pattern" 2> /dev/null)

			for path in "${matches[@]}"; do
				if [ -e "$path" ] || [ -L "$path" ]; then
					# Symlink targets are resolved in the report because the two
					# stacks cross here: /usr/local/lib/pkgconfig/ptlib.pc is a
					# symlink into the OPAL prefix on the pinned layout, and an
					# uninstall that treated it as a PTLib 2.10.9 file of its own
					# would break the OPAL stack instead.
					if [ -L "$path" ]; then
						say "    present : $path -> $(readlink -- "$path")"
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
			say "    retained mirror : $srcdir"
		else
			say "    retained mirror : $srcdir (not present)"
		fi

		treedir=$(component_tree_dir "$id")

		if [ -d "$treedir" ]; then
			say "    build tree      : $treedir"
		else
			say "    build tree      : $treedir (not present; recreated from the mirror on demand)"
		fi

		say ''
	done

	say '  Host integration an uninstall would also have to undo:'

	# Everything this script can create outside the two prefixes, in one list: the OPAL
	# .pc symlinks and loader entry the pinned layout uses, and every H.323 bridge the
	# chosen prefix would need.  A path reported absent is one an uninstall can ignore.
	h323_bridge_table

	integration=(/usr/local/lib/pkgconfig/opal.pc /usr/local/lib/pkgconfig/ptlib.pc /etc/ld.so.conf.d/opalvoip.conf)

	if [ "${#H323_BRIDGE_TARGET[@]}" -gt 0 ]; then
		integration+=("$H323_LD_CONF")
		integration+=("${H323_BRIDGE_TARGET[@]}")
	fi

	for path in "${integration[@]}"; do
		if [ -L "$path" ]; then
			say "    present : $path -> $(readlink -- "$path")"
		elif [ -e "$path" ]; then
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
	say '  Toolchain this run would build with:'
	printf '  %-26s %s\n' 'C++ compiler' "$(${CXX:-g++} --version 2> /dev/null | head -1)"
	printf '  %-26s %s\n' 'autoconf' "$(autoconf --version 2> /dev/null | head -1)"
	printf '  %-26s %s\n' 'aclocal' "$(aclocal --version 2> /dev/null | head -1)"
	say '  The pinned Makefiles regenerate configure from the pinned configure.ac with'
	say '  that autoconf, so it is a build input and is reported as one.'
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
		# run that will fail at the install step should say so before it spends an
		# expensive compile getting there.
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

	# autoconf and aclocal are not optional here even though nothing in this script
	# calls them: PTLib's and OPAL's own top-level Makefile REGENERATES configure from
	# the pinned configure.ac when configure is older than its inputs, which it always
	# is in a fresh worktree because aclocal.m4 does not exist yet.  Without them that
	# rule prints "the configure script requires updating but autoconf not is installed"
	# and the build proceeds against a configure the pinned configure.ac no longer
	# describes - a silent, unreproducible difference, which is worse than a refusal.
	for tool in "$MAKE" git pkg-config autoconf aclocal; do
		if ! command -v "$tool" > /dev/null 2>&1; then
			warn "$tool is required to provision the toolkits and is not installed"
			return 1
		fi
	done

	return 0
}

# The source root has to exist and be writable BY THIS USER before anything is fetched.
#
# Creating it with sudo and then testing -w is the trap this replaces: on a host with
# passwordless sudo the directory appears, owned by root, and the very next check fails
# for the user who asked for it - after the script has already changed the filesystem.
# The ownership is therefore decided AS it is created, and a run that cannot create it
# says which of the three contracts it needs rather than failing twenty minutes later.
require_source_root()
{
	local owner_uid
	local owner_gid

	if [ -d "$SRC_ROOT" ]; then
		if [ ! -w "$SRC_ROOT" ]; then
			warn "the source root $SRC_ROOT exists but is not writable by uid $(id -u)"
			warn "re-run as its owner, re-run with --src-root pointing somewhere this user can write, or chown it"
			return 1
		fi

		return 0
	fi

	if mkdir -p -- "$SRC_ROOT" 2> /dev/null; then
		return 0
	fi

	if [ -z "$SUDO" ]; then
		warn "cannot create the source root $SRC_ROOT"
		return 1
	fi

	owner_uid=$(id -u)
	owner_gid=$(id -g)

	# install -d assigns the ownership as it creates, so the directory is never
	# root-owned even for an instant
	if ! $SUDO install -d -o "$owner_uid" -g "$owner_gid" -- "$SRC_ROOT"; then
		warn "cannot create the source root $SRC_ROOT, with or without sudo"
		warn 'provisioning needs one of: root, passwordless sudo, or a --src-root this user can already write'
		return 1
	fi

	note "created the source root $SRC_ROOT owned by uid $owner_uid"

	if [ ! -w "$SRC_ROOT" ]; then
		warn "the source root $SRC_ROOT was created but is still not writable by uid $owner_uid"
		return 1
	fi

	return 0
}

# Fetch a component at its pinned commit, into a MIRROR that is never built in.
#
# A detached checkout of a COMMIT, never a branch name: build/buildopal.sh installs
# whatever `trunk' is on the day it runs (build/buildopal.sh:29-34), which is the
# specific non-reproducibility this script exists to remove.  The mirror is retained
# between runs so a rebuild does not refetch, and it is the only thing a re-run
# updates - the tree that gets configured and built is created fresh from it by
# prepare_component_tree() below, because a retained tree is a tree that accumulates.
fetch_component()
{
	local id="$1"
	local dir="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"

	require_source_root || return 1

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

	# The pin has to EXIST in what was fetched.  A mirror that predates the pin, or a
	# rewritten upstream branch, otherwise fails later and much less clearly.
	if ! git -C "$dir" rev-parse --verify --quiet "${COMPONENT_COMMIT[$id]}^{commit}" > /dev/null; then
		warn "${COMPONENT_REPO[$id]} does not carry the pinned commit ${COMPONENT_COMMIT[$id]} (${COMPONENT_REF[$id]})"
		return 1
	fi

	note "${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} is pinned at ${COMPONENT_COMMIT[$id]}"

	return 0
}

#------------------------------------------------------------------------------
# Reproducibility: a clean tree, then patches with recorded digests
#
# "Checked out the pinned commit" is not the same claim as "built the pinned
# commit".  A retained tree carries whatever the last run, or a person debugging
# it, left behind: tracked files still modified, generated files from an older
# configure, objects from a different compiler.  `git checkout' does not remove any
# of that, so a build in a retained tree can produce a library that no commit
# describes - which is precisely how the toolkits this script pins came to be
# unreproducible in the first place.
#
# So the build tree is created FRESH from the mirror for every run, as a detached
# worktree, and it is asserted clean before anything touches it.  Then the
# compatibility patches this toolchain requires are applied from the repository,
# each verified against a SHA-256 recorded here first, and the set of files they
# changed is checked against the set they were supposed to change.  Every one of
# those steps happens before configure, so a contaminated or unexpected source tree
# fails the run instead of quietly producing a different library.
#
# Why patches at all: both pinned PTLib commits include <termio.h>, which glibc 2.42
# removed, and the C++ standard has moved under all three components since they were
# written.  The fixes are small, they are upstream's problem rather than this
# project's, and they have to be part of the pin or the pin is a fiction.  Each patch
# file documents what it repairs and why, and names the commit it applies to.
#------------------------------------------------------------------------------

readonly PATCH_DIR_RELATIVE='build/patches/endpoint_toolkits'

# Patches per component, in apply order.  Empty for a component that needs none -
# H323Plus builds as it stands, because the only file its retained tree carried
# modified was openh323u.mak, which its own configure GENERATES from
# openh323u.mak.in.
declare -A COMPONENT_PATCHES=(
	[ptlib_h323]='ptlib-2.10.9-termios.patch'
	[h323plus]=''
	[ptlib_opal]='ptlib-2.12-beta10-termios.patch ptlib-2.12-beta10-ifstream-pstring.patch ptlib-2.12-beta10-stack-min.patch ptlib-2.12-beta10-argspec-null.patch ptlib-2.12-beta10-revision.patch'
	[opal]='opal-3.12.10-msrp-printcontents.patch opal-3.12.10-revision.patch'
)

# The digest of every patch, so that "the patch in the tree" and "the patch this
# script was written against" are the same bytes.  A patch is a build input exactly
# as much as a commit is, and an unpinned build input is the thing this file exists
# to remove.
declare -A PATCH_SHA256=(
	['ptlib-2.10.9-termios.patch']='afe3f1afdd7b4355f9c498f4cb1e7042176a4551af2bfb090a1ed10b073260a6'
	['ptlib-2.12-beta10-termios.patch']='72b4665221a22e9634d625ef0ed91a65c2a228c10a60f32634354ea3ddc4557e'
	['ptlib-2.12-beta10-ifstream-pstring.patch']='6ee030c05299154eb44a48becaf7243b9d0dd5c1373110b4086e8db21137f2a5'
	['ptlib-2.12-beta10-stack-min.patch']='0bf46dcab8728618a0014fb36aa8b0217a92233c0859b9f73f6133054e504b86'
	['ptlib-2.12-beta10-argspec-null.patch']='adf08550157eccbda40d223c0463a9f0204022c06f802032e95d8bf03496b0c5'
	['ptlib-2.12-beta10-revision.patch']='52f53cbefffde6389158c3680eae513df76b98d8a0b2b0433c5d6232fceb1e16'
	['opal-3.12.10-msrp-printcontents.patch']='a5dbf794b82af82e082661fea5a386944ed044ba68af125fe5a6d525b95580f2'
	['opal-3.12.10-revision.patch']='848219fb18e769672f4a7fd74311188e0515e2cb48be88603d441d8c778a72ab'
)

# Where the build tree for one component lives.  Under the source root so it shares
# the mirror's filesystem, and named apart from the mirrors so neither can be
# mistaken for the other.
component_tree_dir()
{
	printf '%s\n' "$SRC_ROOT/build/${COMPONENT_SRCDIR[$1]}"
}

# Verify one patch against its recorded digest.
verify_patch_digest()
{
	local name="$1"
	local file="$FS_DIR/$PATCH_DIR_RELATIVE/$name"
	local expected="${PATCH_SHA256[$name]:-}"
	local observed

	if [ -z "$expected" ]; then
		warn "no SHA-256 is recorded for the patch '$name', so it cannot be applied"
		return 1
	fi

	if [ ! -r "$file" ]; then
		warn "the patch $file is missing or unreadable"
		return 1
	fi

	observed=$(sha256sum -- "$file" | awk '{ print $1 }')

	if [ "${#observed}" -ne 64 ] || [ "$observed" != "$expected" ]; then
		warn "the patch $name does not match its recorded SHA-256"
		warn "  recorded: $expected"
		warn "  on disk : ${observed:-unreadable}"
		return 1
	fi

	return 0
}

# Create a pristine detached worktree at the pinned commit, apply the pinned patches,
# and verify every step.  Sets COMPONENT_TREE to the directory that must be built.
COMPONENT_TREE=''

prepare_component_tree()
{
	local id="$1"
	local mirror="$SRC_ROOT/${COMPONENT_SRCDIR[$id]}"
	local tree
	local name
	local head
	local dirty
	local -a expected_files=()
	local -a changed_files=()
	local expected_list
	local changed_list
	local line

	COMPONENT_TREE=''

	tree=$(component_tree_dir "$id")

	# Guarded rather than trusted: this path is about to be removed, so it has to be
	# absolute and under the source root and it has to have a component name on the end
	case "$tree" in
		"$SRC_ROOT"/build/?*) ;;
		*)
			warn "refusing to prepare a build tree at '$tree'"
			return 1
			;;
	esac

	if [ -e "$tree" ] && ! rm -rf -- "$tree"; then
		warn "could not remove the previous build tree $tree"
		return 1
	fi

	if ! mkdir -p -- "$SRC_ROOT/build"; then
		warn "could not create $SRC_ROOT/build"
		return 1
	fi

	# A worktree the mirror still remembers but that no longer exists blocks re-adding
	# it, and one is left behind by exactly the rm above
	git -C "$mirror" worktree prune > /dev/null 2>&1

	if ! git -C "$mirror" worktree add --quiet --detach --force "$tree" "${COMPONENT_COMMIT[$id]}"; then
		warn "could not create a clean worktree of ${COMPONENT_COMMIT[$id]} at $tree"
		return 1
	fi

	head=$(git -C "$tree" rev-parse HEAD 2> /dev/null)

	if [ "$head" != "${COMPONENT_COMMIT[$id]}" ]; then
		warn "the build tree $tree is at '${head:-nothing}', not the pinned ${COMPONENT_COMMIT[$id]}"
		return 1
	fi

	dirty=$(git -C "$tree" status --porcelain 2> /dev/null)

	if [ -n "$dirty" ]; then
		warn "the freshly created build tree $tree is not clean, so it cannot be trusted to represent ${COMPONENT_COMMIT[$id]}:"
		printf '%s\n' "$dirty" | while IFS= read -r line; do
			warn "  $line"
		done

		return 1
	fi

	note "${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]}: clean worktree at $head"

	for name in ${COMPONENT_PATCHES[$id]}; do
		verify_patch_digest "$name" || return 1

		if ! git -C "$tree" apply --check -p1 -- "$FS_DIR/$PATCH_DIR_RELATIVE/$name"; then
			warn "the patch $name does not apply to ${COMPONENT_COMMIT[$id]}, so the pin and the patch have drifted apart"
			return 1
		fi

		# What the patch says it touches, before it touches it
		while IFS= read -r line; do
			expected_files+=("${line##*$'\t'}")
		done < <(git -C "$tree" apply --numstat -p1 -- "$FS_DIR/$PATCH_DIR_RELATIVE/$name")

		if ! git -C "$tree" apply -p1 -- "$FS_DIR/$PATCH_DIR_RELATIVE/$name"; then
			warn "the patch $name failed to apply to $tree after passing its own dry run"
			return 1
		fi

		note "  applied $name (sha256 ${PATCH_SHA256[$name]})"
	done

	# And what the tree says changed, after.  The two lists must be the same set: a
	# patched tree that differs anywhere else is not the pinned source plus known
	# repairs, which is the only thing this script is allowed to build.
	while IFS= read -r line; do
		changed_files+=("$line")
	done < <(git -C "$tree" diff --name-only)

	expected_list=$(printf '%s\n' "${expected_files[@]}" | LC_ALL=C sort -u)
	changed_list=$(printf '%s\n' "${changed_files[@]}" | LC_ALL=C sort -u)

	if [ "$expected_list" != "$changed_list" ]; then
		warn "the patched build tree $tree differs from ${COMPONENT_COMMIT[$id]} in files the patches do not name:"
		warn "  patches touch : ${expected_list//$'\n'/ }"
		warn "  tree changed  : ${changed_list//$'\n'/ }"

		return 1
	fi

	# The generated configure inputs are the pinned ones: configure.ac and configure
	# are both tracked in all four components and neither is in the changed set above,
	# so the configure that runs is the one the pinned commit ships.  Its presence and
	# executability are still asserted, because a tree without them fails much later.
	if [ ! -x "$tree/configure" ]; then
		warn "$tree carries no executable configure, so ${COMPONENT_NAME[$id]} cannot be configured reproducibly"
		return 1
	fi

	COMPONENT_TREE="$tree"

	return 0
}

# The environment one component is configured, built and installed in.
#
# H323Plus is the whole reason this exists.  Its configure looks for ptlib-config with
# AC_PATH_PROG over the HARDCODED list /usr/local/bin:/usr/bin:/opt/local/bin, so on a
# host carrying two PTLibs - which is every host this script provisions, since the OPAL
# stack brings its own 2.12 - it is free to select the wrong one, and H323Plus compiled
# against 2.12 headers while mod_h323 links 2.10.9 is a link that succeeds and a process
# that crashes.  PTLIB_CONFIG is therefore SET, which AC_PATH_PROG honours instead of
# searching, and pkg-config's search path is narrowed to the PTLib prefix so the OPAL
# one cannot be resolved either way.
#
# Printed as one assignment per line by the report, so what a provisioning run would do
# is inspectable without reading this function.
declare -a COMPONENT_ENV=()
declare -a COMPONENT_MAKE_VARS=()

# Variables that have to reach make on its COMMAND LINE rather than through the
# environment.
#
# H323Plus' generated openh323u.mak assigns PTLIBDIR unconditionally, so an environment
# variable of that name is overwritten by the makefile and only a command-line variable
# wins.  It has to win: the assignment configure bakes in is the PTLib PREFIX, while the
# makefile then includes $(PTLIBDIR)/make/ptlib.mak, and PTLib installs its make files
# under $prefix/share/ptlib.  H323Plus' own configure papers over the difference for
# exactly two prefixes - it rewrites /usr and /usr/local, and nothing else - so any other
# prefix fails at that include.  Passing it for every prefix makes all of them behave the
# way the two special-cased ones do.
component_make_vars()
{
	COMPONENT_MAKE_VARS=()

	if [ "$1" = 'h323plus' ]; then
		COMPONENT_MAKE_VARS+=("PTLIBDIR=$PTLIB_PREFIX/share/ptlib")
	fi

	return 0
}

component_build_env()
{
	local id="$1"

	COMPONENT_ENV=()

	if [ -n "${COMPONENT_CXX_STANDARD[$id]}" ]; then
		COMPONENT_ENV+=("CXX=${CXX:-g++} ${COMPONENT_CXX_STANDARD[$id]}")
	fi

	if [ "$id" != 'h323plus' ]; then
		return 0
	fi

	COMPONENT_ENV+=("PTLIB_CONFIG=$PTLIB_PREFIX/bin/ptlib-config")
	COMPONENT_ENV+=("PKG_CONFIG_PATH=$PTLIB_PREFIX/lib/pkgconfig")
	COMPONENT_ENV+=("PATH=$PTLIB_PREFIX/bin:$PATH")

	return 0
}

build_and_install_component()
{
	local id="$1"
	local dir="$2"
	local prefix
	local target="${COMPONENT_BUILD_TARGET[$id]}"
	local -a configure_args
	local -a extra_args=()

	prefix=$(prefix_for "$id")

	component_build_env "$id"
	component_make_vars "$id"

	# H323Plus configures against an INSTALLED PTLib, so the thing it is being routed at
	# has to be there before configure runs.  Checked rather than assumed: a missing
	# ptlib-config sends AC_PATH_PROG back to its hardcoded search list, which is exactly
	# the wrong-PTLib selection this routing exists to prevent.
	if [ "$id" = 'h323plus' ] && [ ! -x "$PTLIB_PREFIX/bin/ptlib-config" ]; then
		warn "$PTLIB_PREFIX/bin/ptlib-config is missing, so ${COMPONENT_NAME[h323plus]} cannot be routed at the PTLib under $PTLIB_PREFIX"
		return 1
	fi

	if [ -n "${COMPONENT_CONFIGURE[$id]}" ]; then
		read -ra extra_args <<< "${COMPONENT_CONFIGURE[$id]}"
	fi

	configure_args=(--prefix="$prefix" "${extra_args[@]}")

	note "configuring ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} for $prefix"

	if ! (cd "$dir" && env "${COMPONENT_ENV[@]}" ./configure "${configure_args[@]}"); then
		warn "configure failed for ${COMPONENT_NAME[$id]} in $dir"
		return 1
	fi

	note "building ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]}"

	if ! (cd "$dir" && env "${COMPONENT_ENV[@]}" "$MAKE" "${COMPONENT_MAKE_VARS[@]}" ${target:+"$target"}); then
		warn "build failed for ${COMPONENT_NAME[$id]} in $dir"
		return 1
	fi

	note "installing ${COMPONENT_NAME[$id]} ${COMPONENT_VERSION[$id]} into $prefix"

	if ! (cd "$dir" && $SUDO env "${COMPONENT_ENV[@]}" "$MAKE" "${COMPONENT_MAKE_VARS[@]}" install); then
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
		prepare_component_tree "$id" || return 1
		build_and_install_component "$id" "$COMPONENT_TREE" || return 1

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

#------------------------------------------------------------------------------
# Making a chosen prefix reachable by the module
#
# mod_h323's own Makefile.am compiles -I/usr/include/openh323 and links -L/usr/lib
# (src/mod/endpoints/mod_h323/Makefile.am:6 and :10).  Those paths are hardcoded in
# the module, not chosen here, so a stack installed anywhere else is invisible to the
# build no matter how correct it is - and that is why this project's own host put the
# H.323 stack under /usr while the documented prefix is /usr/local.
#
# Installing into the documented default and then reporting failure would be useless,
# so the gap is BRIDGED: for a prefix other than /usr, the few paths the module looks
# for are symlinked at the stack that was just installed, and one ld.so.conf.d entry
# puts its library directory on the loader's path.  This is the same host integration
# the OPAL side of this script has always reported for its .pc files, done rather than
# merely described.
#
# Two rules keep it safe.  Nothing that already exists is touched - a path this script
# did not create is left exactly as it is, so a host with a real /usr/include/openhh323
# from a distribution package is never disturbed - and everything it does create is
# named in --uninstall-check, so the integration is reversible by inspection.
#------------------------------------------------------------------------------

readonly H323_LD_CONF='/etc/ld.so.conf.d/ptlib-h323plus.conf'

declare -a H323_BRIDGE_TARGET=()
declare -a H323_BRIDGE_SOURCE=()

# The paths mod_h323 looks for, and what they would point at under the chosen prefix.
# Empty for --ptlib-prefix=/usr, where the module already looks in the right place.
h323_bridge_table()
{
	H323_BRIDGE_TARGET=()
	H323_BRIDGE_SOURCE=()

	if [ "$PTLIB_PREFIX" = '/usr' ]; then
		return 0
	fi

	H323_BRIDGE_TARGET+=('/usr/include/openh323')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/include/openh323")

	H323_BRIDGE_TARGET+=('/usr/include/ptlib.h')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/include/ptlib.h")

	H323_BRIDGE_TARGET+=('/usr/include/ptbuildopts.h')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/include/ptbuildopts.h")

	H323_BRIDGE_TARGET+=('/usr/include/ptlib')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/include/ptlib")

	H323_BRIDGE_TARGET+=('/usr/include/ptclib')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/include/ptclib")

	H323_BRIDGE_TARGET+=('/usr/lib/libpt.so')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/lib/libpt.so")

	H323_BRIDGE_TARGET+=('/usr/lib/libopenh323.so')
	H323_BRIDGE_SOURCE+=("$PTLIB_PREFIX/lib/libopenh323.so")

	return 0
}

integrate_h323_host_paths()
{
	local index
	local target
	local source
	local created='no'

	h323_bridge_table

	if [ "${#H323_BRIDGE_TARGET[@]}" -eq 0 ]; then
		return 0
	fi

	note "bridging the stack under $PTLIB_PREFIX onto the paths mod_h323 hardcodes"

	if [ ! -e "$H323_LD_CONF" ]; then
		if ! printf '%s\n' "$PTLIB_PREFIX/lib" | $SUDO tee -- "$H323_LD_CONF" > /dev/null; then
			warn "could not create $H323_LD_CONF, so $PTLIB_PREFIX/lib stays off the loader path"
			return 1
		fi

		note "created $H323_LD_CONF carrying $PTLIB_PREFIX/lib"
		created='yes'
	fi

	for index in "${!H323_BRIDGE_TARGET[@]}"; do
		target="${H323_BRIDGE_TARGET[$index]}"
		source="${H323_BRIDGE_SOURCE[$index]}"

		# -e is false for a dangling symlink, so -L is asked as well: a path that
		# exists in ANY form belongs to whoever put it there
		if [ -e "$target" ] || [ -L "$target" ]; then
			note "leaving $target as it is; this script never replaces a path it did not create"
			continue
		fi

		if [ ! -e "$source" ]; then
			warn "$source does not exist, so $target cannot be bridged to it"
			return 1
		fi

		if ! $SUDO ln -s -- "$source" "$target"; then
			warn "could not link $target to $source"
			return 1
		fi

		note "created $target -> $source"
		created='yes'
	done

	if [ "$created" = 'yes' ] && command -v ldconfig > /dev/null 2>&1; then
		$SUDO ldconfig > /dev/null 2>&1 ||
			warn 'ldconfig did not run, so the bridged libraries may not be visible to the loader yet'
	fi

	return 0
}

# Does the stack under the CHOSEN prefix work?
#
# Asked against that prefix, through the same shared guard pointed at it, because the
# thing just installed is the thing to verify: a correct install into /usr/local is a
# correct install whether or not the module can reach it yet, and conflating the two
# was what made the documented default report failure after a successful install.
verify_prefix_install()
{
	local verdict
	local linkable

	H323_PROBE_PREFIX="$PTLIB_PREFIX"
	cve_gate
	H323_PROBE_PREFIX=''

	verdict="$CVE_VERDICT"
	linkable="$H323_TOOLKIT_LINKABLE"

	if ! enforce_cve_gate; then
		return "$EX_REFUSED"
	fi

	if [ "$linkable" != 'yes' ]; then
		warn "post-install verification failed: the stack under $PTLIB_PREFIX cannot compile and link mod_h323's own inputs (verdict '$verdict')"
		warn "expected $PTLIB_PREFIX/include/ptlib.h, $PTLIB_PREFIX/include/openh323/h323.h and $PTLIB_PREFIX/lib/libpt.so with $PTLIB_PREFIX/lib/libopenh323.so"

		return "$EX_PROVISION"
	fi

	# A prefix-scoped probe adds -I and -L, it does not remove the compiler's and
	# linker's default paths, so a prefix that produced no library at all could be
	# carried by a stack somewhere else.  The library that would actually LOAD has to be
	# the one just installed, or this function has verified the wrong thing.
	case "$H323_RESOLVED_LIBPT" in
		"$PTLIB_PREFIX"/*) ;;
		*)
			warn "post-install verification failed: a probe of $PTLIB_PREFIX resolved '${H323_RESOLVED_LIBPT:-no libpt}', which is not under that prefix, so the install did not produce the library that would load"

			return "$EX_PROVISION"
			;;
	esac

	note "the stack under $PTLIB_PREFIX verifies: mod_h323's inputs are satisfied there, $H323_RESOLVED_LIBPT is what would load, and the CVE-2013-1864 verdict is '$verdict'"

	return 0
}

# Re-observe after installing and refuse to call the run a success if anything
# regressed.  An install that leaves the gate unable to clear the library it just put
# in place is worse than no install at all, because the next build would use it.
#
# Two questions in order: did the install work where it was made, and can the module
# reach it.  The second is bridged if it can be and reported precisely if it cannot.
verify_after_install()
{
	local status

	observe_pkg_config_state

	# The pre-install refusal text is no longer true once something is on disk
	CVE_GATE_STAGE='the install completed, so this toolkit is on disk and must not be used until it clears'

	verify_prefix_install
	status=$?

	if [ "$status" != '0' ]; then
		return "$status"
	fi

	cve_gate

	if ! enforce_cve_gate; then
		return "$EX_REFUSED"
	fi

	if [ "$H323_TOOLKIT_LINKABLE" != 'yes' ]; then
		if ! integrate_h323_host_paths; then
			warn "post-install verification failed: the stack under $PTLIB_PREFIX could not be bridged onto the paths mod_h323 hardcodes"

			return "$EX_PROVISION"
		fi

		cve_gate

		if ! enforce_cve_gate; then
			return "$EX_REFUSED"
		fi

		if [ "$H323_TOOLKIT_LINKABLE" != 'yes' ]; then
			warn "post-install verification failed: mod_h323's own inputs still do not resolve after bridging $PTLIB_PREFIX onto them"
			warn 'mod_h323 compiles -I/usr/include/openh323 and links -L/usr/lib (src/mod/endpoints/mod_h323/Makefile.am:6 and :10), so --ptlib-prefix=/usr installs it where the module looks'

			return "$EX_PROVISION"
		fi
	fi

	note "mod_h323's own compile and link inputs resolve, so ci.sh's capability guard will enable endpoints/mod_h323"

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
# The self-test: prove the refusal, hermetically, without installing anything
#------------------------------------------------------------------------------
#
# The CVE-2013-1864 gate exists for one outcome, and it is the outcome no healthy
# host can produce: a PTLib that does not bound XML entity expansion.  On every
# machine this script has ever run on, the gate cleared - so the branch that
# actually protects anything has never executed, and a refusal path that has never
# executed is a refusal path nobody knows works.  A comment claiming it does is not
# evidence.
#
# So it is exercised here, against a scratch PTLib STUB built by ci.sh's own stub
# builder - the same fixture ci.sh --guard-self-test uses, because a second copy of
# a security fixture is a second thing to get wrong.  The whole provisioning flow
# runs, pointed at the stub by h323_probe_search_prefix() and PKG_CONFIG_LIBDIR,
# and it has to end in the named refusal with the documented exit status.
#
# Four properties are asserted, and the third and fourth are what make this a proof
# rather than a demonstration:
#
#   1. the refusal fires, by its stable text, for both unsafe verdicts
#   2. the two SAFE verdicts still clear - including the expat-less one this
#      project's own hosts depend on, so the adjudication both scripts share is
#      proven rather than asserted
#   3. fetch, source preparation, build, install and host integration are never
#      entered.  Not "were not observed to run": each one is replaced by a tripwire
#      that records the attempt, and the absence of that record is the assertion
#   4. the real compiler, the real pkg-config answers and the SHA-256 of every
#      installed toolkit library are identical before and after
#
# Nothing outside one scratch directory is written, no .pc file is read outside the
# stub, and no prefix is touched.  The mode is safe to run on a production host.
#------------------------------------------------------------------------------

# Build one stub SDK with ci.sh's builder.
self_test_build_stub()
{
	local dir="$1"
	local flavour="$2"
	local output

	output=$(
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

		if ! declare -F h323_probe_stub_sdk > /dev/null 2>&1; then
			printf '%s\n' "$CI_SCRIPT publishes no h323_probe_stub_sdk"
			exit 1
		fi

		h323_probe_stub_sdk "$dir" "$flavour" 2>&1
	)

	if [ ! -r "$dir/lib/pkgconfig/ptlib.pc" ] || [ ! -e "$dir/lib/libpt.so" ]; then
		warn "could not build a '$flavour' stub PTLib SDK in $dir"

		if [ -n "$output" ]; then
			warn "  $output"
		fi

		return 1
	fi

	return 0
}

# A fingerprint of everything this mode must not change.
#
# The installed libraries are digested rather than merely listed, because a proof
# that "the toolkits are still there" is not a proof that they are the same bytes.
self_test_state_fingerprint()
{
	local line
	local path

	printf 'compiler=%s\n' "$(command -v "${CXX:-g++}" 2> /dev/null)"
	printf 'compiler-version=%s\n' "$(${CXX:-g++} --version 2> /dev/null | head -1)"
	printf 'pkg-config-ptlib=%s@%s\n' \
		"$(pkg-config --modversion ptlib 2> /dev/null)" \
		"$(pkg-config --variable=libdir ptlib 2> /dev/null)"
	printf 'pkg-config-opal=%s@%s\n' \
		"$(pkg-config --modversion opal 2> /dev/null)" \
		"$(pkg-config --variable=libdir opal 2> /dev/null)"

	if ! command -v ldconfig > /dev/null 2>&1; then
		printf 'ldconfig=absent\n'
		return 0
	fi

	# Every toolkit library the loader knows about, in a stable order
	while IFS= read -r line; do
		path="${line##*=> }"

		if [ -e "$path" ]; then
			printf 'library=%s %s\n' "$path" "$(sha256sum -- "$path" 2> /dev/null | awk '{ print $1 }')"
		fi
	done < <(ldconfig -p 2> /dev/null |
		grep -E 'libpt\.so|libopenh323\.so|libh323_|libopal\.so' | LC_ALL=C sort -u)

	return 0
}

# Point every path the flow consults at one scratch tree, and pkg-config at the stub's
# own .pc directory.
#
# PKG_CONFIG_LIBDIR rather than PKG_CONFIG_PATH because it REPLACES pkg-config's search
# path: with it set, no installed .pc file is read at all, so the stub cannot be
# confused with the host's real toolkit.  Always called inside a subshell, so none of
# this reaches the run that invoked the self-test.
self_test_redirect_state()
{
	local dir="$1"
	local mode="$2"

	PTLIB_PREFIX="$dir/sdk"
	OPAL_PREFIX="$dir/sdk"
	SRC_ROOT="$dir/src"
	MODE="$mode"
	H323_PROBE_PREFIX="$dir/sdk"

	PKG_CONFIG_LIBDIR="$dir/sdk/lib/pkgconfig"
	export PKG_CONFIG_LIBDIR
	unset PKG_CONFIG_PATH

	return 0
}

# Run the whole provisioning flow against one stub SDK, with tripwires in place of
# everything that could change the host.
#
# Returns the flow's own exit status, and leaves its output in $dir/out and $dir/err
# and any tripwire record in $dir/tripwire.
self_test_run_flow()
{
	local dir="$1"
	local mode="$2"

	(
		self_test_redirect_state "$dir" "$mode"

		# The tripwires.  Each one records the attempt and fails, so a flow that
		# reached it neither changes anything nor passes quietly.
		fetch_component()
		{
			printf 'fetch_component %s\n' "$1" >> "$dir/tripwire"
			return 1
		}

		prepare_component_tree()
		{
			printf 'prepare_component_tree %s\n' "$1" >> "$dir/tripwire"
			return 1
		}

		build_and_install_component()
		{
			printf 'build_and_install_component %s\n' "$1" >> "$dir/tripwire"
			return 1
		}

		integrate_h323_host_paths()
		{
			printf 'integrate_h323_host_paths\n' >> "$dir/tripwire"
			return 1
		}

		provision_run
	) > "$dir/out" 2> "$dir/err"

	return $?
}

# Report one assertion in a fixed, greppable shape.
self_test_report()
{
	local status="$1"
	local what="$2"

	if [ "$status" -eq 0 ]; then
		say "  PASS: $what"
	else
		warn "FAIL: $what"
	fi

	return "$status"
}

# One unsafe verdict: the flow must end in the named refusal and touch nothing.
self_test_refusal_case()
{
	local root="$1"
	local flavour="$2"
	local expected_verdict="$3"
	local dir="$root/$flavour"
	local status
	local observed

	mkdir -p -- "$dir/sdk" || return 1
	self_test_build_stub "$dir/sdk" "$flavour" || return 1

	# What the shared verdict says about this stub, on its own, so the end-to-end
	# refusal below is attributable to a specific verdict rather than to anything
	# that happens to fail
	observed=$(
		self_test_redirect_state "$dir" 'report'

		cve_gate > /dev/null 2>&1
		printf '%s\n' "$CVE_VERDICT"
	)

	if [ "$observed" != "$expected_verdict" ]; then
		warn "the shared verdict called a '$flavour' stub PTLib '$observed', expected '$expected_verdict'"
		return 1
	fi

	self_test_run_flow "$dir" 'provision'
	status=$?

	if [ "$status" != "$EX_REFUSED" ]; then
		warn "the flow exited $status against a '$flavour' stub PTLib, expected $EX_REFUSED"
		sed 's/^/    /' "$dir/err" >&2
		return 1
	fi

	if ! grep -qF "REFUSED (CVE-2013-1864)" "$dir/err"; then
		warn "the flow refused a '$flavour' stub PTLib without the named refusal text"
		sed 's/^/    /' "$dir/err" >&2
		return 1
	fi

	if ! grep -qF 'nothing was fetched, built or installed' "$dir/err"; then
		warn "the refusal for a '$flavour' stub PTLib does not state that nothing was fetched, built or installed"
		return 1
	fi

	if [ -e "$dir/tripwire" ]; then
		warn "the flow entered a step that changes the host after refusing a '$flavour' stub PTLib:"
		sed 's/^/    /' "$dir/tripwire" >&2
		return 1
	fi

	return 0
}

# One safe verdict: the flow must NOT refuse, and must still touch nothing.
#
# Run in report mode, so even a defect in this test cannot install anything: the
# report path has no install step to reach.
self_test_clearance_case()
{
	local root="$1"
	local flavour="$2"
	local expected_verdict="$3"
	local dir="$root/$flavour"
	local status

	mkdir -p -- "$dir/sdk" || return 1
	self_test_build_stub "$dir/sdk" "$flavour" || return 1

	self_test_run_flow "$dir" 'report'
	status=$?

	if [ "$status" != '0' ]; then
		warn "the flow exited $status against a '$flavour' stub PTLib, which the gate must clear"
		sed 's/^/    /' "$dir/err" >&2
		return 1
	fi

	if ! grep -qE "CVE-2013-1864 verdict +: $expected_verdict\$" "$dir/out"; then
		warn "the report for a '$flavour' stub PTLib does not carry the verdict '$expected_verdict'"
		grep -F 'CVE-2013-1864 verdict' "$dir/out" | sed 's/^/    /' >&2
		return 1
	fi

	if [ -e "$dir/tripwire" ]; then
		warn "the report mode entered a step that changes the host for a '$flavour' stub PTLib:"
		sed 's/^/    /' "$dir/tripwire" >&2
		return 1
	fi

	return 0
}

# Every committed patch is the bytes this script was written against.
self_test_patch_digests()
{
	local name
	local failures=0

	for name in "${!PATCH_SHA256[@]}"; do
		verify_patch_digest "$name" || failures=$((failures + 1))
	done

	if [ "$failures" -ne 0 ]; then
		return 1
	fi

	return 0
}

self_test()
{
	local root
	local before
	local after
	local failures=0

	say "$PROG: self-test - nothing is fetched, built or installed"
	say ''
	say "  ci.sh under test : $CI_SCRIPT"
	say "  patches          : $FS_DIR/$PATCH_DIR_RELATIVE (${#PATCH_SHA256[@]} pinned)"
	say ''

	if ! new_scratch_dir; then
		warn 'no scratch directory could be created, so the self-test cannot run hermetically'
		return "$EX_SELFTEST"
	fi

	root="$SCRATCH_LAST"

	before=$(self_test_state_fingerprint)

	self_test_patch_digests
	self_test_report $? 'every pinned patch matches its recorded SHA-256' || failures=$((failures + 1))

	self_test_refusal_case "$root" 'unbounded' 'vulnerable'
	self_test_report $? 'a PTLib that ignores its entity ceiling is refused by name, and nothing is fetched, built or installed' || failures=$((failures + 1))

	self_test_refusal_case "$root" 'no_ceiling_api' 'vulnerable'
	self_test_report $? 'a PTLib whose parser predates the ceiling API is refused by name, and nothing is fetched, built or installed' || failures=$((failures + 1))

	self_test_clearance_case "$root" 'no_parser' 'clear_no_parser'
	self_test_report $? 'a PTLib built without expat is cleared, because it carries no parser to be vulnerable with' || failures=$((failures + 1))

	self_test_clearance_case "$root" 'bounded' 'clear'
	self_test_report $? 'a PTLib that bounds entity expansion is cleared' || failures=$((failures + 1))

	after=$(self_test_state_fingerprint)

	if [ "$before" = "$after" ]; then
		self_test_report 0 'the real compiler, pkg-config answers and installed-toolkit digests are unchanged'
	else
		self_test_report 1 'the real compiler, pkg-config answers and installed-toolkit digests are unchanged'
		diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") >&2
		failures=$((failures + 1))
	fi

	discard_scratch_dir "$root" || failures=$((failures + 1))

	say ''

	if [ "$failures" -ne 0 ]; then
		warn "self-test: $failures of 6 assertions failed"
		return "$EX_SELFTEST"
	fi

	note 'self-test: all 6 assertions passed'

	return "$(final_status "$EX_OK")"
}

#------------------------------------------------------------------------------
# main
#------------------------------------------------------------------------------

# Fold a scratch-cleanup failure into an otherwise successful status.
#
# A directory this script created and could not remove is a leak on a build host, and
# a leak that only ever produced a warning is a leak nobody acts on.  A status that was
# already non-zero is left alone: the first failure is the one worth reporting.
final_status()
{
	local status="$1"

	if [ "$status" = '0' ] && [ "$SCRATCH_CLEANUP_FAILED" = 'yes' ]; then
		warn 'a scratch directory this script created could not be removed; see the warnings above'
		printf '%s\n' "$EX_PROVISION"

		return 0
	fi

	printf '%s\n' "$status"

	return 0
}

main()
{
	if ! parse_args "$@"; then
		warn "try '$0 --help'"
		return "$EX_USAGE"
	fi

	if ! normalise_directory_options; then
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

	if [ "$MODE" = 'self-test' ]; then
		self_test

		return $?
	fi

	provision_run

	return $?
}

# Everything a provisioning or reporting run does, from the banner to the summary.
#
# Separated from main() so that the self-test above can run the WHOLE flow - the same
# observation, the same gate, the same enforcement, the same order - against a stub
# SDK, rather than re-implementing an approximation of it and proving something else.
provision_run()
{
	say "$PROG: pinned endpoint toolkit provisioning for $FS_DIR"
	say ''
	say "  mode          : $MODE"
	say "  ptlib prefix  : $PTLIB_PREFIX   (PTLib ${COMPONENT_VERSION[ptlib_h323]} + H323Plus ${COMPONENT_VERSION[h323plus]})"
	say "  opal prefix   : $OPAL_PREFIX   (OPAL ${COMPONENT_VERSION[opal]} + PTLib ${COMPONENT_VERSION[ptlib_opal]})"
	say "  source root   : $SRC_ROOT"
	say "  H.323 verdict : $CI_SCRIPT (h323_guard_evaluate, sourced - one shared implementation)"
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

		return "$(final_status "$EX_OK")"
	fi

	if [ "$H323_TOOLKIT_LINKABLE" = 'yes' ] && [ "$OPAL_USABLE" = 'yes' ]; then
		note 'both toolkits are already present and usable; nothing to install'
		say ''
		report_pkg_config_path
		print_summary

		return "$(final_status "$EX_OK")"
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

	return "$(final_status "$EX_OK")"
}

main "$@"
exit $?
