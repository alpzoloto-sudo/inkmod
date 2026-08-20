Import('env')

from pathlib import Path

root = Path(env.subst('$PROJECT_DIR'))
hdr = root / 'src' / 'network' / 'OtaUpdater.h'
src = root / 'src' / 'network' / 'OtaUpdater.cpp'
act = root / 'src' / 'activities' / 'settings' / 'OtaUpdateActivity.cpp'

# Header: persist raw ESP-IDF error so the e-ink error screen can show the real cause.
h = hdr.read_text(encoding='utf-8')
needle = '  size_t totalSize = 0;\n'
if 'lastEspError' not in h:
    h = h.replace(needle, needle + '  int lastEspError = 0;\n', 1)
    h = h.replace('  size_t getTotalSize() const { return totalSize; }\n',
                  '  size_t getTotalSize() const { return totalSize; }\n\n  int getLastEspError() const { return lastEspError; }\n', 1)
hdr.write_text(h, encoding='utf-8')

s = src.read_text(encoding='utf-8')
# Make OTA less sensitive to slow GitHub/CDN transfers.
s = s.replace('.timeout_ms = 15000,', '.timeout_ms = 45000,')
s = s.replace('    delay(100);  // TODO: should we replace this with something better?',
              '    delay(25);  // Keep OTA pump responsive on slow GitHub/CDN transfers')

# Do NOT inject esp_http_client redirect designators here: in this ESP-IDF version
# designated initializers must follow the struct declaration order and the previous
# diagnostic patch broke compilation. First collect the exact esp_err_t on-device.

s = s.replace('  processedSize = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;',
              '  processedSize = 0;\n  lastEspError = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;', 1)

# Capture failures at begin/perform/finish.
s = s.replace('  if (esp_err != ESP_OK) {\n    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }',
              '  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }', 1)
s = s.replace('  if (esp_err != ESP_OK) {\n    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));\n    esp_https_ota_finish(ota_handle);\n    return HTTP_ERROR;\n  }',
              '  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));\n    esp_https_ota_finish(ota_handle);\n    return HTTP_ERROR;\n  }', 1)
s = s.replace('  esp_err = esp_https_ota_finish(ota_handle);\n  if (esp_err != ESP_OK) {\n    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }',
              '  esp_err = esp_https_ota_finish(ota_handle);\n  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }', 1)
src.write_text(s, encoding='utf-8')

# Error UI: add the raw esp_err_t and received byte count under OTA ERR=N.
a = act.read_text(encoding='utf-8')
if 'ESP RAW=' not in a:
    a = a.replace('    char errLine[40];\n    char runLine[64];',
                  '    char errLine[40];\n    char rawErrLine[40];\n    char bytesLine[48];\n    char runLine[64];', 1)
    a = a.replace('    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n',
                  '    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n'
                  '    snprintf(rawErrLine, sizeof(rawErrLine), "ESP RAW=0x%X", static_cast<unsigned int>(updater.getLastEspError()));\n'
                  '    snprintf(bytesLine, sizeof(bytesLine), "DL %lu / %lu", static_cast<unsigned long>(updater.getProcessedSize()), static_cast<unsigned long>(updater.getTotalSize()));\n', 1)
    a = a.replace('    int y = top - (height + metrics.verticalSpacing) * 2;\n',
                  '    int y = top - (height + metrics.verticalSpacing) * 3;\n', 1)
    a = a.replace('    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);',
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, rawErrLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);', 1)
act.write_text(a, encoding='utf-8')

print('[inkMOD] Applied OTA v2 timeout + raw error/byte diagnostics patch')
