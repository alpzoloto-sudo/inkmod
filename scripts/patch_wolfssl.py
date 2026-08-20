from pathlib import Path

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* inkMOD wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
"""

for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    original = settings.read_text()
    text = original
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    patched = text + OVERRIDES + "\n"
    if patched != original:
        settings.write_text(patched)
        print(f"[inkMOD] Patched wolfSSL settings: {settings.relative_to(PROJECT_DIR)}")
