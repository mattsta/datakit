"""``uvGate.sh``: only environment setup may skip (77); the command's
own failures, crashes and signals fail the gate with its status.

A fake ``uv`` stands in: ``sync`` succeeds unless FAKE_UV_OFFLINE is
set, ``run --frozen --project DIR CMD...`` execs CMD.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

GATE = Path(__file__).resolve().parents[1] / "uvGate.sh"

FAKE_UV = """\
#!/bin/sh
case "$1" in
sync)
    if [ -n "${FAKE_UV_OFFLINE:-}" ]; then
        echo "error: failed to fetch python" >&2
        exit 1
    fi
    exit 0 ;;
run)
    shift 4  # run --frozen --project DIR
    exec "$@" ;;
esac
exit 99
"""


class UvGate(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.uv = Path(self._tmp.name) / "uv"
        self.uv.write_text(FAKE_UV)
        self.uv.chmod(0o755)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def gate(self, *cmd: str, offline: bool = False) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env.pop("FAKE_UV_OFFLINE", None)
        if offline:
            env["FAKE_UV_OFFLINE"] = "1"
        return subprocess.run(
            ["sh", str(GATE), str(self.uv), self._tmp.name, *cmd],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )

    def test_command_status_passes_through(self) -> None:
        self.assertEqual(self.gate("true").returncode, 0)
        self.assertEqual(self.gate("sh", "-c", "exit 3").returncode, 3)

    def test_offline_setup_skips(self) -> None:
        proc = self.gate("true", offline=True)
        self.assertEqual(proc.returncode, 77)
        self.assertIn("skipping", proc.stderr)
        self.assertIn("failed to fetch python", proc.stderr)

    def test_a_crashing_command_fails_instead_of_skipping(self) -> None:
        # The command itself dies of a signal: not a skip.
        proc = self.gate("sh", "-c", "kill -TERM $$")
        self.assertNotIn(proc.returncode, (0, 77))

    def test_usage(self) -> None:
        proc = subprocess.run(
            ["sh", str(GATE), str(self.uv), self._tmp.name],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(proc.returncode, 2)
        self.assertIn("usage:", proc.stderr)


if __name__ == "__main__":
    unittest.main()
