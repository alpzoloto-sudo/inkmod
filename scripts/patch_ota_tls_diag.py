Import('env')

from pathlib import Path

root = Path(env.subst('$PROJECT_DIR'))
hdr = root / 'src' / 'network' / 'OtaUpdater.h'
src = root / 'src' / 'network' / 'OtaUpdater.cpp'
act = root / 'src' / 'activities' / 'settings' / 'OtaUpdateActivity.cpp'

# This script runs after patch_ota_diag_v2.py and extends the stage-2 diagnostics.
h = hdr.read_text(encoding='utf-8')
if 'lastSocketErrno' not in h:
    h = h.replace('  int lastEspError = 0;\n',
                  '  int lastEspError = 0;\n'
                  '  int lastSocketErrno = 0;\n'
                  '  int lastTlsError = 0;\n'
                  '  int lastTlsFlags = 0;\n'
                  '  size_t otaHeapFree = 0;\n'
                  '  size_t otaHeapLargest = 0;\n', 1)
    h = h.replace('  int getLastEspError() const { return lastEspError; }\n',
                  '  int getLastEspError() const { return lastEspError; }\n'
                  '  int getLastSocketErrno() const { return lastSocketErrno; }\n'
                  '  int getLastTlsError() const { return lastTlsError; }\n'
                  '  int getLastTlsFlags() const { return lastTlsFlags; }\n'
                  '  size_t getOtaHeapFree() const { return otaHeapFree; }\n'
                  '  size_t getOtaHeapLargest() const { return otaHeapLargest; }\n', 1)
hdr.write_text(h, encoding='utf-8')

s = src.read_text(encoding='utf-8')
if '#include "esp_heap_caps.h"' not in s:
    s = s.replace('#include "esp_https_ota.h"\n', '#include "esp_https_ota.h"\n#include "esp_heap_caps.h"\n', 1)

# Capture the low-level HTTP/TLS cause while the client handle is still valid.
if 'ota_diag_event_handler' not in s:
    anchor = 'size_t totalBytesReceived = 0;\n\n'
    helper = '''size_t totalBytesReceived = 0;\n\nstruct OtaDiagState {\n  int socketErrno = 0;\n  int tlsError = 0;\n  int tlsFlags = 0;\n};\n\nOtaDiagState otaDiagState;\n\nesp_err_t ota_diag_event_handler(esp_http_client_event_t* event) {\n  if (event == nullptr || event->client == nullptr) return ESP_OK;\n  if (event->event_id == HTTP_EVENT_ERROR || event->event_id == HTTP_EVENT_DISCONNECTED) {\n    otaDiagState.socketErrno = esp_http_client_get_errno(event->client);\n    int tlsError = 0;\n    int tlsFlags = 0;\n    esp_http_client_get_and_clear_last_tls_error(event->client, &tlsError, &tlsFlags);\n    if (tlsError != 0) otaDiagState.tlsError = tlsError;\n    if (tlsFlags != 0) otaDiagState.tlsFlags = tlsFlags;\n  }\n  return ESP_OK;\n}\n\n'''
    if anchor not in s:
        raise RuntimeError('totalBytesReceived anchor not found')
    s = s.replace(anchor, helper, 1)

# Add event callback only to the installUpdate() HTTP config (the one containing otaUrl.c_str()).
install_anchor = '  esp_http_client_config_t client_config = {\n      .url = otaUrl.c_str(),\n'
if install_anchor in s and '.event_handler = ota_diag_event_handler,' not in s[s.find(install_anchor):s.find(install_anchor)+700]:
    s = s.replace(install_anchor,
                  '  esp_http_client_config_t client_config = {\n      .url = otaUrl.c_str(),\n      .event_handler = ota_diag_event_handler,\n', 1)

# Reset diagnostics and snapshot heap before TLS starts.
reset_old = '  processedSize = 0;\n  lastEspError = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;'
reset_new = '''  processedSize = 0;\n  lastEspError = 0;\n  lastSocketErrno = 0;\n  lastTlsError = 0;\n  lastTlsFlags = 0;\n  otaDiagState = {};\n  otaHeapFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);\n  otaHeapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);\n\n  esp_https_ota_handle_t ota_handle = NULL;'''
if reset_old in s:
    s = s.replace(reset_old, reset_new, 1)

# Retry connection setup: 0x7002 is ESP_ERR_HTTP_CONNECT and occurs before any image bytes are read.
begin_old = '''  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);\n  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }'''
begin_new = '''  for (int attempt = 0; attempt < 3; ++attempt) {\n    ota_handle = NULL;\n    otaDiagState = {};\n    otaHeapFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);\n    otaHeapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);\n    esp_err = esp_https_ota_begin(&ota_config, &ota_handle);\n    if (esp_err == ESP_OK) break;\n\n    lastEspError = static_cast<int>(esp_err);\n    lastSocketErrno = otaDiagState.socketErrno;\n    lastTlsError = otaDiagState.tlsError;\n    lastTlsFlags = otaDiagState.tlsFlags;\n    LOG_DBG("OTA", "HTTP OTA Begin attempt %d failed: %s errno=%d tls=0x%X flags=0x%X",\n            attempt + 1, esp_err_to_name(esp_err), lastSocketErrno,\n            static_cast<unsigned int>(lastTlsError), static_cast<unsigned int>(lastTlsFlags));\n    if (attempt < 2) delay(750 * (attempt + 1));\n  }\n  if (esp_err != ESP_OK) {\n    return INTERNAL_UPDATE_ERROR;\n  }'''
if begin_old not in s:
    raise RuntimeError('patched esp_https_ota_begin block not found')
s = s.replace(begin_old, begin_new, 1)

src.write_text(s, encoding='utf-8')

# Add low-level diagnostics to the error page. Keep it compact for 800x480.
a = act.read_text(encoding='utf-8')
if 'TLS=' not in a:
    a = a.replace('    char bytesLine[48];\n',
                  '    char bytesLine[48];\n    char netLine[64];\n    char tlsLine[64];\n    char heapLine[64];\n', 1)
    a = a.replace('    snprintf(bytesLine, sizeof(bytesLine), "DL %lu / %lu", static_cast<unsigned long>(updater.getProcessedSize()), static_cast<unsigned long>(updater.getTotalSize()));\n',
                  '    snprintf(bytesLine, sizeof(bytesLine), "DL %lu / %lu", static_cast<unsigned long>(updater.getProcessedSize()), static_cast<unsigned long>(updater.getTotalSize()));\n'
                  '    snprintf(netLine, sizeof(netLine), "SOCK=%d", updater.getLastSocketErrno());\n'
                  '    snprintf(tlsLine, sizeof(tlsLine), "TLS=0x%X FL=0x%X", static_cast<unsigned int>(updater.getLastTlsError()), static_cast<unsigned int>(updater.getLastTlsFlags()));\n'
                  '    snprintf(heapLine, sizeof(heapLine), "HEAP %lu / %lu", static_cast<unsigned long>(updater.getOtaHeapFree()), static_cast<unsigned long>(updater.getOtaHeapLargest()));\n', 1)
    a = a.replace('    int y = top - (height + metrics.verticalSpacing) * 3;\n',
                  '    int y = top - (height + metrics.verticalSpacing) * 5;\n', 1)
    needle = '''    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);'''
    repl = '''    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, netLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, tlsLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, heapLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);'''
    if needle not in a:
        raise RuntimeError('error-page bytes block not found')
    a = a.replace(needle, repl, 1)
act.write_text(a, encoding='utf-8')

print('[inkMOD] Applied OTA TLS/socket/heap diagnostics + 3 begin retries')
