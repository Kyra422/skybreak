"""Exercise production PowerShell tool checks without building or using a disc."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'outputs/release/Bouw-SkywardSword.ps1').read_text(encoding='utf-8-sig')


def function(name):
    return re.search(r'(?ms)^function ' + re.escape(name) + r'\b.*?^\}', SOURCE).group(0)


@unittest.skipUnless(os.name == 'nt', 'Windows process launch regression')
class BuildToolTest(unittest.TestCase):
    def run_ps(self, body):
        with tempfile.TemporaryDirectory() as temp:
            script = Path(temp) / 'checks.ps1'
            functions = '\n'.join(function(n) for n in
                                  ('Volledig-Pad', 'Quote-Arg', 'Log-Staart', 'Zoek-Cmd', 'Start-Direct', 'Start-Gelogd', 'Zoek-Exe'))
            script.write_text("$ErrorActionPreference='Stop'\nSet-StrictMode -Version 2\n"
                              "function Fout([string]$Melding) { throw $Melding }\n"
                              "function Voortgang {}\n$script:HuidigeNaam='1. Checking tools'\n"
                              + functions + '\n' + body, encoding='utf-8-sig')
            return subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                   '-File', str(script)], env=dict(os.environ, SB_TEST_PYTHON=sys.executable,
                                                                  SB_TEST_ROOT=str(ROOT), SB_TEST_TEMP=temp),
                                  capture_output=True, text=True, errors='replace', timeout=90)

    def test_missing_comspec_runs_command_processor(self):
        r = self.run_ps("""
Remove-Item Env:ComSpec -ErrorAction SilentlyContinue
$r = Start-Direct (Zoek-Cmd) @('/d','/c','echo','SHELL_OK')
if ($r.Code -ne 0 -or $r.Uit.Trim() -ne 'SHELL_OK') { throw 'Command processor failed' }
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_stale_comspec_falls_back_for_logged_process(self):
        r = self.run_ps("""
$env:ComSpec = Join-Path $env:SB_TEST_TEMP 'missing-cmd.exe'
$log = Join-Path $env:SB_TEST_TEMP 'tool.log'
$rc = Start-Gelogd $env:SB_TEST_PYTHON @('-c','print(12345)') $log 'Test tool'
if ($rc -ne 0 -or (Get-Content -Raw $log) -notmatch '12345') { throw 'Logged process failed' }
""")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_empty_program_has_actionable_error(self):
        r = self.run_ps("Start-Direct '' @('--version')")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('No program was selected', r.stderr)
        self.assertIn('Checking tools', r.stderr)
        self.assertNotIn('Cannot start process because a file name', r.stderr)

    def test_empty_logged_program_has_actionable_error(self):
        r = self.run_ps("Start-Gelogd '' @() 'unused.log' 'Configuring game'")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("No program was selected for 'Configuring game'", r.stderr)

    def test_nonexistent_program_identifies_tool(self):
        r = self.run_ps("Start-Direct (Join-Path $env:SB_TEST_TEMP 'missing-python.exe') @()")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Could not start', r.stderr)
        self.assertIn('missing-python.exe', r.stderr)
        self.assertIn('Checking/running:', r.stdout)

    def test_full_tool_step_without_comspec(self):
        start = SOURCE.index("    Stap '1. Checking tools' {")
        end = SOURCE.index('    # ---------------------------------------------------------------------------- 2.', start)
        # Actual prerequisite step: real Python, MSVC, CMake and Ninja, no compilation.
        r = self.run_ps("""
Remove-Item Env:ComSpec -ErrorAction SilentlyContinue
$projectRoot = $env:SB_TEST_ROOT
$Python = $env:SB_TEST_PYTHON
$CMake = ''; $Ninja = ''; $Bronnen = ''; $Taken = 1; $isoIsMap = $true
function Stap([string]$Naam, [scriptblock]$Werk) { & $Werk }
""" + SOURCE[start:end] + "\nif (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'MSVC missing' }\n")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('CMake', r.stdout)
        self.assertIn('Ninja', r.stdout)


if __name__ == '__main__':
    unittest.main()
