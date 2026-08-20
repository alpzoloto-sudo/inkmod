from pathlib import Path
from urllib.request import Request, urlopen

Import("env")

ROOT = Path(env.subst("$PROJECT_DIR"))
UPSTREAM = "https://raw.githubusercontent.com/uxjulia/CrossInk/release/v1.5.1/"
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
    # Keep the upstream 1.5.1 implementation while preserving inkMOD branding,
    # release endpoint and build-variant asset naming.
    text = text.replace("CROSSINK_VERSION", "INKMOD_VERSION")
    text = text.replace("CROSSINK_OTA_RELEASE_URL", "INKMOD_OTA_RELEASE_URL")
    text = text.replace("CROSSINK_FIRMWARE_DEVICE_TYPE", "INKMOD_FIRMWARE_VARIANT")
    text = text.replace("CrossInk-ESP32-", "inkMOD-ESP32-")
    text = text.replace("https://api.github.com/repos/uxjulia/CrossInk/releases/latest",
                        "https://api.github.com/repos/alpzoloto-sudo/inkmod/releases/latest")
    text = text.replace('constexpr char DEVICE_ID[] = "crossink-device";',
                        'constexpr char DEVICE_ID[] = "inkmod-device";')

    if path == "src/network/OtaUpdater.cpp":
        # inkMOD's FirmwareFlasher predates CrossInk's chip-id helper. The X3/X4
        # branch is already built for ESP32-C3 only, so keep CrossInk's buffered
        # header path but disable the unavailable helper until the flasher is
        # upgraded separately.
        text = text.replace(
            "const uint16_t runningChipId = firmware_flash::runningPartitionChipId();",
            "const uint16_t runningChipId = 0xFFFF;")
        # Always use the wolfSSL streaming transport for the firmware asset.
        # This is the key CrossInk 1.5.1 behavior that avoids esp_https_ota's
        # ESP_ERR_HTTP_CONNECT failures on the memory-constrained X4.
        text = text.replace(
            "if (hasManifestSha256) downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;",
            "downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;")

    return text


for rel in FILES:
    dst = ROOT / rel
    upstream = adapt(rel, download(rel))
    dst.write_text(upstream, encoding="utf-8")
    print(f"[inkMOD] Ported CrossInk 1.5.1 network file: {rel}")

# Compatibility shims for the older inkMOD credential store. CrossInk 1.5.1's
# client can talk to the normal KOReader server without the optional CrossPoint
# protocol extensions; these methods intentionally disable only those extras.
cred = ROOT / "lib" / "KOReaderSync" / "KOReaderCredentialStore.h"
text = cred.read_text(encoding="utf-8")
if "usesCrossPointSyncServer() const" not in text:
    needle = "  // Document matching method\n"
    shim = (
        "  // CrossInk 1.5.1 client compatibility. inkMOD keeps its existing\\n"
        "  // credential format and disables CrossPoint-only protocol extras.\\n"
        "  bool usesCrossPointSyncServer() const { return false; }\\n"
        "  bool getSendMetadata() const { return false; }\\n\\n"
    ).replace("\\n", "\n")
    if needle not in text:
        raise RuntimeError("KOReaderCredentialStore insertion point not found")
    text = text.replace(needle, shim + needle, 1)
    cred.write_text(text, encoding="utf-8")
    print("[inkMOD] Added KOReader credential compatibility shims")

print("[inkMOD] CrossInk 1.5.1 OTA + KOReader network port applied")
