from pathlib import Path
from urllib.request import Request, urlopen

Import("env")

ROOT = Path(env.subst("$PROJECT_DIR"))
# Pin the OTA/KOReader network port to the exact CrossInk release candidate
# validated for this inkMOD 1.1.4 test branch. Do not follow a moving release
# branch: a rebuild must always compile the same upstream sources.
CROSSINK_TAG = "v1.5.1-rc-3"
UPSTREAM = f"https://raw.githubusercontent.com/uxjulia/CrossInk/{CROSSINK_TAG}/"
FILES = [
    "src/network/HttpDownloader.cpp",
    "src/network/HttpDownloader.h",
    "src/network/OtaUpdater.cpp",
    "src/network/OtaUpdater.h",
    "lib/JsonParser/ReleaseJsonParser.cpp",
    "lib/JsonParser/ReleaseJsonParser.h",
    "lib/KOReaderSync/KOReaderSyncClient.cpp",
    "lib/KOReaderSync/KOReaderSyncClient.h",
]


def download(path: str) -> str:
    req = Request(UPSTREAM + path, headers={"User-Agent": "inkMOD-build/1.1.4"})
    with urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8")


def adapt(path: str, text: str) -> str:
    # Keep the exact CrossInk v1.5.1-rc-3 implementation while preserving
    # inkMOD branding, release endpoint and build-variant asset naming.
    text = text.replace("CROSSINK_VERSION", "INKMOD_VERSION")
    text = text.replace("CROSSINK_OTA_RELEASE_URL", "INKMOD_OTA_RELEASE_URL")
    text = text.replace("CROSSINK_FIRMWARE_DEVICE_TYPE", "INKMOD_FIRMWARE_VARIANT")
    text = text.replace("CrossInk-ESP32-", "inkMOD-ESP32-")
    text = text.replace("https://api.github.com/repos/uxjulia/CrossInk/releases/latest",
                        "https://api.github.com/repos/alpzoloto-sudo/inkmod/releases/latest")
    text = text.replace('constexpr char DEVICE_ID[] = "crossink-device";',
                        'constexpr char DEVICE_ID[] = "inkmod-device";')

    # CrossInk's AppVersion.h only provides its build-time version macros.
    # inkMOD already provides the adapted macros through platformio.ini, and
    # this repository does not have AppVersion.h. Keeping the include would
    # make the exact rc3 sources fail before the OTA code is compiled.
    text = text.replace('#include "AppVersion.h"\n', '')

    if path == "src/network/OtaUpdater.cpp":
        # Keep the visible firmware version at 1.1.4 while allowing the OTA test
        # harness to use a private fourth numeric segment (1.1.4.0 -> 1.1.4.1).
        # Production builds that do not define INKMOD_OTA_VERSION fall back to
        # the normal visible INKMOD_VERSION.
        text = text.replace("INKMOD_VERSION", "INKMOD_OTA_VERSION")
        marker = "#include \"network/WifiPowerSaveGuard.h\"\n"
        ota_version_fallback = (
            marker
            + "\n#ifndef INKMOD_OTA_VERSION\n"
            + "#define INKMOD_OTA_VERSION INKMOD_VERSION\n"
            + "#endif\n"
        )
        if marker not in text:
            raise RuntimeError("OtaUpdater include insertion point not found")
        text = text.replace(marker, ota_version_fallback, 1)

        # inkMOD's FirmwareFlasher predates CrossInk's chip-id helper. The X3/X4
        # branch is ESP32-C3 only, so preserve rc3's image-header validation but
        # disable only the unavailable running-partition helper when present.
        text = text.replace(
            "const uint16_t runningChipId = firmware_flash::runningPartitionChipId();",
            "const uint16_t runningChipId = 0xFFFF;")

        # Do NOT force WolfSSL. CrossInk rc3 intentionally selects WolfSSL only
        # when the trusted release manifest provides a SHA-256 digest; otherwise
        # it keeps the verified esp_http_client HTTPS path.

    return text


for rel in FILES:
    dst = ROOT / rel
    upstream = adapt(rel, download(rel))
    dst.write_text(upstream, encoding="utf-8")
    print(f"[inkMOD] Ported CrossInk {CROSSINK_TAG} network file: {rel}")

# Compatibility shims for the older inkMOD credential store. CrossInk rc3's
# client can talk to the normal KOReader server without the optional CrossPoint
# protocol extensions; these methods intentionally disable only those extras.
cred = ROOT / "lib" / "KOReaderSync" / "KOReaderCredentialStore.h"
text = cred.read_text(encoding="utf-8")
if "usesCrossPointSyncServer() const" not in text:
    needle = "  // Document matching method\n"
    shim = (
        "  // CrossInk v1.5.1-rc-3 client compatibility. inkMOD keeps its existing\\n"
        "  // credential format and disables CrossPoint-only protocol extras.\\n"
        "  bool usesCrossPointSyncServer() const { return false; }\\n"
        "  bool getSendMetadata() const { return false; }\\n\\n"
    ).replace("\\n", "\n")
    if needle not in text:
        raise RuntimeError("KOReaderCredentialStore insertion point not found")
    text = text.replace(needle, shim + needle, 1)
    cred.write_text(text, encoding="utf-8")
    print("[inkMOD] Added KOReader credential compatibility shims")

print(f"[inkMOD] CrossInk {CROSSINK_TAG} OTA + KOReader network port applied")
