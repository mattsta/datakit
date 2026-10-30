"""Tiny programs and pool functions for the ``procs`` tests.

Pool functions live here, not in the test module, because spawn
workers import them by name.  The programs run as
``python -m prochelpers COMMAND ...`` with this directory as cwd.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path

#: Python child that sleeps until killed.
SLEEP_FOREVER = [sys.executable, "-c", "import time; time.sleep(600)"]


def alive(pid: int) -> bool:
    """True while `pid` exists and is not a zombie we could reap."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        done = 0  # not our child: launchd/init reaps it
    if done == pid:
        return False
    state = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True
    ).stdout.strip()
    return bool(state) and not state.startswith("Z")


def wait_gone(pids: list[int], seconds: float = 10.0) -> list[int]:
    """The pids still alive after up to `seconds`."""
    deadline = time.monotonic() + seconds
    left = list(pids)
    while left and time.monotonic() < deadline:
        left = [p for p in left if alive(p)]
        if left:
            time.sleep(0.05)
    return left


def read_pids(directory: Path) -> list[int]:
    return [int(p.read_text()) for p in directory.glob("*.pid") if p.read_text()]


# -- pool functions (module level: pickled by name) ---------------------


def square(x: int) -> int:
    return x * x


def boom_on_three(x: int) -> int:
    if x == 3:
        raise ValueError("three is right out")
    return x


def die_on_three(x: int) -> int:
    if x == 3:
        os.kill(os.getpid(), signal.SIGKILL)
    return x


def hang_recording_pid(directory: str) -> int:
    """Record this worker's pid under `directory`, then never return."""
    Path(directory, f"{os.getpid()}.pid").write_text(str(os.getpid()))
    while True:
        time.sleep(1)


def die_audit(args: tuple[str, str, bool]) -> object:
    """Stands in for allocaudit._audit_one: the worker dies."""
    os.kill(os.getpid(), signal.SIGKILL)
    raise AssertionError("unreachable")


def hang_audit(args: tuple[str, str, bool]) -> object:
    """Stands in for allocaudit._audit_one: the worker records its pid
    in the audited repo's directory and never returns."""
    return hang_recording_pid(args[0])


# -- programs -----------------------------------------------------------


def _spawn_grandchild(pidfile: str) -> None:
    """Start a sleeping grandchild (same process group), record its
    pid, then sleep: a stand-in for cmake running make running cc."""
    grandchild = subprocess.Popen(SLEEP_FOREVER)
    Path(pidfile).write_text(str(grandchild.pid))
    time.sleep(600)


def _orphan_holding_stdout(pidfile: str) -> None:
    """Exit at once, leaving a grandchild that holds stdout open."""
    grandchild = subprocess.Popen(SLEEP_FOREVER)
    Path(pidfile).write_text(str(grandchild.pid))


def _pool_parent(directory: str) -> None:
    """Run a pool whose workers hang; the test SIGKILLs this process."""
    from dktools import procs

    procs.parallel_map(
        hang_recording_pid, [directory, directory], jobs=2, stall_timeout=None
    )


def _noisy(count: str) -> None:
    """Write `count` bytes to stdout and to stderr, interleaved."""
    chunk = b"x" * 4096
    for _ in range(int(count) // len(chunk)):
        sys.stdout.buffer.write(chunk)
        sys.stderr.buffer.write(chunk)
    sys.stdout.flush()
    sys.stderr.flush()


def _write_fd(fd: str) -> None:
    """Write one byte to the inherited descriptor `fd`."""
    os.write(int(fd), b"x")


COMMANDS = {
    "spawn-grandchild": _spawn_grandchild,
    "orphan-holding-stdout": _orphan_holding_stdout,
    "pool-parent": _pool_parent,
    "noisy": _noisy,
    "write-fd": _write_fd,
}


def command(name: str, *args: str) -> list[str]:
    """argv running COMMANDS[name] with `args`."""
    return [sys.executable, "-m", "prochelpers", name, *args]


if __name__ == "__main__":
    COMMANDS[sys.argv[1]](*sys.argv[2:])
