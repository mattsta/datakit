"""Child processes that cannot outlive, hang or orphan dktools.

Every external program dktools starts goes through this module:

- ``run`` (output captured) and ``run_to_file`` (output to an open
  file) start the program from an argument list, never a shell, with
  stdin from /dev/null and in its own session, so the program and
  everything it starts form one process group.  On a timeout, Ctrl-C,
  SIGTERM or any other exception the whole group gets SIGTERM, then
  SIGKILL after a short grace period, and the program is reaped before
  the exception propagates: no cmake, make, compiler or ctest process
  is left running.  A timeout raises ``TimedOut``.

- ``parallel_map`` runs a function over items in a process pool
  started with "spawn" (never "fork": a forked child inherits locks
  held by other threads of the parent).  A worker that dies (a signal,
  the OOM killer) or a pool where no item finishes within the stall
  timeout fails with a one-line ``ToolError``; the workers are killed
  and the pool shut down before the error propagates.  Workers ignore
  SIGINT (the parent cleans up on Ctrl-C) and exit on their own when
  the parent dies.

- ``signal_handlers`` turns SIGTERM and SIGHUP into
  ``SystemExit`` so the cleanup above also runs when dktools is
  terminated rather than interrupted, and makes SIGUSR1 print every
  thread's stack to stderr (``kill -USR1 PID`` on a process that
  looks stuck shows where it waits).
"""

from __future__ import annotations

import faulthandler
import multiprocessing
import multiprocessing.connection
import os
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterator, Mapping, Sequence
from concurrent.futures import FIRST_COMPLETED, Future, ProcessPoolExecutor, wait
from concurrent.futures.process import BrokenProcessPool
from contextlib import contextmanager, suppress
from pathlib import Path
from typing import IO, Any, TypeVar

T = TypeVar("T")
R = TypeVar("R")

#: Seconds between SIGTERM and SIGKILL when a process group is stopped.
KILL_GRACE = 2.0

#: Seconds parallel_map waits for killed workers and the pool's
#: manager thread before giving up on them.
POOL_JOIN_GRACE = 10.0

#: Exit code of a program dktools stopped on a timeout (as timeout(1)).
TIMEOUT_CODE = 124


class ToolError(Exception):
    """An input or environment error: exit code 2, one stderr line.

    Defined here, the lowest layer, and re-exported by ``gitutil``."""


class TimedOut(Exception):
    """A program ran past its timeout; its process group was killed."""

    def __init__(self, cmd: Sequence[str], timeout: float) -> None:
        super().__init__(f"{cmd[0]}: timed out after {timeout:g}s")
        self.cmd = list(cmd)
        self.timeout = timeout


# -- signals ----------------------------------------------------------

_DEFERRED = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)


@contextmanager
def signals_deferred() -> Iterator[None]:
    """Hold SIGINT, SIGTERM and SIGHUP until the block ends, so a
    second Ctrl-C cannot abandon a cleanup halfway; a signal that
    arrives meanwhile is raised again, to its own handler, at the end.

    The handlers are swapped rather than the signals masked: Python
    runs handlers in the main thread whichever thread the kernel
    delivers to, and a pool has other threads.  Handlers can only be
    changed from the main thread; elsewhere this is a no-op."""
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    caught: list[int] = []

    def record(signum: int, frame: object) -> None:
        caught.append(signum)

    saved = {}
    for sig in _DEFERRED:
        old = signal.getsignal(sig)
        saved[sig] = signal.SIG_DFL if old is None else old
        signal.signal(sig, record)
    try:
        yield
    finally:
        for sig, handler in saved.items():
            signal.signal(sig, handler)
        for sig in dict.fromkeys(caught):
            signal.raise_signal(sig)


def _register_stack_dump() -> bool:
    """SIGUSR1 prints every thread's stack to the process's real
    stderr (fd 2, whatever sys.stderr is redirected to); False when
    there is no such file."""
    try:
        faulthandler.register(signal.SIGUSR1, file=sys.__stderr__, all_threads=True)
    except (AttributeError, ValueError, OSError):
        return False
    return True


def _exit_on_signal(signum: int, frame: object) -> None:
    raise SystemExit(128 + signum)


