#!/usr/bin/env python3
# Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.
from __future__ import annotations

import json
import os
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


SCRIPT = Path(__file__).with_name("release_package.py")
COMMIT = "1" * 40
UPSTREAM = "5ebd9b57984d5854e0a37fd0125da48e9e59e190"
EPOCH = "1760000000"


class ReleasePackageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.package_root = self.root / "AICraft-Luanti"
        (self.package_root / "bin").mkdir(parents=True)
        executable = self.package_root / "bin" / "aicraft-agent-client"
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        (self.package_root / "LICENSE.txt").write_text("LGPL\n", encoding="utf-8")
        os.symlink("aicraft-agent-client", self.package_root / "bin" / "agent")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_script(self, *arguments: str) -> None:
        subprocess.run(
            ["python3", str(SCRIPT), *arguments],
            check=True,
            cwd=self.root,
        )

    def test_manifest_records_exact_source_and_honest_signing_state(self) -> None:
        arguments = self.root / "arguments.txt"
        arguments.write_text("-DBUILD_AGENT_CLIENT=ON\n", encoding="utf-8")
        manifest = self.package_root / "AICRAFT_RELEASE.json"
        self.run_script(
            "manifest",
            "--output",
            str(manifest),
            "--version",
            "5.16.1-aicraft.123456789abc",
            "--target",
            "linux-x86_64",
            "--repository",
            "https://github.com/Holdge/AICraft-Luanti",
            "--commit",
            COMMIT,
            "--upstream-commit",
            UPSTREAM,
            "--source-epoch",
            EPOCH,
            "--workflow",
            "test",
            "--run-id",
            "42",
            "--runner",
            "unit-test",
            "--build-arguments",
            str(arguments),
            "--dependency",
            f"fixture|local|{'a' * 64}",
            "--signing-status",
            "unsigned",
            "--no-notarized",
        )
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        self.assertEqual(payload["source"]["commit"], COMMIT)
        self.assertEqual(payload["source"]["upstream_luanti_5_16_1_commit"], UPSTREAM)
        self.assertEqual(payload["code_signing"], {"notarized": False, "status": "unsigned"})
        self.assertNotIn("token", manifest.read_text(encoding="utf-8").lower())

    def test_tar_and_zip_are_deterministic_and_preserve_executable_and_symlink(self) -> None:
        for archive_format, suffix in (("tar.gz", ".tar.gz"), ("zip", ".zip")):
            first = self.root / f"first{suffix}"
            second = self.root / f"second{suffix}"
            for output in (first, second):
                self.run_script(
                    "archive",
                    "--root",
                    str(self.package_root),
                    "--output",
                    str(output),
                    "--checksum",
                    str(output.with_suffix(output.suffix + ".sha256")),
                    "--format",
                    archive_format,
                    "--source-epoch",
                    EPOCH,
                )
            self.assertEqual(first.read_bytes(), second.read_bytes())

        with tarfile.open(self.root / "first.tar.gz", "r:gz") as archive:
            executable = archive.getmember("AICraft-Luanti/bin/aicraft-agent-client")
            link = archive.getmember("AICraft-Luanti/bin/agent")
            self.assertEqual(executable.mode, 0o755)
            self.assertTrue(link.issym())

        with zipfile.ZipFile(self.root / "first.zip") as archive:
            executable = archive.getinfo("AICraft-Luanti/bin/aicraft-agent-client")
            link = archive.getinfo("AICraft-Luanti/bin/agent")
            self.assertEqual((executable.external_attr >> 16) & 0o777, 0o755)
            self.assertTrue(((link.external_attr >> 16) & 0o170000) == 0o120000)


if __name__ == "__main__":
    unittest.main()
