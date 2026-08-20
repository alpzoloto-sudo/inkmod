Import('env')

from pathlib import Path

root = Path(env.subst('$PROJECT_DIR'))

# --- OtaUpdater.h: expose the raw ESP-IDF error and OTA stage to the UI. ---
hdr = root / 'src' / 'network' / 'OtaUpdater.h'
h = hdr.read_text(encoding='utf-8')
if '#include <cstdint>' not in h:
    h = h.replace('#include <atomic>\n', '#include <atomic>\n#include <cstdint>\n', 1)

field_marker = '  size_t totalSize = 0;\n'
field_patch = (
    '  size_t totalSize = 0;\n'
    '  int32_t lastEspError = 0;\n'
    '  uint8_t lastOtaStage = 0;  // 1=begin, 2=perform, 3=incomplete, 4=finish\n'
)
if 'lastEspError' not in h:
    if field_marker not in h:
        raise RuntimeError('OtaUpdater.h field marker not found')
    h = h.replace(field_marker, field_patch, 1)

getter_marker = '  size_t getTotalSize() const { return totalSize; }\n'
getter_patch = (
    '  size_t getTotalSize() const { return totalSize; }\n\n'
    '  int32_t getLastEspError() const { return lastEspError; }\n\n'
    '  uint8_t getLastOtaStage() const { return lastOtaStage; }\n'
)
if 'getLastEspError()' not in h:
    if getter_marker not in h:
        raise RuntimeError('OtaUpdater.h getter marker not found')
    h = h.replace(getter_marker, getter_patch, 1)
hdr.write_text(h, encoding='utf-8')

# --- OtaUpdater.cpp: record exactly where the asset download/OTA write fails. ---
cpp = root / 'src' / 'network' / 'OtaUpdater.cpp'
c = cpp.read_text(encoding='utf-8')

reset_marker = '  processedSize = 0;\n\n  esp_https_ota_handle_t ota_handle = NULL;'
reset_patch = (
    '  processedSize = 0;\n'
    '  lastEspError = 0;\n'
    '  lastOtaStage = 0;\n\n'
    '  esp_https_ota_handle_t ota_handle = NULL;'
)
if 'lastOtaStage = 0;' not in c:
    if reset_marker not in c:
        raise RuntimeError('OTA diagnostic reset marker not found')
    c = c.replace(reset_marker, reset_patch, 1)

begin_marker = '  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);\n  if (esp_err != ESP_OK) {'
begin_patch = (
    '  lastOtaStage = 1;\n'
    '  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);\n'
    '  if (esp_err != ESP_OK) {\n'
    '    lastEspError = static_cast<int32_t>(esp_err);'
)
if 'lastOtaStage = 1;' not in c:
    if begin_marker not in c:
        raise RuntimeError('OTA begin marker not found')
    c = c.replace(begin_marker, begin_patch, 1)

perform_marker = '    esp_err = esp_https_ota_perform(ota_handle);\n    processedSize = esp_https_ota_get_image_len_read(ota_handle);'
perform_patch = (
    '    lastOtaStage = 2;\n'
    '    esp_err = esp_https_ota_perform(ota_handle);\n'
    '    processedSize = esp_https_ota_get_image_len_read(ota_handle);\n'
    '    if (esp_err != ESP_OK && esp_err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {\n'
    '      lastEspError = static_cast<int32_t>(esp_err);\n'
    '    }'
)
if 'lastOtaStage = 2;' not in c:
    if perform_marker not in c:
        raise RuntimeError('OTA perform marker not found')
    c = c.replace(perform_marker, perform_patch, 1)

incomplete_marker = '  if (!esp_https_ota_is_complete_data_received(ota_handle)) {\n    LOG_ERR('
incomplete_patch = (
    '  if (!esp_https_ota_is_complete_data_received(ota_handle)) {\n'
    '    lastOtaStage = 3;\n'
    '    lastEspError = 0;\n'
    '    LOG_ERR('
)
if 'lastOtaStage = 3;' not in c:
    if incomplete_marker not in c:
        raise RuntimeError('OTA incomplete marker not found')
    c = c.replace(incomplete_marker, incomplete_patch, 1)

finish_marker = '  esp_err = esp_https_ota_finish(ota_handle);\n  if (esp_err != ESP_OK) {'
finish_patch = (
    '  lastOtaStage = 4;\n'
    '  esp_err = esp_https_ota_finish(ota_handle);\n'
    '  if (esp_err != ESP_OK) {\n'
    '    lastEspError = static_cast<int32_t>(esp_err);'
)
if 'lastOtaStage = 4;' not in c:
    if finish_marker not in c:
        raise RuntimeError('OTA finish marker not found')
    c = c.replace(finish_marker, finish_patch, 1)

success_marker = '  LOG_INF("OTA", "Update completed");\n  return OK;'
success_patch = '  lastOtaStage = 0;\n  lastEspError = 0;\n  LOG_INF("OTA", "Update completed");\n  return OK;'
if success_marker in c and success_patch not in c:
    c = c.replace(success_marker, success_patch, 1)

cpp.write_text(c, encoding='utf-8')

# --- OtaUpdateActivity.cpp: make the failure screen useful without serial. ---
act = root / 'src' / 'activities' / 'settings' / 'OtaUpdateActivity.cpp'
a = act.read_text(encoding='utf-8')

char_marker = '    char errLine[40];\n    char runLine[64];'
char_patch = (
    '    char errLine[40];\n'
    '    char diagLine[64];\n'
    '    char bytesLine[64];\n'
    '    char runLine[64];'
)
if 'char diagLine[64];' not in a:
    if char_marker not in a:
        raise RuntimeError('OTA activity char marker not found')
    a = a.replace(char_marker, char_patch, 1)

snprintf_marker = '    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n'
snprintf_patch = (
    '    snprintf(errLine, sizeof(errLine), "OTA ERR=%d", lastErrorCode);\n'
    '    snprintf(diagLine, sizeof(diagLine), "STAGE=%u ESP=0x%08lX",\n'
    '             static_cast<unsigned>(updater.getLastOtaStage()),\n'
    '             static_cast<unsigned long>(static_cast<uint32_t>(updater.getLastEspError())));\n'
    '    snprintf(bytesLine, sizeof(bytesLine), "DL %lu / %lu",\n'
    '             static_cast<unsigned long>(updater.getProcessedSize()),\n'
    '             static_cast<unsigned long>(updater.getTotalSize()));\n'
)
if 'STAGE=%u ESP=0x%08lX' not in a:
    if snprintf_marker not in a:
        raise RuntimeError('OTA activity snprintf marker not found')
    a = a.replace(snprintf_marker, snprintf_patch, 1)

render_marker = (
    '    int y = top - (height + metrics.verticalSpacing) * 2;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);'
)
render_patch = (
    '    int y = top - (height + metrics.verticalSpacing) * 3;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, errLine);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, diagLine);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, bytesLine);\n'
    '    y += height + metrics.verticalSpacing;\n'
    '    renderer.drawCenteredText(UI_10_FONT_ID, y, runLine);'
)
if 'renderer.drawCenteredText(UI_10_FONT_ID, y, diagLine);' not in a:
    if render_marker not in a:
        raise RuntimeError('OTA activity render marker not found')
    a = a.replace(render_marker, render_patch, 1)

act.write_text(a, encoding='utf-8')
print('[inkMOD] Applied OTA stage/raw-error diagnostics')
