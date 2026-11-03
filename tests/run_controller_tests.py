import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "source/hooks/game.c").read_text(encoding="utf-8")
start = source.index("static void install_controller_hook(")
end = source.index("\nvoid patch_game(", start)
# Windows may briefly keep the executed test binary open after process exit.
with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as temporary:
    build = Path(temporary)
    (build / "controller_installer.inc").write_text(source[start:end],encoding="utf-8")
    exe = build / ("controller_test.exe" if os.name == "nt" else "controller_test")
    subprocess.run([os.environ.get("CC","gcc"),"-std=c11","-Wall","-Wextra","-Werror",
        "-I"+str(root / "tests/stubs"),"-I"+str(root / "source"),"-I"+str(build),
        str(root / "tests/controller_test.c"),str(root / "source/controller.c"),
        "-lm","-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
