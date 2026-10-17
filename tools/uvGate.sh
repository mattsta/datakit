#!/bin/sh
# Run a command in the dktools uv environment for a ctest gate.
#
#     sh uvGate.sh UV TOOLS_DIR COMMAND [ARGS...]
#
# Exits with COMMAND's status, or 77 (the gate's SKIP_RETURN_CODE) when
# the environment cannot be set up: uv must fetch Python 3.12, the
# build backend, and the libclang wheel on first use, so an offline
# machine without a cached environment skips the gate instead of
# failing it.  --frozen keeps uv from rewriting uv.lock during a test.
set -u

if [ "$#" -lt 3 ]; then
    echo "usage: uvGate.sh UV TOOLS_DIR COMMAND [ARGS...]" >&2
    exit 2
fi

uv=$1
tools=$2
shift 2

if ! setupLog=$("$uv" run --frozen --project "$tools" dktools --help 2>&1); then
    echo "dktools environment unavailable (offline, or uv cannot fetch" \
        "its dependencies); skipping:" >&2
    printf '%s\n' "$setupLog" >&2
    exit 77
fi

exec "$uv" run --frozen --project "$tools" "$@"
