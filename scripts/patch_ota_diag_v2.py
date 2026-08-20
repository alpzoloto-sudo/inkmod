Import('env')

from pathlib import Path

root = Path(env.subst('$PROJECT_DIR'))
hdr = root / 'src' / 'network' / 'OtaUpdater.h'
src = root / 'src' / 'network' / 'OtaUpdater.cpp'
act = root / 'src' / 'activities' / 'settings' / 'OtaUpdateActivity.cpp'

# Header: persist raw ESP-IDF error and the stage where it happened.
h = hdr.read_text(encoding='utf-8')
needle = '  size_t totalSize = 0;\n'
if 'lastEspError' not in h:
    h = h.replace(needle, needle + '  int lastEspError = 0;\n  int lastOtaStage = 0;\n', 1)
    h = h.replace('  size_t getTotalSize() const { return totalSize; }\n',
                  '  size_t getTotalSize() const { return totalSize; }\n\n  int getLastEspError() const { return lastEspError; }\n  int getLastOtaStage() const { return lastOtaStage; }\n', 1)
else:
    if 'lastOtaStage' not in h:
        h = h.replace('  int lastEspError = 0;\n', '  int lastEspError = 0;\n  int lastOtaStage = 0;\n', 1)
        h = h.replace('  int getLastEspError() const { return lastEspError; }\n',
                      '  int getLastEspError() const { return lastEspError; }\n  int getLastOtaStage() const { return lastOtaStage; }\n', 1)
hdr.write_text(h, encoding='utf-8')

s = src.read_text(encoding='utf-8')

# Make OTA less sensitive to slow GitHub/CDN transfers.
s = s.replace('.timeout_ms = 15000,', '.timeout_ms = 45000,')
s = s.replace('    delay(100);  // TODO: should we replace this with something better?',
              '    delay(25);  // Keep OTA pump responsive on slow GitHub/CDN transfers')

# Reset diagnostics at the beginning of update check.
s = s.replace('  updateAvailable = false;\n  latestVersion.clear();',
              '  updateAvailable = false;\n  lastEspError = 0;\n  lastOtaStage = 1;  // CHECK\n  latestVersion.clear();', 1)

# Capture the raw failure from the release API request too. The last test showed
# OTA ERR=2 with RAW=0 and DL 0/0, which means the HTTP_ERROR can happen during
# checkForUpdate(), before installUpdate() ever starts.
check_fail = '''  esp_err = esp_http_client_perform(client_handle);\n  if (esp_err != ESP_OK) {\n    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));\n    esp_http_client_cleanup(client_handle);\n    return HTTP_ERROR;\n  }'''
check_fail_new = '''  esp_err = esp_http_client_perform(client_handle);\n  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 1;  // CHECK\n    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));\n    esp_http_client_cleanup(client_handle);\n    return HTTP_ERROR;\n  }'''
if check_fail in s:
    s = s.replace(check_fail, check_fail_new, 1)

# Install diagnostics.
s = s.replace('  processedSize = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;',
              '  processedSize = 0;\n  lastEspError = 0;\n  lastOtaStage = 2;  // BEGIN\n\n  esp_https_ota_handle_t ota_handle = NULL;', 1)
s = s.replace('  processedSize = 0;\n  lastEspError = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;',
              '  processedSize = 0;\n  lastEspError = 0;\n  lastOtaStage = 2;  // BEGIN\n\n  esp_https_ota_handle_t ota_handle = NULL;', 1)

begin_fail = '''  if (esp_err != ESP_OK) {\n    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }'''
begin_fail_new = '''  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 2;  // BEGIN\n    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }'''
if begin_fail in s:
    s = s.replace(begin_fail, begin_fail_new, 1)

# Existing injected version may already capture the raw error; add stage markers.
s = s.replace('    lastEspError = static_cast<int>(esp_err);\n    LOG_DBG("OTA", "HTTP OTA Begin Failed:',
              '    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 2;  // BEGIN\n    LOG_DBG("OTA", "HTTP OTA Begin Failed:', 1)
s = s.replace('    lastEspError = static_cast<int>(esp_err);\n    LOG_ERR("OTA", "esp_https_ota_perform Failed:',
              '    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 3;  // PERFORM\n    LOG_ERR("OTA", "esp_https_ota_perform Failed:', 1)
