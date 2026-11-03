import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
shop = (root / "source/hooks/shop.c").read_text(encoding="utf-8")
jni = (root / "source/jni_fake.c").read_text(encoding="utf-8")
with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as temporary:
    build = Path(temporary)
    (build / "shop_hook.inc").write_text(
        shop[shop.index("static void (*set_iap_state)"):], encoding="utf-8")
    (build / "shop_jni.inc").write_text(
        jni[jni.index("static juint call_boolean("):jni.index("static void *call_object(")],
        encoding="utf-8")
    exe = build / ("shop_test.exe" if os.name == "nt" else "shop_test")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(build), str(root / "tests/shop_test.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
