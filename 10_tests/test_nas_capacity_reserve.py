"""Capacity failures must defer delivery without consuming staged data."""
import importlib.util
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


def load(name):
    path = Path(__file__).resolve().parents[1] / "05_tools" / (name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class ReserveTests(unittest.TestCase):
    def test_host_guard_does_not_resume_or_destroy(self):
        m = load("nas_vm_capacity_guard")
        calls = []
        def runner(args, **kwargs):
            calls.append(args)
            if "dumpxml" in args:
                return SimpleNamespace(stdout='<domain><devices><disk><source file="/volume1/vm.qcow2"/></disk></devices></domain>')
            if "domstate" in args:
                return SimpleNamespace(stdout="running\n")
            return SimpleNamespace(stdout="")
        with patch.object(m.os.path, "ismount", return_value=True):
            result = m.guard(Path("/volume1"), "test", 100, runner,
                             lambda p: SimpleNamespace(free=99))
            self.assertEqual(result["action"], "graceful_shutdown_requested")
            self.assertEqual(calls[-1][-2:], ["shutdown", "test"])
            calls.clear()
            result = m.guard(Path("/volume1"), "test", 100, runner,
                             lambda p: SimpleNamespace(free=200))
            self.assertEqual(calls, [])
            self.assertEqual(result["action"], "none")

    def test_video_pauses_and_recovers(self):
        m = load("recording_uploader")
        u = m.Uploader.__new__(m.Uploader)
        u.nas_root = Path("/nas")
        u.nas_min_free_bytes = 100
        u.pause_during_receiver_finalize = False
        with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=99)):
            self.assertFalse(u.nas_capacity_ready())
            with self.assertRaises(OSError) as caught:
                u.should_pause_for_receiver_io("copying")
            self.assertEqual(caught.exception.errno, 28)
        with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=101)):
            self.assertTrue(u.nas_capacity_ready())
            self.assertFalse(u.should_pause_for_receiver_io("copying"))
        with patch.object(m.shutil, "disk_usage", side_effect=OSError("unavailable")):
            self.assertFalse(u.nas_capacity_ready())

    def test_audio_keeps_staged_data(self):
        m = load("audio_archive_receiver")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            staged = root / "segment.staged"
            staged.mkdir()
            (staged / "sentinel").write_bytes(b"original audio")
            u = m.AudioUploader.__new__(m.AudioUploader)
            u.app = SimpleNamespace(nas_root=root, config={"nas_require_mount": False,
                "nas_min_free_mb": 1, "nas_low_space_warning_mb": 0})
            with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=0)):
                with self.assertRaises(OSError):
                    u._publish(staged)
            self.assertEqual((staged / "sentinel").read_bytes(), b"original audio")
            self.assertFalse(u.nas_available)
            with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=2**21)):
                u._nas_ready()
            self.assertTrue(u.nas_available)

    def test_photo_reserve(self):
        m = load("photo_uploader")
        with tempfile.TemporaryDirectory() as tmp:
            u = m.PhotoUploader.__new__(m.PhotoUploader)
            u.nas_root = Path(tmp)
            u.nas_min_free_bytes = 100
            u.require_nas_mount = False
            with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=0)):
                self.assertFalse(u.nas_available())
            with patch.object(m.shutil, "disk_usage", return_value=SimpleNamespace(free=200)):
                self.assertTrue(u.nas_available())


if __name__ == "__main__":
    unittest.main()
