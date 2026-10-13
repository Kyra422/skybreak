"""Validate fresh-install configuration without any game data (Python 3.11+)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DefaultSettingsTest(unittest.TestCase):
    def test_generated_settings_are_valid_and_english(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / 'settings.toml'
            subprocess.run([
                sys.executable, str(ROOT / 'outputs/release/tools/pkg10_release.py'),
                'make-settings', '--profile', str(ROOT / 'outputs/pc-profile/recomp_config.toml'),
                '--out', str(target), '--nand-root', 'private-test/nand',
                '--screenshot-dir', 'screenshots',
            ], check=True, capture_output=True)
            settings = tomllib.loads(target.read_text(encoding='utf-8'))
            self.assertEqual(settings['ui']['language'], 'en')
            self.assertEqual(settings['nand']['root'], 'private-test/nand')
            self.assertEqual(settings['graphics']['screenshot_dir'], 'screenshots')
            self.assertNotIn('rels', settings)
            self.assertNotIn('rel_aot', settings)


if __name__ == '__main__':
    unittest.main()
