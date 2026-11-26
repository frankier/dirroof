"""Run with make && python3 -m unittest discover -s tests."""

import contextlib
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
import unittest

BINARY = Path(
    os.environ.get("DIRROOF_BINARY", Path(__file__).resolve().parents[1] / "dirroof")
).resolve()


class RecursiveEvictionTest(unittest.TestCase):
    def setUp(self):
        self.scratch = self.enterContext(tempfile.TemporaryDirectory())
        self.root = Path(self.scratch) / "root"
        self.root.mkdir()

    def file(self, name, size, age=0):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x" * size)
        if age:
            os.utime(path, (age, age))
        return path

    @contextlib.contextmanager
    def watch(self, limit, *options):
        with subprocess.Popen(
            [BINARY, "-v", "-s", str(limit), *options, self.root],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        ) as process:
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(5), "watcher did not start")
                    self.assertIn(" recursively", process.stdout.readline())
                yield
                if process.poll() is not None:
                    self.fail(process.stdout.read())
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)

    def until(self, condition):
        deadline = time.monotonic() + 5
        while not condition():
            if time.monotonic() >= deadline:
                self.fail("watcher did not converge")
            time.sleep(0.01)

    def test_recursive_files_leave_directories_and_ignore_symlinks(self):
        old = self.file("one/nested/old", 8, 1)
        new = self.file("two/new", 8, 2)
        outside = Path(self.scratch) / "outside"
        outside.write_bytes(b"x" * 100)
        (self.root / "link").symlink_to(outside)
        with self.watch(8, "--recursive"):
            self.until(lambda: not old.exists())
            self.assertTrue(old.parent.is_dir())
            self.assertTrue(new.is_file())
            self.assertTrue((self.root / "link").is_symlink())
            self.assertEqual(outside.stat().st_size, 100)

    def test_prune_empty_only_after_eviction_and_keep_root(self):
        old = self.file("one/two/old", 8, 1)
        new = self.file("three/new", 8, 2)
        empty = self.root / "already-empty"
        empty.mkdir()
        with self.watch(8, "--prune-empty"):
            self.until(lambda: not (self.root / "one").exists())
            self.assertFalse(old.exists())
            self.assertTrue(new.exists())
            self.assertTrue(empty.is_dir())
            self.assertTrue(self.root.is_dir())

    def test_directory_units_at_depth_two_and_loose_files(self):
        old = self.file("parent/old/a", 3, 1)
        self.file("parent/old/nested/b", 3, 2)
        new = self.file("parent/new/a", 5, 3)
        loose = self.file("parent/loose", 2, 4)
        outside = Path(self.scratch) / "outside"
        outside.mkdir()
        marker = outside / "marker"
        marker.touch()
        (old.parent / "link").symlink_to(outside, target_is_directory=True)
        with self.watch(7, "--evict-depth", "2"):
            self.until(lambda: not old.parent.exists())
            self.assertTrue(new.exists())
            self.assertTrue(loose.exists())
            self.assertTrue(marker.exists())

    def test_live_subtree_moves_and_growth(self):
        with self.watch(10, "-r"):
            data = self.file("a/b/data", 8)
            (self.root / "a").rename(self.root / "renamed")
            (self.root / "renamed").rename(self.root / "latest")
            data = self.root / "latest/b/data"
            os.truncate(data, 12)
            self.until(lambda: not data.exists())

            incoming = Path(self.scratch) / "incoming"
            incoming.mkdir()
            (incoming / "data").write_bytes(b"x" * 8)
            incoming.rename(self.root / "incoming")
            (self.root / "incoming").rename(incoming)
            survivor = self.file("elsewhere/survivor", 8)
            external_file = Path(self.scratch) / "oversized"
            external_file.write_bytes(b"x" * 12)
            os.utime(external_file, (1, 1))
            oversized = self.root / "oversized"
            external_file.rename(oversized)
            self.until(lambda: not oversized.exists())
            self.assertTrue(survivor.exists())
            self.assertTrue((incoming / "data").exists())

            (incoming / "data").write_bytes(b"x" * 12)
            os.utime(incoming / "data", (1, 1))
            incoming.rename(self.root / "incoming")
            self.until(lambda: not (self.root / "incoming/data").exists())

    def test_open_file_protects_moved_directory_unit(self):
        data = self.file("job/sub/data", 0)
        peer = self.file("job/peer", 3)
        with self.watch(5, "--evict-depth", "1"):
            with data.open("r+b", buffering=0) as opened:
                (self.root / "job").rename(self.root / "moved-job")
                opened.write(b"x" * 8)
                disposable = self.file("disposable/file", 5)
                self.until(lambda: not disposable.parent.exists())
                self.assertTrue((self.root / "moved-job/sub/data").is_file())
                self.assertTrue((self.root / "moved-job" / peer.name).is_file())
            self.until(lambda: not (self.root / "moved-job").exists())

    def test_delete_open_allows_eviction_of_directory_unit(self):
        data = self.file("job/data", 1)
        with self.watch(5, "--evict-depth", "1", "--delete-open"):
            with data.open("r+b", buffering=0) as opened:
                opened.write(b"x" * 8)
                self.until(lambda: not data.parent.exists())
                self.assertEqual(os.fstat(opened.fileno()).st_size, 8)

    def test_depth_requires_positive_integer_and_excludes_pruning(self):
        for options in (
            ("--evict-depth", "0"),
            ("--evict-depth", "-1"),
            ("--evict-depth", "2x"),
            ("--evict-depth", "1", "--prune-empty"),
        ):
            with self.subTest(options=options):
                process = subprocess.run(
                    [BINARY, "-s", "1", *options, self.root],
                    capture_output=True,
                    text=True,
                    timeout=5,
                )
                self.assertNotEqual(process.returncode, 0)
                self.assertIn("depth", process.stderr)


if __name__ == "__main__":
    unittest.main()