@contextmanager
def signal_handlers() -> Iterator[None]:
    """For the block: SIGTERM/SIGHUP raise SystemExit(128+N) so finally
    blocks run, and SIGUSR1 dumps every thread's stack to stderr.  The
    previous handlers come back afterwards (the tests call the CLI
    in-process)."""
    saved = {
        sig: signal.signal(sig, _exit_on_signal)
        for sig in (signal.SIGTERM, signal.SIGHUP)
    }
    dumping = _register_stack_dump()
    try:
        yield
    finally:
        if dumping:
            faulthandler.unregister(signal.SIGUSR1)
        for sig, handler in saved.items():
            signal.signal(sig, signal.SIG_DFL if handler is None else handler)


# -- single programs ----------------------------------------------------


def kill_group(proc: subprocess.Popen, grace: float = KILL_GRACE) -> None:
    """Stop `proc` and every process in its group, then reap `proc`.

    SIGTERM to the group, up to `grace` seconds for `proc` to exit,
    then SIGKILL to the group (stragglers that ignored SIGTERM or
    outlived their parent).  Our ends of its pipes are closed without
    reading them: a grandchild that escaped the group may hold the
    other ends open forever.  Never raises for an already-dead group."""
    with signals_deferred():
        _signal_group(proc, signal.SIGTERM)
        with suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=grace)
        _signal_group(proc, signal.SIGKILL)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            if stream is not None:
                with suppress(OSError):
                    stream.close()
        proc.wait()


def _signal_group(proc: subprocess.Popen, sig: int) -> None:
    # Gone, or (macOS) only zombies are left in the group.
    with suppress(ProcessLookupError, PermissionError):
        os.killpg(proc.pid, sig)


