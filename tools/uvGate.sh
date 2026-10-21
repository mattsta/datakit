#!/bin/sh
# Run a command in the dktools uv environment for a ctest gate.
#
#     sh uvGate.sh UV TOOLS_DIR COMMAND [ARGS...]
#
# Exits with COMMAND's status, or 77 (the gate's SKIP_RETURN_CODE) when
# the environment cannot be set up: uv must fetch Python 3.12, the
# build backend, and the libclang wheel on first use, so an offline
# machine without a cached environment skips the gate instead of
# failing it.  Only that setup step can skip: once the environment
# exists, a COMMAND that fails, crashes or is killed fails the gate
# with its own status (a broken dktools must never read as a skip).
# --frozen keeps uv from rewriting uv.lock during a test, and
# --inexact matches what `uv run` itself syncs.
#
# COMMAND replaces this shell (exec), so a signal ctest sends on a
# timeout reaches uv, which passes SIGTERM and SIGINT on to COMMAND;
# dktools then stops its own children before exiting.
set -u

if [ "$#" -lt 3 ]; then
    echo "usage: uvGate.sh UV TOOLS_DIR COMMAND [ARGS...]" >&2
    exit 2
fi

uv=$1
tools=$2
shift 2

if ! setupLog=$("$uv" sync --frozen --inexact --project "$tools" 2>&1); then
    echo "dktools environment unavailable (offline, or uv cannot fetch" \
        "its dependencies); skipping:" >&2
    printf '%s\n' "$setupLog" >&2
    exit 77
fi

exec "$uv" run --frozen --project "$tools" "$@"
