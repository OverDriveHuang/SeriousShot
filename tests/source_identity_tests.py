"""Source provenance regression tests. Git repositories are isolated fixtures."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
CMAKE = os.environ.get("HDRSHOT_TEST_CMAKE") or shutil.which("cmake") or str(PROJECT / ".build-tools/cmake-venv/bin/cmake")


class SourceIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="seriousshot-identity-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.source = self.repo / "projects/product"
        self.source.mkdir(parents=True)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Identity Test")
        self.git("config", "user.email", "test@example.invalid")
        self.put("CMakeLists.txt", "project(Test VERSION 0.1.0)\n")
        self.put("src/main.cpp", "int main() {}\n")
        self.commit()
        self.initial = self.git("rev-parse", "HEAD").strip()
        self.time = "2026-01-02T03:04:05+08:00"

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.PIPE).decode()

    def put(self, name, content):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self):
        self.git("add", "--all")
        # Deliberately different author/committer times: the latter is required.
        subprocess.check_call(["git", "-C", str(self.repo), "commit", "-qm", "fixture"],
            env={**os.environ, "GIT_AUTHOR_DATE": "2025-01-01T00:00:00+00:00",
                 "GIT_COMMITTER_DATE": "2026-01-02T03:04:05+08:00"})

    def generate(self, source=None, release=False, succeeds=True):
        output = self.root / "metadata.cpp"
        result = subprocess.run([CMAKE, f"-DSOURCE_DIR={source or self.source}",
            "-DPRODUCT_VERSION=0.1.0", f"-DOUTPUT_FILE={output}",
            f"-DREQUIRE_CLEAN_SOURCE={'ON' if release else 'OFF'}", "-P",
            str(PROJECT / "cmake/GenerateBuildTimestamp.cmake")], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
        if not succeeds:
            self.assertIn("Release requires known, committed", result.stderr)
            return None
        cpp = output.read_text()
        fields = {name: value for name, value in re.findall(r'(\w+)\(\) noexcept \{ return "([^"]*)";', cpp)}
        fields["modified"] = "return true;" in cpp
        return fields

    def test_clean_repeat_and_committer_time(self):
        first = self.generate(release=True)
        self.assertEqual(first["product_version"], "0.1.0")
        self.assertEqual(first["source_commit"], self.initial)
        self.assertEqual(first["source_commit_timestamp"], self.time)
        self.assertFalse(first["modified"])
        self.assertEqual(first, self.generate())

    def test_unrelated_commits_and_dirty_docs_do_not_change_identity(self):
        self.put("scratchpad/note.md", "note")
        (self.repo / "other-project.txt").write_text("other")
        self.commit()
        self.assertNotEqual(self.initial, self.git("rev-parse", "HEAD").strip())
        self.put("scratchpad/note.md", "uncommitted")
        (self.repo / "other-project.txt").write_text("dirty")
        result = self.generate(release=True)
        self.assertEqual(result["source_commit"], self.initial)
        self.assertFalse(result["modified"])

    def test_code_commit_advances_identity(self):
        self.put("src/main.cpp", "int main() { return 0; }\n")
        self.commit()
        self.assertEqual(self.generate()["source_commit"], self.git("rev-parse", "HEAD").strip())
        self.assertNotEqual(self.generate()["source_commit"], self.initial)

    def test_modified_staged_untracked_and_deleted_inputs(self):
        self.put("src/main.cpp", "changed")
        self.assertTrue(self.generate()["modified"])
        self.generate(release=True, succeeds=False)
        self.git("add", "projects/product/src/main.cpp")
        self.assertTrue(self.generate()["modified"])
        self.commit()
        self.put("src/new.cpp", "new")
        self.assertTrue(self.generate()["modified"])
        self.commit()
        (self.source / "src/main.cpp").unlink()
        self.assertTrue(self.generate()["modified"])

    def test_without_git_and_legacy_receipt_are_unknown(self):
        archive = self.root / "archive"
        shutil.copytree(self.source, archive)
        self.assertEqual(self.generate(archive)["source_commit"], "")
        self.generate(archive, release=True, succeeds=False)
        self.put(".seriousshot-export.json", json.dumps({"files": {}}))
        self.assertEqual(self.generate()["source_commit"], "")

    def receipt(self, source):
        files = {p.relative_to(source).as_posix(): {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(), "mode": 420}
                 for p in source.rglob("*") if p.is_file()}
        (source / ".seriousshot-export.json").write_text(json.dumps({
            "code_identity": {"commit": self.initial, "timestamp": self.time}, "files": files}))

    def test_export_receipt_survives_archive_and_independent_history(self):
        archive = self.root / "archive"
        shutil.copytree(self.source, archive)
        self.receipt(archive)
        self.assertEqual(self.generate(archive, release=True)["source_commit"], self.initial)
        subprocess.check_call(["git", "init", "-q", str(archive)])
        subprocess.check_call(["git", "-C", str(archive), "add", "."])
        subprocess.check_call(["git", "-C", str(archive), "-c", "user.name=Test", "-c",
                               "user.email=test@example.invalid", "commit", "-qm", "export"])
        self.assertEqual(self.generate(archive, release=True)["source_commit_timestamp"], self.time)
        (archive / "src/main.cpp").write_text("public-only change")
        self.assertTrue(self.generate(archive)["modified"])
        self.generate(archive, release=True, succeeds=False)

    def test_archive_receipt_detects_added_deleted_changed_files(self):
        archive = self.root / "archive"
        shutil.copytree(self.source, archive)
        self.receipt(archive)
        path = archive / "src/main.cpp"
        original = path.read_text()
        path.write_text("changed")
        self.assertTrue(self.generate(archive)["modified"])
        path.write_text(original)
        self.assertFalse(self.generate(archive)["modified"])
        path.unlink()
        self.assertTrue(self.generate(archive)["modified"])
        path.write_text(original)
        (archive / "src/new.cpp").write_text("new")
        self.assertTrue(self.generate(archive)["modified"])

    def test_malformed_receipt_does_not_inject_generated_code(self):
        self.put(".seriousshot-export.json", json.dumps({"code_identity": {
            "commit": '"; malicious', "timestamp": self.time}, "files": {}}))
        self.assertEqual(self.generate()["source_commit"], "")
        self.put(".seriousshot-export.json", "broken json")
        self.assertEqual(self.generate()["source_commit"], "")

    def test_shallow_history_cannot_invent_code_time(self):
        shallow = self.root / "shallow"
        subprocess.check_call(["git", "clone", "-q", "--depth=1", self.repo.as_uri(), str(shallow)])
        self.assertEqual(self.generate(shallow / "projects/product")["source_commit"], "")
        self.generate(shallow / "projects/product", release=True, succeeds=False)


if __name__ == "__main__":
    unittest.main()