def _popen(
    cmd: Sequence[str],
    *,
    cwd: Path | None,
    env: Mapping[str, str] | None,
    stdin: Any,
    stdout: Any,
    stderr: Any,
) -> subprocess.Popen:
    """Start `cmd` in a new session; ToolError when the program or
    `cwd` does not exist."""
    try:
        return subprocess.Popen(
            list(cmd),
            cwd=cwd,
            env=None if env is None else dict(env),
            stdin=stdin,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
    except FileNotFoundError as exc:
        if cwd is not None and str(exc.filename) == str(cwd):
            raise ToolError(f"{cwd}: no such directory") from exc
        raise ToolError(f"{cmd[0]}: not found on PATH") from exc


def run(
    cmd: Sequence[str],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    input: bytes | None = None,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[bytes]:
    """Run `cmd` with stdout and stderr captured (``communicate``, so
    neither pipe can fill and stall the program) and return them.

    `env` is the program's whole environment (None: inherit).  Raises
    ``TimedOut`` after `timeout` seconds and ``ToolError`` when the
    program does not exist; the process group is stopped and reaped
    before any exception, KeyboardInterrupt included, propagates."""
    proc = _popen(
        cmd,
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL if input is None else subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        out, err = proc.communicate(input, timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_group(proc)
        assert timeout is not None
        raise TimedOut(cmd, timeout) from None
    except BaseException:
        kill_group(proc)
        raise
    return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)


def run_to_file(
    cmd: Sequence[str],
    out: IO[bytes],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
) -> int:
    """Run `cmd` with stdout and stderr both written to `out` (an open
    file, so there is no pipe to fill) and return its exit code.

    Raises like ``run``; the process group is stopped and reaped first."""
    out.flush()
    proc = _popen(
        cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=out
    )
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_group(proc)
        assert timeout is not None
        raise TimedOut(cmd, timeout) from None
    except BaseException:
        kill_group(proc)
        raise


@contextmanager
def streaming(
    cmd: Sequence[str],
    stderr: IO[bytes],
    *,
    cwd: Path | None = None,
) -> Iterator[subprocess.Popen]:
    """Start `cmd` with stdout on a pipe for the caller to read and
    stderr to `stderr` (an open file: stderr on a second pipe that is
    read only afterwards deadlocks once it fills).  On leaving the
    block the program is waited for; on an exception its process group
    is stopped and reaped first.  The caller checks ``returncode``."""
    proc = _popen(
        cmd,
        cwd=cwd,
        env=None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=stderr,
    )
    try:
        yield proc
    except BaseException:
        kill_group(proc)
        raise
    assert proc.stdout is not None
    proc.stdout.close()
    proc.wait()


# -- process pools ------------------------------------------------------


def _watch_parent(parent: int) -> None:
    """Worker thread: exit the worker once its parent is gone (the
    worker is re-parented), so a SIGKILLed dktools leaves no workers
    blocked forever on the call queue."""
    while True:
        time.sleep(0.5)
        if os.getppid() != parent:
            os._exit(1)


def _worker_init(parent: int, init: Callable[[], None] | None) -> None:
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    _register_stack_dump()
    threading.Thread(target=_watch_parent, args=(parent,), daemon=True).start()
    if init is not None:
        init()


def parallel_map(
    fn: Callable[[T], R],
    items: Sequence[T],
    *,
    jobs: int,
    stall_timeout: float | None,
    init: Callable[[], None] | None = None,
    describe: Callable[[T], str] = str,
    noun: str = "item",
) -> list[R]:
    """``[fn(item) for item in items]`` across `jobs` worker processes.

    `fn` and `init` must be module-level functions (they are pickled
    by name); `init` runs once in each worker before any item.  Items
    are handed out one at a time in the given order (put the slowest
    first), results come back in that order.

    Raises ``ToolError`` (one line) when a worker process dies, when
    `fn` raises (naming the item, via `describe`), or when no item
    finishes for `stall_timeout` seconds (naming the items in flight;
    `noun` is what the message calls an item).
    Before any exception propagates, KeyboardInterrupt included, every
    worker is killed and the pool is shut down."""
    if not items:
        return []
    if jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {jobs})")
    pool = ProcessPoolExecutor(
        max_workers=min(jobs, len(items)),
        mp_context=multiprocessing.get_context("spawn"),
        initializer=_worker_init,
        initargs=(os.getpid(), init),
    )
    finished = False
    try:
        futures: dict[Future[R], int] = {}
        for i, item in enumerate(items):
            futures[pool.submit(fn, item)] = i
        results: list[Any] = [None] * len(items)
        pending = set(futures)
        while pending:
            done, pending = wait(
                pending, timeout=stall_timeout, return_when=FIRST_COMPLETED
            )
            if not done:
                assert stall_timeout is not None
                busy = sorted(futures[f] for f in pending if f.running())
                names = ", ".join(describe(items[i]) for i in busy) or "none started"
                raise ToolError(
                    f"no {noun} finished in {stall_timeout:g}s, "
                    f"{len(pending)} left (in flight: {names})"
                )
            for future in done:
                i = futures[future]
                try:
                    results[i] = future.result()
                except BrokenProcessPool as exc:
                    raise ToolError(
                        "a worker process died (killed by a signal or out of "
                        f"memory?) while {describe(items[i])} was queued or "
                        "in progress"
                    ) from exc
                except Exception as exc:
                    raise ToolError(
                        f"{describe(items[i])}: {type(exc).__name__}: {exc}"
                    ) from exc
        finished = True
        return results
    finally:
        if finished:
            pool.shutdown(wait=True)
        else:
            _abandon_pool(pool)


def _abandon_pool(pool: ProcessPoolExecutor) -> None:
    """Kill every worker of `pool` and shut it down without waiting on
    work that will never finish.  Bounded: never blocks for more than
    about 2 * POOL_JOIN_GRACE seconds."""
    with signals_deferred():
        workers = list((getattr(pool, "_processes", None) or {}).values())
        manager = getattr(pool, "_executor_manager_thread", None)
        pool.shutdown(wait=False, cancel_futures=True)
        for worker in workers:
            with suppress(OSError, ValueError):
                worker.kill()
        sentinels = []
        for worker in workers:
            with suppress(ValueError):
                sentinels.append(worker.sentinel)
        deadline = time.monotonic() + POOL_JOIN_GRACE
        while sentinels and time.monotonic() < deadline:
            ready = multiprocessing.connection.wait(
                sentinels, timeout=deadline - time.monotonic()
            )
            sentinels = [s for s in sentinels if s not in ready]
        # With every worker dead the manager thread sees their
        # sentinels, marks the pool broken and returns.
        if manager is not None:
            manager.join(timeout=POOL_JOIN_GRACE)
        if sentinels or (manager is not None and manager.is_alive()):
            print(
                "dktools: warning: process pool did not stop cleanly",
                file=sys.stderr,
            )
