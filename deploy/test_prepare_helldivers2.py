import configparser
from pathlib import Path
import tempfile
import unittest

from prepare_helldivers2 import configure_ini, stage


class PrepareTests(unittest.TestCase):
    def parse(self, text):
        ini = configparser.ConfigParser()
        ini.read_string(text)
        return ini

    def test_client_spacing_preserves_provider_and_nr_tuning(self):
        source = ("; keep this comment\n[FrameGen]\nEnabled = true\n"
                  "FGNvngxReplacement = Arturs\n[DLSSG]\nInterpolationCount = 3\n"
                  "[DlssNr]\nEnabled = false\nCacheInterval = 8\n"
                  "[Menu]\nShortcutKey = -1\n[Log]\nLogLevel = 0\n")
        result = configure_ini(source)
        ini = self.parse(result)
        self.assertIn("; keep this comment", result)
        self.assertEqual(ini["FrameGen"]["FGNvngxReplacement"], "Arturs")
        self.assertEqual(ini["DLSSG"]["InterpolationCount"], "3")
        self.assertEqual(ini["DlssNr"]["CacheInterval"], "8")
        self.assertEqual(ini["DlssNr"]["Enabled"], "true")
        self.assertEqual(ini["Menu"]["ShortcutKey"], "0x2D")
        self.assertEqual(ini["Menu"]["OverlayMenu"], "true")
        self.assertEqual(ini["Log"]["LogLevel"], "2")
        self.assertEqual(configure_ini(result), result)

    def test_missing_sections_and_case_variations(self):
        result = configure_ini("\ufeff[menu]\nshortcutkey=0x24\n")
        ini = self.parse(result)
        self.assertEqual(ini["menu"]["shortcutkey"], "0x2D")
        self.assertEqual(ini["DlssNr"]["Enabled"], "true")
        self.assertEqual(result.lower().count("[menu]"), 1)

    def test_stage_never_changes_game_and_does_not_bundle_model(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            game, build, output = base / "game", base / "build", base / "output"
            game.mkdir()
            build.mkdir()
            sl = game / "OptiScaler/streamline"
            sl.mkdir(parents=True)
            for name in ("sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll"):
                (sl / name).write_bytes(b"existing dependency")
            for name in ("helldivers2.exe", "dxgi.dll", "nvngx_dlssnr.dll"):
                (game / name).write_bytes(b"existing game file")
            (game / "OptiScaler.ini").write_text("[FrameGen]\nFGNvngxReplacement=Arturs\n")
            for name in ("OptiScaler.dll", "nvngx.dll_dlssnr.dll"):
                (build / name).write_bytes(b"test build")
            before = {p.relative_to(game): p.read_bytes() for p in game.rglob("*") if p.is_file()}
            stage(game, build, output)
            after = {p.relative_to(game): p.read_bytes() for p in game.rglob("*") if p.is_file()}
            self.assertEqual(before, after)
            self.assertEqual((output / "dxgi.dll").read_bytes(), b"test build")
            self.assertFalse((output / "nvngx_dlssnr.dll").exists())
            with self.assertRaises(ValueError):
                stage(game, build, output)
            with self.assertRaises(ValueError):
                stage(game, build, game / "update")

    def test_missing_dependency_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            output = base / "output"
            with self.assertRaises(FileNotFoundError):
                stage(base / "game", base / "build", output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
