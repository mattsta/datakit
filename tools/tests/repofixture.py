"""Throwaway git repositories for dktools tests.

Each repository lives in a TemporaryDirectory and runs git with the
user's global and system configuration disabled, so the tests behave
the same on every machine.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

#: Environment for every git command a test runs (and for the code
#: under test, through os.environ, while a TempRepo is active).
GIT_ENV = {
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_AUTHOR_NAME": "dktools test",
    "GIT_AUTHOR_EMAIL": "test@example.invalid",
    "GIT_COMMITTER_NAME": "dktools test",
    "GIT_COMMITTER_EMAIL": "test@example.invalid",
}


class IsolatedGitEnv:
    """Context manager: apply GIT_ENV to os.environ, restore on exit."""

    def __enter__(self) -> IsolatedGitEnv:
        self._saved = {k: os.environ.get(k) for k in GIT_ENV}
        os.environ.update(GIT_ENV)
        return self

    def __exit__(self, *exc: object) -> None:
        for key, value in self._saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class TempRepo:
    """A git repository in a temporary directory."""

    def __init__(self, parent: Path | None = None) -> None:
        self._dir = tempfile.TemporaryDirectory(dir=parent)
        self.path = Path(self._dir.name) / "repo"
        self.path.mkdir()
        self.git("init", "-q", "-b", "main")

    def cleanup(self) -> None:
        self._dir.cleanup()

    def git(self, *args: str) -> str:
        env = dict(os.environ)
        env.update(GIT_ENV)
        proc = subprocess.run(
            ["git", "-C", str(self.path), *args],
            capture_output=True,
            env=env,
            check=True,
        )
        return proc.stdout.decode().strip()

    def write(self, rel: str, text: str | bytes, mode: int | None = None) -> Path:
        path = self.path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text, encoding="utf-8")
        if mode is not None:
            path.chmod(mode)
        return path

    def commit(self, message: str, *paths: str) -> str:
        """Stage exactly `paths` (added, changed or deleted) and commit."""
        if not paths:
            raise ValueError("commit needs explicit paths")
        self.git("add", "--", *paths)
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")
