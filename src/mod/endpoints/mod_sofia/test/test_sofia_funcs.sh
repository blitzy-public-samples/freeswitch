#!/usr/bin/env bash
### shfmt -w -s -ci -sr -kp -fn src/mod/endpoints/mod_sofia/test/test_sofia_funcs.sh

#
# Wrapper for the collected mod_sofia unit test.
#
# Automake collects this script through the module's TESTS declaration, so the status it
# exits with is the verdict the CI runner records for test_sofia_funcs.  Its second job
# is to publish the STIR/SHAKEN certificate fixture over HTTP on 127.0.0.1:8080, because
# the passports the identity cases present carry x5u=http://127.0.0.1:8080/cert.pem and
# the verifier fetches that URL while the test runs.
#
# DISCLOSED OUT-OF-AAP RECONCILIATION.  mod_sofia is a REFERENCE artifact in the Agent
# Action Plan (0.2.2, resolution C3) and is otherwise read-only for this engagement.
# This wrapper is repaired regardless, because QA finding RUNNER-SHELL-001 fails the
# acceptance rows CI-13, I11 and QG3 on it.  As shipped, line 1 was the comment
# "#/bin/sh" instead of a shebang, so executing the script directly returned 126 with
# "Exec format error"; "cd test" contradicted the runner's contract of invoking every
# test from the directory that contains it (tests/unit/test.sh); "pushd"/"popd" are Bash
# builtins that a real /bin/sh would reject; and Python 2's SimpleHTTPServer module no
# longer exists, so the certificate fixture was never served at all.  Each of those
# failures was discarded and the script still exited with the test binary's status, so
# the runner reported success for a test whose setup had failed.  The tree carries no
# failure budget, which makes that false green a correctness defect rather than a
# cosmetic one.  The repair is confined to this wrapper -- no production source, no
# public header and no module behaviour is touched -- following the disclosed-addendum
# precedent already accepted in this engagement for the core defect in src/switch_event.c.
#

set -u

readonly test_name="test_sofia_funcs"
readonly fixture_host="127.0.0.1"
readonly fixture_port="8080"
readonly ready_timeout_seconds=20

# Populated as setup progresses; the EXIT trap consults them, so they must exist before
# the trap is installed (the script runs under "set -u").
script_dir=""
server_log=""
fetch_scratch=""
server_pid=""
test_pid=""
server_should_be_running=0

fail()
{
	echo "$test_name.sh: $*" >&2
	exit 1
}

# Surfaces why the fixture server refused to come up -- "Address already in use" when a
# foreign process owns the port is the common case, and silently swallowing it is the
# whole substance of RUNNER-SHELL-001.
dump_server_log()
{
	if [ -n "$server_log" ] && [ -s "$server_log" ]; then
		echo "$test_name.sh: fixture HTTP server output follows" >&2
		sed -n '1,20p' "$server_log" >&2
	fi
}

# Retrieves $1 into $2 using the interpreter that is already a hard requirement for the
# fixture server, so readiness checking adds no further dependency such as curl or wget.
fetch_cert()
{
	"$python_bin" -c 'import sys, urllib.request; sys.stdout.buffer.write(urllib.request.urlopen(sys.argv[1], timeout=5).read())' "$1" > "$2" 2> /dev/null
}

# Runs on every exit path, including "fail" above and any signal, so no HTTP server and
# no scratch file outlives the test.  A server that died while the tests were running is
# itself a setup failure and turns a zero status non-zero.
# shellcheck disable=SC2317 # reached through the trap installed below, never called directly
cleanup()
{
	local status=$?

	# Only reachable when this wrapper is ending early, because the normal path waits
	# for the test to finish.  Left alone, the test process would outlive the wrapper
	# still holding whatever ports its core opened.
	if [ -n "$test_pid" ] && kill -0 "$test_pid" 2> /dev/null; then
		echo "$test_name.sh: terminating test process $test_pid" >&2
		kill "$test_pid" 2> /dev/null
		wait "$test_pid" 2> /dev/null
	fi

	if [ -n "$server_pid" ]; then
		if [ "$server_should_be_running" -eq 1 ] && ! kill -0 "$server_pid" 2> /dev/null; then
			echo "$test_name.sh: the fixture HTTP server on $fixture_host:$fixture_port exited before the tests finished" >&2
			dump_server_log
			if [ "$status" -eq 0 ]; then
				status=1
			fi
		fi

		kill "$server_pid" 2> /dev/null
		wait "$server_pid" 2> /dev/null
		server_pid=""
	fi

	[ -n "$server_log" ] && rm -f "$server_log"
	[ -n "$fetch_scratch" ] && rm -f "$fetch_scratch"

	trap - EXIT
	exit "$status"
}

