"""Named compilers, presets and the git helpers behind verify/validate.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from dktools import gitutil, toolchain
from dktools.gitutil import ToolError
from repofixture import IsolatedGitEnv, TempRepo


class Compilers(unittest.TestCase):
    def test_clang22_is_the_default_and_never_system_cc(self) -> None:
        self.assertEqual(toolchain.DEFAULT_COMPILER, "clang22")
        clang = toolchain.resolve_compiler("clang22", env={})
        bindir = toolchain.DEFAULT_LLVM_DIR
        self.assertEqual(clang.cc, f"{bindir}/clang-22")
        self.assertEqual(
            clang.cmake_defines(),
            [
                f"-DCMAKE_C_COMPILER={bindir}/clang-22",
                f"-DCMAKE_CXX_COMPILER={bindir}/clang++",
                f"-DCMAKE_AR={bindir}/llvm-ar",
                f"-DCMAKE_RANLIB={bindir}/llvm-ranlib",
            ],
        )
        for name in toolchain.COMPILERS:
            compiler = toolchain.resolve_compiler(name, env={})
            for flag in compiler.cmake_defines():
                self.assertNotIn("/usr/bin/cc", flag)

    def test_gcc15_flags(self) -> None:
        gcc = toolchain.resolve_compiler("gcc15", env={})
        self.assertEqual(
            gcc.cmake_defines(),
            ["-DCMAKE_C_COMPILER=gcc-15", "-DCMAKE_CXX_COMPILER=g++-15"],
        )

    def test_flag_beats_env_beats_default(self) -> None:
        env = {
            toolchain.ENV_LLVM_DIR: "envdir",
            toolchain.ENV_GCC_CC: "env-gcc",
            toolchain.ENV_GCC_CXX: "env-g++",
        }
        self.assertEqual(
            toolchain.resolve_compiler("clang22", env=env).ar, "envdir/llvm-ar"
        )
        self.assertEqual(
            toolchain.resolve_compiler("clang22", llvm_dir="flagdir", env=env).cc,
            "flagdir/clang-22",
        )
        gcc = toolchain.resolve_compiler("gcc15", gcc_cc="flag-gcc", env=env)
        self.assertEqual((gcc.cc, gcc.cxx), ("flag-gcc", "env-g++"))

    def test_unknown_compiler_and_missing_program(self) -> None:
        with self.assertRaises(ToolError):
            toolchain.resolve_compiler("cc", env={})
        missing = toolchain.resolve_compiler(
            "clang22", llvm_dir="/nonexistent-dir", env={}
        )
        with self.assertRaisesRegex(ToolError, "compiler clang22"):
            missing.check()


class Presets(unittest.TestCase):
    def setUp(self) -> None:
        self.clang = toolchain.resolve_compiler("clang22", env={})
        self.gcc = toolchain.resolve_compiler("gcc15", env={})

    def test_preset_commands(self) -> None:
        src = Path("tree")
        dev = toolchain.build_config(self.clang, None)
        self.assertEqual(dev.preset, "dev")
        cmd = dev.configure_command(src)
        self.assertEqual(
            cmd[:7],
            ["cmake", "-S", "tree", "-B", "tree/build-clang22-dev", "--preset", "dev"],
        )
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Release", cmd)

        rel = toolchain.build_config(self.gcc, None)
        self.assertEqual(rel.preset, "release")
        cmd = rel.configure_command(src)
        self.assertNotIn("--preset", cmd)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", cmd)

        self.assertEqual(
            dev.build_command(src, 8, toolchain.PRESET_TARGETS),
            [
                "cmake",
                "--build",
                "tree/build-clang22-dev",
                "-j",
                "8",
                "--target",
                *toolchain.PRESET_TARGETS,
            ],
        )
        self.assertNotIn("--target", dev.build_command(src, 8, None))

    def test_unsupported_pairs(self) -> None:
        with self.assertRaises(ToolError):
            toolchain.build_config(self.gcc, "dev")
        with self.assertRaises(ToolError):
            toolchain.build_config(self.clang, "release")

    def test_matrix(self) -> None:
        configs = toolchain.matrix([self.clang, self.gcc], ["dev", "debug"])
        self.assertEqual(
            [c.label for c in configs],
            ["clang22-dev", "clang22-debug", "gcc15-release"],
        )
        with self.assertRaises(ToolError):
            toolchain.matrix([self.clang], ["nosuch"])

    def test_perf_preset_is_clang22_only(self) -> None:
        perf = toolchain.build_config(self.clang, "perf")
        self.assertEqual(
            perf.configure_command(Path("tree"))[:7],
            [
                "cmake",
                "-S",
                "tree",
                "-B",
                "tree/build-clang22-perf",
                "--preset",
                "perf",
            ],
        )
        with self.assertRaises(ToolError):
            toolchain.build_config(self.gcc, "perf")
        # perf is opt-in: never a compiler's default build.
        self.assertEqual(toolchain.build_config(self.clang, None).preset, "dev")

    def test_presets_file_check(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            dev = toolchain.build_config(self.clang, "dev")
            with self.assertRaises(ToolError):
                dev.check_presets_file(root)
            (root / "CMakePresets.json").write_text(
                json.dumps({"configurePresets": [{"name": "dev"}]})
            )
            dev.check_presets_file(root)
            with self.assertRaises(ToolError):
                toolchain.build_config(self.clang, "debug").check_presets_file(root)


class GitTrees(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.repo.write("a.txt", "a1\n")
        self.repo.write("b.txt", "b1\n")
        self.repo.write("dir/c.txt", "c1\n")
        self.repo.commit("base", "a.txt", "b.txt", "dir/c.txt")
        self.work = Path(tempfile.mkdtemp(dir=self.repo.path.parent))

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def tree_files(self, tree: str) -> dict[str, bytes]:
        entries = gitutil.ls_tree(self.repo.path, tree)
        blobs = gitutil.cat_blobs(
            self.repo.path, (oid for _, _, oid in entries.values())
        )
        return {path: blobs[oid] for path, (_, _, oid) in entries.items()}

    def test_build_tree_takes_only_the_named_paths(self) -> None:
        self.repo.write("a.txt", "a2\n")
        self.repo.write("b.txt", "b2 never staged\n")
        (self.repo.path / "dir" / "c.txt").unlink()
        self.repo.write("new.txt", "new\n")
        other = self.work / "replacement"
        other.write_text("from file\n")
        index_before = self.repo.git("ls-files", "-s")

        tree = gitutil.build_tree(
            self.repo.path,
            self.work / "index",
            [
                ("a.txt", None),
                ("dir/c.txt", None),
                ("new.txt", None),
                ("x/y.txt", other),
            ],
        )
        files = self.tree_files(tree)
        self.assertEqual(
            files,
            {
                "a.txt": b"a2\n",
                "b.txt": b"b1\n",
                "new.txt": b"new\n",
                "x/y.txt": b"from file\n",
            },
        )
        # The real index is untouched.
        self.assertEqual(self.repo.git("ls-files", "-s"), index_before)

        out = self.work / "export"
        gitutil.export_tree(self.repo.path, tree, out)
        self.assertEqual((out / "x" / "y.txt").read_text(), "from file\n")
        self.assertFalse((out / "dir").exists())

    def test_build_tree_errors(self) -> None:
        index = self.work / "index"
        for spec in (("absent.txt", None), ("dir", None)):
            with self.subTest(spec=spec), self.assertRaises(ToolError):
                gitutil.build_tree(self.repo.path, index, [spec])
        with self.assertRaises(ToolError):
            gitutil.build_tree(self.repo.path, index, [("a.txt", self.work / "nope")])
        for bad in ("/abs/path", "../up", ""):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                gitutil.repo_path(bad)

    def test_resolve_commit_and_names(self) -> None:
        head = self.repo.git("rev-parse", "HEAD")
        self.assertEqual(gitutil.resolve_commit(self.repo.path, "HEAD"), head)
        for bad in ("no-such-rev", "--all"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                gitutil.resolve_commit(self.repo.path, bad)
        with self.assertRaises(ToolError):
            gitutil.export_tree(self.repo.path, "no-such-rev", self.work / "x")
        for bad in ("", "..", "a/b", "-x"):
            with self.subTest(name=bad), self.assertRaises(ToolError):
                gitutil.check_name(bad)
        self.assertEqual(gitutil.check_name("ok-name"), "ok-name")

    def test_scratch_root(self) -> None:
        self.assertEqual(
            gitutil.scratch_root(Path("flag"), {gitutil.ENV_SCRATCH: "env"}),
            Path("flag"),
        )
        self.assertEqual(
            gitutil.scratch_root(None, {gitutil.ENV_SCRATCH: "env"}), Path("env")
        )
        self.assertEqual(gitutil.scratch_root(None, {}).name, "dktools")


if __name__ == "__main__":
    unittest.main()
