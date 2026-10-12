"""REL22: helpers for the 'SkyBreak still running' tests: build the test double dummy_skybreak.cpp once (MSVC, one file) and
start copies of it at sandbox paths. A copy only sleeps; stop() asks it to exit (a .stop file) and waits - it is never needed to
kill anything. Copies always live under work\ (locked.sh ignores SkyBreak-game.exe processes under work\, so a test double never
looks like the user playing)."""
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'work/install19/unit-build'
DUMMY = OUT / 'dummy-skybreak.exe'
SRC = Path(__file__).resolve().parent / 'dummy_skybreak.cpp'
VSDEV = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat')


def ensure_dummy():
    if DUMMY.is_file() and DUMMY.stat().st_mtime >= SRC.stat().st_mtime:
        return DUMMY
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = (f'call "{VSDEV}" -arch=amd64 -host_arch=amd64 >nul && cl /nologo /O1 /MT /DUNICODE /D_UNICODE "{SRC}" '
           f'/Fo"{OUT}\\\\" /Fe"{DUMMY}" /link /SUBSYSTEM:WINDOWS user32.lib')
    r = subprocess.run(['cmd', '/d', '/s', '/c', cmd], capture_output=True, text=True, errors='replace', timeout=300)
    if r.returncode != 0 or not DUMMY.is_file():
        raise RuntimeError('dummy-skybreak.exe build failed: ' + (r.stdout + r.stderr)[-800:])
    return DUMMY


class Dummy:
    """A running copy of the test double at `path` (an .exe path inside a sandbox under work\)."""

    def __init__(self, path, seconds=60):
        assert 'work' in Path(path).resolve().parts, path
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.backup = None
        if self.path.exists():
            self.backup = self.path.with_name(self.path.name + '.rel22-orig')
            shutil.move(str(self.path), str(self.backup))
        shutil.copy2(ensure_dummy(), self.path)
        stop = Path(str(self.path) + '.stop')
        if stop.exists():
            stop.unlink()
        self.proc = subprocess.Popen([str(self.path), str(seconds)])
        time.sleep(0.3)

    def stop(self):
        stop = Path(str(self.path) + '.stop')
        if self.proc.poll() is None:
            stop.write_text('stop')
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        if stop.exists():
            stop.unlink()
        for _ in range(50):  # the image file stays locked for a moment after exit
            try:
                self.path.unlink()
                break
            except OSError:
                time.sleep(0.1)
        if self.backup is not None and self.backup.exists():
            shutil.move(str(self.backup), str(self.path))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.stop()