s = s.replace('    lastEspError = static_cast<int>(esp_err);\n    LOG_ERR("OTA", "esp_https_ota_finish Failed:',
              '    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 4;  // FINISH\n    LOG_ERR("OTA", "esp_https_ota_finish Failed:', 1)

perform_fail = '''  if (esp_err != ESP_OK) {\n    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));\n    esp_https_ota_finish(ota_handle);\n    return HTTP_ERROR;\n  }'''
perform_fail_new = '''  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 3;  // PERFORM\n    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));\n    esp_https_ota_finish(ota_handle);\n    return HTTP_ERROR;\n  }'''
if perform_fail in s:
    s = s.replace(perform_fail, perform_fail_new, 1)

finish_fail = '''  esp_err = esp_https_ota_finish(ota_handle);\n  if (esp_err != ESP_OK) {\n    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }'''
finish_fail_new = '''  esp_err = esp_https_ota_finish(ota_handle);\n  if (esp_err != ESP_OK) {\n    lastEspError = static_cast<int>(esp_err);\n    lastOtaStage = 4;  // FINISH\n    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));\n    return INTERNAL_UPDATE_ERROR;\n  }'''
if finish_fail in s:
    s = s.replace(finish_fail, finish_fail_new, 1)

src.write_text(s, encoding='utf-8')

# Error UI: show stage, raw esp_err_t and byte counters.
a = act.read_text(encoding='utf-8')
if 'ESP RAW=' not in a:
    a = a.replace('    char errLine[40];\n    char runLine[64];',
                  '    char errLine[40];\n    char stageLine[40];\n    char rawErrLine[40];\n    char bytesLine[48];\n    char runLine[64];', 1)
    a = a.replace('    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n',
                  '    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n'
                  '    snprintf(stageLine, sizeof(stageLine), "STAGE=%d", updater.getLastOtaStage());\n'
                  '    snprintf(rawErrLine, sizeof(rawErrLine), "ESP RAW=0x%X", static_cast<unsigned int>(updater.getLastEspError()));\n'
                  '    snprintf(bytesLine, sizeof(bytesLine), "DL %lu / %lu", static_cast<unsigned long>(updater.getProcessedSize()), static_cast<unsigned long>(updater.getTotalSize()));\n', 1)
    a = a.replace('    int y = top - (height + metrics.verticalSpacing) * 2;\n',
                  '    int y = top - (height + metrics.verticalSpacing) * 4;\n', 1)
    a = a.replace('    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);',
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, stageLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, rawErrLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesLine);\n'
                  '    y += height + metrics.verticalSpacing;\n'
                  '    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);', 1)
else:
    # Upgrade v2 UI to v3 without duplicating existing lines.
    if 'STAGE=' not in a:
        a = a.replace('    char rawErrLine[40];\n', '    char stageLine[40];\n    char rawErrLine[40];\n', 1)
        a = a.replace('    snprintf(rawErrLine, sizeof(rawErrLine), "ESP RAW=0x%X", static_cast<unsigned int>(updater.getLastEspError()));\n',
                      '    snprintf(stageLine, sizeof(stageLine), "STAGE=%d", updater.getLastOtaStage());\n'
                      '    snprintf(rawErrLine, sizeof(rawErrLine), "ESP RAW=0x%X", static_cast<unsigned int>(updater.getLastEspError()));\n', 1)
        a = a.replace('    int y = top - (height + metrics.verticalSpacing) * 3;\n',
                      '    int y = top - (height + metrics.verticalSpacing) * 4;\n', 1)
        a = a.replace('    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n    y += height + metrics.verticalSpacing;\n    renderer.drawCenteredText(UI_10_FONT_ID, y, rawErrLine);',
                      '    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n'
                      '    y += height + metrics.verticalSpacing;\n'
                      '    renderer.drawCenteredText(UI_10_FONT_ID, y, stageLine);\n'
                      '    y += height + metrics.verticalSpacing;\n'
                      '    renderer.drawCenteredText(UI_10_FONT_ID, y, rawErrLine);', 1)
act.write_text(a, encoding='utf-8')

print('[inkMOD] Applied OTA v3 stage + raw error/byte diagnostics patch')