# Being signalled is not success either, so each signal handler exits with the
# conventional 128+signal status and leaves the actual cleaning to the EXIT trap, which
# every one of those exits passes through.
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 131' QUIT
trap 'exit 143' TERM
trap cleanup EXIT

# Locate ourselves rather than assuming a working directory.  The runner already enters
# the directory holding the collected test, but a developer invoking the script by any
# other path has to reach the same fixtures and the same binary.
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" > /dev/null 2>&1 && pwd) ||
	fail "cannot determine the directory containing this script"

test_binary="$script_dir/$test_name"
fixture_root="$script_dir/stir-shaken/www"
fixture_cert="$fixture_root/cert.pem"
cert_url="http://$fixture_host:$fixture_port/cert.pem"

[ -x "$test_binary" ] || fail "$test_binary is missing or not executable; build the module before running its tests"
[ -d "$fixture_root" ] || fail "$fixture_root is missing; the certificate fixture cannot be served"
[ -r "$fixture_cert" ] || fail "$fixture_cert is missing or unreadable; the certificate fixture cannot be served"

# Python 2's SimpleHTTPServer is gone, so use the Python 3 http.server module, and probe
# for an interpreter that actually provides it (with the 3.7 --directory option) instead
# of hard-coding a name that may resolve to either major version.
python_bin=""
for candidate in python3 python; do
	if command -v "$candidate" > /dev/null 2>&1 &&
		"$candidate" -c 'import http.server, sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)' > /dev/null 2>&1; then
		python_bin="$candidate"
		break
	fi
done

[ -n "$python_bin" ] || fail "no Python 3.7+ interpreter providing http.server was found, so $cert_url cannot be served"

server_log=$(mktemp "${TMPDIR:-/tmp}/$test_name-httpd.XXXXXX") || fail "cannot create a temporary file for the fixture server log"
fetch_scratch=$(mktemp "${TMPDIR:-/tmp}/$test_name-cert.XXXXXX") || fail "cannot create a temporary file for the readiness probe"

"$python_bin" -m http.server "$fixture_port" --bind "$fixture_host" --directory "$fixture_root" > "$server_log" 2>&1 &
server_pid=$!

# Wait until the fixture is genuinely retrievable, and compare what comes back with the
# file on disk.  A plain connect check would accept a foreign process that already owns
# the port and serves something else entirely; comparing the bytes is what distinguishes
# "our fixture server is up" from "something is listening".
server_ready=0
ready_deadline=$((SECONDS + ready_timeout_seconds))

while [ "$SECONDS" -lt "$ready_deadline" ]; do
	if ! kill -0 "$server_pid" 2> /dev/null; then
		break
	fi

	if fetch_cert "$cert_url" "$fetch_scratch" && cmp -s "$fetch_scratch" "$fixture_cert"; then
		server_ready=1
		break
	fi

	sleep 0.2
done

if [ "$server_ready" -ne 1 ]; then
	dump_server_log

	if kill -0 "$server_pid" 2> /dev/null; then
		fail "$cert_url did not return the contents of $fixture_cert within ${ready_timeout_seconds}s; a foreign process may already own port $fixture_port"
	fi

	fail "the fixture HTTP server on $fixture_host:$fixture_port exited during startup"
fi

server_should_be_running=1

# Run from the directory holding the test so that any relative artefact the binary writes
# lands beside it, as it does under the runner.  The binary is started as a background job
# only so that its pid is known to the cleanup above; the wait immediately below makes
# this synchronous, and its status is the test's own.
cd "$script_dir" || fail "cannot enter $script_dir"

"$test_binary" "$@" &
test_pid=$!
wait "$test_pid"
test_status=$?

# The trap performs cleanup and preserves this status, raising it only if cleanup itself
# discovers a failure.
exit "$test_status"
