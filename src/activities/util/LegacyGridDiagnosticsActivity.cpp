#include "LegacyGridDiagnosticsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "InkMODSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t POINTS_PER_CLEAR[5] = {0, 100, 300, 500, 800};
constexpr uint32_t SAVE_MAGIC = 0x54545253;
constexpr uint8_t SAVE_VERSION = 1;
constexpr const char* SAVE_DIR = "/.inkmod";
constexpr const char* SAVE_PATH = "/.inkmod/.legacy_grid.bin";
}  // namespace

const LegacyGridDiagnosticsActivity::CellOffset LegacyGridDiagnosticsActivity::TETROMINOES[PIECE_COUNT][4][4] = {
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}, {{1, 0}, {1, 1}, {0, 1}, {0, 2}},
     {{0, 0}, {1, 0}, {1, 1}, {2, 1}}, {{1, 0}, {1, 1}, {0, 1}, {0, 2}}},
    {{{0, 1}, {1, 0}, {1, 1}, {2, 0}}, {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
     {{0, 1}, {1, 0}, {1, 1}, {2, 0}}, {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
    {{{0, 0}, {0, 1}, {0, 2}, {1, 1}}, {{0, 0}, {0, 1}, {1, 1}, {-1, 1}},
     {{1, 0}, {1, 1}, {0, 1}, {1, 2}}, {{0, 1}, {1, 1}, {-1, 1}, {0, 2}}},
    {{{0, 0}, {1, 0}, {2, 0}, {2, 1}}, {{0, 1}, {1, -1}, {1, 0}, {1, 1}},
     {{0, 0}, {0, 1}, {1, 1}, {2, 1}}, {{1, 0}, {1, 1}, {1, 2}, {2, 0}}},
    {{{0, 1}, {1, 1}, {2, 0}, {2, 1}}, {{0, 0}, {0, 1}, {0, 2}, {1, 2}},
     {{0, 0}, {0, 1}, {1, 0}, {2, 0}}, {{0, 0}, {1, 0}, {1, 1}, {1, 2}}},
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
     {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}},
    {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}, {{0, 0}, {0, 1}, {0, 2}, {0, 3}},
     {{0, 0}, {1, 0}, {2, 0}, {3, 0}}, {{0, 0}, {0, 1}, {0, 2}, {0, 3}}},
};


namespace {
constexpr const char* kPlayPhrases[] = {
  "Отложи книгу. Ненадолго.",
  "Ещё одну линию. Ты можешь.",
  "Не дай бренному миру победить.",
  "Спутник, фигура уже летит.",
  "Книгу дочитаешь. Сначала выживи.",
  "Хорошо идёшь. Не расслабляйся.",
  "Соберись. Поле всё помнит.",
  "Почти красиво. Продолжай.",
  "Ещё чуть-чуть — и можно обратно читать.",
  "Вот теперь похоже на отдых.",
  "Не спеши. Пустоты ошибок не прощают.",
  "Одна хорошая линия меняет всё.",
  "Держи поле чистым. Мир и так захламлён.",
  "Спокойно. Следующая фигура уже решила твою судьбу.",
  "Рекорд сам себя не поставит.",
  "Читай потом. Сейчас строй."
};
constexpr const char* BEST_PATH = "/.inkmod/.legacy_best.bin";
}

void LegacyGridDiagnosticsActivity::nextPhrase() {
  phraseIndex = (phraseIndex + 1) % (sizeof(kPlayPhrases) / sizeof(kPlayPhrases[0]));
}
const char* LegacyGridDiagnosticsActivity::currentPhrase() const {
  return kPlayPhrases[phraseIndex % (sizeof(kPlayPhrases) / sizeof(kPlayPhrases[0]))];
}

void LegacyGridDiagnosticsActivity::loadBestScore() {
  FsFile f;
  if (Storage.openFileForRead("LGC", BEST_PATH, f)) {
    uint32_t v = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&v), sizeof(v)) == static_cast<int>(sizeof(v))) highScore = v;
    f.close();
  }
}
void LegacyGridDiagnosticsActivity::saveBestScore() const {
  Storage.mkdir("/.inkmod");
  FsFile f;
  if (Storage.openFileForWrite("LGC", BEST_PATH, f)) {
    f.write(reinterpret_cast<const uint8_t*>(&highScore), sizeof(highScore));
    f.close();
  }
}

void LegacyGridDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  chillModeEnabled = false;
  loadBestScore();
  std::srand(static_cast<unsigned>(esp_random()));
  if (!loadSavedGame()) {
    startNewGame(false);
  }
  requestUpdate();
}

void LegacyGridDiagnosticsActivity::onExit() {
  if (gameOver) {
    clearSavedGame();
  } else {
    saveGame();
  }
  // Leave the e-ink panel clean after the game; otherwise the board remains
  // visible as heavy ghosting behind the reader UI.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  Activity::onExit();
}

void LegacyGridDiagnosticsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= FULL_REFRESH_HOLD_MS) {
    // This branch has no requestNextFullRefresh() API. Refresh immediately.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  if (overlay == OverlayMode::None) {
    handleGameplayInput();
    const uint32_t now = millis();
    if (!gameOver && now - lastDropMs >= getDropIntervalMs()) {
      stepGravity();
      lastDropMs = now;
      requestUpdate();
    }
  } else {
    handleOverlayInput();
  }
}

void LegacyGridDiagnosticsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 24, "INKMOD // AFTER DARK", true, EpdFontFamily::BOLD);
  const Layout layout = computeLayout();
  renderBoard(layout);
  renderSidePanel(layout);
  if (overlay != OverlayMode::None) {
    renderOverlay();
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int hintsTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int phraseBandTop = std::max(layout.boardY + layout.boardHeight + 8, hintsTop - 76);
  renderer.fillRect(0, phraseBandTop, renderer.getScreenWidth(), hintsTop - phraseBandTop, false);
  renderer.drawLine(24, phraseBandTop, renderer.getScreenWidth() - 24, phraseBandTop, true);

  const auto phraseLines = renderer.wrappedText(UI_10_FONT_ID, currentPhrase(), renderer.getScreenWidth() - 52, 2);
  const int phraseBlockHeight = static_cast<int>(phraseLines.size()) * renderer.getLineHeight(UI_10_FONT_ID) +
                                std::max(0, static_cast<int>(phraseLines.size()) - 1) * 2;
  int phraseY = phraseBandTop + std::max(8, (hintsTop - phraseBandTop - phraseBlockHeight) / 2);
  for (const auto& line : phraseLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, phraseY, line.c_str());
    phraseY += renderer.getLineHeight(UI_10_FONT_ID) + 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(fullRefreshPending ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  fullRefreshPending = false;
}

void LegacyGridDiagnosticsActivity::startNewGame(const bool clearSave) {
  if (clearSave) {
    clearSavedGame();
  }
  clearBoard();
  score = 0;
  linesCleared = 0;
  level = 1;
  overlay = OverlayMode::None;
  overlaySelection = 0;
  gameOver = false;
  nextPiece = randomPiece();
  spawnPiece();
  lastDropMs = millis();
  phraseIndex = 0;
  requestUpdate();
}

void LegacyGridDiagnosticsActivity::clearBoard() {
  for (auto& row : board) {
    row.fill(-1);
  }
}

void LegacyGridDiagnosticsActivity::spawnPiece() {
  currentPiece = nextPiece;
  currentPiece.rotation = 0;
  currentPiece.row = 1;
  currentPiece.col = COLS / 2;
  nextPiece = randomPiece();
  if (!canPlace(currentPiece)) {
    if (score > highScore) { highScore = score; saveBestScore(); }
    gameOver = true;
    fullRefreshPending = true;
    overlay = OverlayMode::GameOver;
    overlaySelection = 0;
    clearSavedGame();
  }
}

void LegacyGridDiagnosticsActivity::openPauseMenu() {
  overlay = OverlayMode::Pause;
  overlaySelection = 0;
  requestUpdate();
}

void LegacyGridDiagnosticsActivity::closeOverlay() {
  overlay = OverlayMode::None;
  overlaySelection = 0;
  lastDropMs = millis();
  requestUpdate();
}

bool LegacyGridDiagnosticsActivity::loadSavedGame() {
  FsFile file;
  if (!Storage.openFileForRead("TTR", SAVE_PATH, file)) {
    return false;
  }
  SaveData saved;
  const bool readOk = file.read(reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == static_cast<int>(sizeof(saved));
  file.close();
  if (!readOk || saved.magic != SAVE_MAGIC || saved.version != SAVE_VERSION) {
    clearSavedGame();
    return false;
  }

  chillModeEnabled = saved.chillModeEnabled != 0;
  gameOver = saved.gameOver != 0;
  score = saved.score;
  linesCleared = saved.linesCleared;
  level = saved.level;
  currentPiece = saved.currentPiece;
  nextPiece = saved.nextPiece;
  for (int row = 0; row < ROWS; ++row) {
    for (int col = 0; col < COLS; ++col) {
      board[row][col] = saved.board[row][col];
    }
  }
  if (!isBoardStateValid()) {
    clearSavedGame();
    return false;
  }
  overlay = gameOver ? OverlayMode::GameOver : OverlayMode::None;
  overlaySelection = 0;
  lastDropMs = millis();
  return true;
}

void LegacyGridDiagnosticsActivity::saveGame() const {
  if (!isBoardStateValid()) {
    return;
  }
  Storage.mkdir(SAVE_DIR);
  FsFile file;
  if (!Storage.openFileForWrite("TTR", SAVE_PATH, file)) {
    return;
  }
  SaveData saved;
  saved.magic = SAVE_MAGIC;
  saved.version = SAVE_VERSION;
  saved.chillModeEnabled = chillModeEnabled ? 1 : 0;
  saved.gameOver = gameOver ? 1 : 0;
  saved.score = score;
  saved.linesCleared = linesCleared;
  saved.level = level;
  saved.currentPiece = currentPiece;
  saved.nextPiece = nextPiece;
  for (int row = 0; row < ROWS; ++row) {
    for (int col = 0; col < COLS; ++col) {
      saved.board[row][col] = board[row][col];
    }
  }
  file.write(reinterpret_cast<const uint8_t*>(&saved), sizeof(saved));
  file.close();
}

void LegacyGridDiagnosticsActivity::clearSavedGame() const {
  if (Storage.exists(SAVE_PATH)) {
    Storage.remove(SAVE_PATH);
  }
}

bool LegacyGridDiagnosticsActivity::isBoardStateValid() const {
  if (level == 0 || !isPieceValid(currentPiece) || !isPieceValid(nextPiece)) {
    return false;
  }
  for (const auto& row : board) {
    for (const int8_t cell : row) {
      if (cell < -1 || cell >= PIECE_COUNT) {
        return false;
      }
    }
  }
  return true;
}

bool LegacyGridDiagnosticsActivity::canPlace(const Piece& piece, const int rowOffset, const int colOffset,
                              const int rotationOverride) const {
  const int rotation = rotationOverride >= 0 ? rotationOverride : piece.rotation;
  for (const auto& cell : TETROMINOES[piece.type][rotation]) {
    const int row = piece.row + rowOffset + cell.row;
    const int col = piece.col + colOffset + cell.col;
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS || board[row][col] != -1) {
      return false;
    }
  }
  return true;
}

bool LegacyGridDiagnosticsActivity::isCurrentCellActive(const int row, const int col) const {
  if (gameOver) {
    return false;
  }
  for (const auto& cell : TETROMINOES[currentPiece.type][currentPiece.rotation]) {
    if (currentPiece.row + cell.row == row && currentPiece.col + cell.col == col) {
      return true;
    }
  }
  return false;
}

bool LegacyGridDiagnosticsActivity::tryMove(const int rowDelta, const int colDelta) {
  if (!canPlace(currentPiece, rowDelta, colDelta)) {
    return false;
  }
  currentPiece.row += rowDelta;
  currentPiece.col += colDelta;
  requestUpdate();
  return true;
}

bool LegacyGridDiagnosticsActivity::tryRotate() {
  const int nextRotation = (currentPiece.rotation + 1) % 4;
  if (!canPlace(currentPiece, 0, 0, nextRotation)) {
    return false;
  }
  currentPiece.rotation = static_cast<uint8_t>(nextRotation);
  requestUpdate();
  return true;
}

void LegacyGridDiagnosticsActivity::softDrop() {
  if (!tryMove(1, 0)) {
    lockPiece();
  }
  lastDropMs = millis();
}

void LegacyGridDiagnosticsActivity::hardDrop() {
  while (canPlace(currentPiece, 1, 0)) {
    ++currentPiece.row;
  }
  lockPiece();
  lastDropMs = millis();
}

void LegacyGridDiagnosticsActivity::stepGravity() {
  if (!tryMove(1, 0)) {
    lockPiece();
  }
}

void LegacyGridDiagnosticsActivity::lockPiece() {
  for (const auto& cell : TETROMINOES[currentPiece.type][currentPiece.rotation]) {
    const int row = currentPiece.row + cell.row;
    const int col = currentPiece.col + cell.col;
    if (row >= 0 && row < ROWS && col >= 0 && col < COLS) {
      board[row][col] = static_cast<int8_t>(currentPiece.type);
    }
  }
  const uint8_t cleared = clearFullRows();
  if (cleared > 0) {
    score += level * POINTS_PER_CLEAR[cleared];
    linesCleared += cleared;
    level = 1 + linesCleared / 10;
    nextPhrase();
    fullRefreshPending = true;
  }
  spawnPiece();
  requestUpdate();
}

uint8_t LegacyGridDiagnosticsActivity::clearFullRows() {
  uint8_t cleared = 0;
  for (int row = ROWS - 1; row >= 0; --row) {
    if (!std::all_of(board[row].begin(), board[row].end(), [](const int8_t cell) { return cell != -1; })) {
      continue;
    }
    for (int moveRow = row; moveRow > 0; --moveRow) {
      board[moveRow] = board[moveRow - 1];
    }
    board[0].fill(-1);
    ++cleared;
    ++row;
  }
  return cleared;
}

uint32_t LegacyGridDiagnosticsActivity::getDropIntervalMs() const {
  if (chillModeEnabled) {
    return CHILL_DROP_MS;
  }
  const uint32_t accelerated = level > 1 ? NORMAL_BASE_DROP_MS - std::min(NORMAL_BASE_DROP_MS, (level - 1) * NORMAL_DROP_STEP_MS)
                                          : NORMAL_BASE_DROP_MS;
  return std::max(NORMAL_MIN_DROP_MS, accelerated);
}

LegacyGridDiagnosticsActivity::Piece LegacyGridDiagnosticsActivity::randomPiece() const {
  Piece piece;
  piece.type = static_cast<uint8_t>(std::rand() % PIECE_COUNT);
  piece.col = COLS / 2;
  return piece;
}

void LegacyGridDiagnosticsActivity::handleGameplayInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    openPauseMenu();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    (void)tryMove(0, -1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    (void)tryMove(0, 1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    softDrop();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    hardDrop();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
             mappedInput.getHeldTime() < FULL_REFRESH_HOLD_MS) {
    (void)tryRotate();
  }
}

void LegacyGridDiagnosticsActivity::handleOverlayInput() {
  const int itemCount = overlay == OverlayMode::Pause ? 4 : 2;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (overlay == OverlayMode::Pause) {
      closeOverlay();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    overlaySelection = overlaySelection == 0 ? itemCount - 1 : overlaySelection - 1;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    overlaySelection = (overlaySelection + 1) % itemCount;
    requestUpdate();
    return;
  }
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.getHeldTime() >= FULL_REFRESH_HOLD_MS) {
    return;
  }
  if (overlay == OverlayMode::GameOver) {
    overlaySelection == 0 ? startNewGame() : finish();
    return;
  }
  if (overlaySelection == 0) {
    closeOverlay();
  } else if (overlaySelection == 1) {
    startNewGame();
  } else if (overlaySelection == 2) {
    chillModeEnabled = !chillModeEnabled;
    lastDropMs = millis();
    requestUpdate();
  } else {
    finish();
  }
}

LegacyGridDiagnosticsActivity::Layout LegacyGridDiagnosticsActivity::computeLayout() const {
  Layout layout;
  constexpr int margin = 16;
  constexpr int gap = 16;
  constexpr int panelWidth = 132;
  constexpr int headerHeight = 72;
  constexpr int footerHeight = 112;
  layout.cellSize = std::max(16, std::min((renderer.getScreenWidth() - margin * 2 - gap - panelWidth) / COLS,
                                          (renderer.getScreenHeight() - headerHeight - footerHeight) / ROWS));
  layout.boardWidth = layout.cellSize * COLS;
  layout.boardHeight = layout.cellSize * ROWS;
  layout.boardX = margin;
  layout.boardY = headerHeight + (renderer.getScreenHeight() - headerHeight - footerHeight - layout.boardHeight) / 2;
  layout.panelX = layout.boardX + layout.boardWidth + gap;
  layout.panelWidth = renderer.getScreenWidth() - layout.panelX - margin;
  layout.previewCellSize = std::max(10, std::min(18, (layout.panelWidth - 16) / 4));
  layout.previewBoxSize = layout.previewCellSize * 4 + 12;
  return layout;
}

void LegacyGridDiagnosticsActivity::renderBoard(const Layout& layout) const {
  renderer.drawRect(layout.boardX - 3, layout.boardY - 3, layout.boardWidth + 6, layout.boardHeight + 6);
  for (int row = 0; row < ROWS; ++row) {
    for (int col = 0; col < COLS; ++col) {
      const bool active = isCurrentCellActive(row, col);
      if (board[row][col] == -1 && !active) {
        continue;
      }
      const int x = layout.boardX + col * layout.cellSize;
      const int y = layout.boardY + row * layout.cellSize;
      renderer.fillRect(x + 1, y + 1, layout.cellSize - 2, layout.cellSize - 2);
      const int inset = active ? std::max(3, layout.cellSize / 4) : std::max(4, layout.cellSize / 3);
      renderer.fillRect(x + inset, y + inset, layout.cellSize - inset * 2, layout.cellSize - inset * 2, false);
    }
  }
}

void LegacyGridDiagnosticsActivity::renderSidePanel(const Layout& layout) const {
  char value[20];
  int y = layout.boardY + 8;
  auto metric = [&](const char* label, const uint32_t number) {
    renderer.drawText(SMALL_FONT_ID, layout.panelX, y, label, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 3;
    std::snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(number));
    renderer.drawText(UI_10_FONT_ID, layout.panelX, y, value);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 15;
  };
  metric("СЧЁТ", score);
  metric("ЛИНИИ", linesCleared);
  renderer.drawText(SMALL_FONT_ID, layout.panelX, y, "ДАЛЬШЕ", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
  renderPreview(layout, y);
  y += layout.previewBoxSize + 16;
  renderer.drawText(SMALL_FONT_ID, layout.panelX, y, "РЕЖИМ", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
  renderer.drawText(SMALL_FONT_ID, layout.panelX, y,
                    chillModeEnabled ? "ЛЕНЬ" : "НОРМА");
}

void LegacyGridDiagnosticsActivity::renderPreview(const Layout& layout, const int y) const {
  renderer.drawRect(layout.panelX, y, layout.previewBoxSize, layout.previewBoxSize);
  for (const auto& cell : TETROMINOES[nextPiece.type][0]) {
    const int x = layout.panelX + 6 + (cell.col + 1) * layout.previewCellSize;
    const int cellY = y + 6 + cell.row * layout.previewCellSize;
    renderer.fillRect(x + 1, cellY + 1, layout.previewCellSize - 2, layout.previewCellSize - 2);
  }
}

void LegacyGridDiagnosticsActivity::renderOverlay() const {
  const int count = overlay == OverlayMode::Pause ? 4 : 2;
  const int width = 360;
  const int height = overlay == OverlayMode::Pause ? 286 : 230;
  const int x = (renderer.getScreenWidth() - width) / 2;
  const int y = (renderer.getScreenHeight() - height) / 2;
  renderer.fillRect(x, y, width, height, false);
  renderer.drawRect(x, y, width, height, 2, true);

  const char* overlayTitle = "ПАУЗА";
  if (overlay == OverlayMode::GameOver) {
    if (score >= highScore && score > 0) {
      overlayTitle = "НОВЫЙ РЕКОРД. НУ ХОТЬ ЗДЕСЬ ТЫ МОЛОДЕЦ.";
    } else {
      static constexpr const char* gameOverPhrases[] = {
          "БРЕННЫЙ МИР ПОБЕДИЛ.",
          "БАШНЯ ПАЛА. МИР ДОВОЛЕН.",
          "НУ ВОТ. КНИГУ ВСЁ-ТАКИ ПРИДЁТСЯ ЧИТАТЬ."};
      overlayTitle = gameOverPhrases[score % 3];
    }
  }

  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, overlayTitle, width - 28, overlay == OverlayMode::Pause ? 1 : 2,
                           EpdFontFamily::BOLD);
  int titleY = y + 18;
  for (const auto& line : titleLines) {
    renderer.drawCenteredText(UI_12_FONT_ID, titleY, line.c_str(), true, EpdFontFamily::BOLD);
    titleY += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  }

  const int menuStartY = titleY + 14;
  const int rowStep = 40;
  for (int i = 0; i < count; ++i) {
    const int rowY = menuStartY + i * rowStep;
    const char* text = nullptr;
    if (overlay == OverlayMode::GameOver) {
      text = i == 0 ? "ЕЩЁ РАЗ" : tr(STR_EXIT);
    } else if (i == 0) {
      text = "ПРОДОЛЖИТЬ";
    } else if (i == 1) {
      text = "ЕЩЁ РАЗ";
    } else if (i == 2) {
      text = chillModeEnabled ? "ЛЕНЬ" : "НОРМА";
    } else {
      text = tr(STR_EXIT);
    }
    if (i == overlaySelection) {
      renderer.fillRect(x + 18, rowY - 6, width - 36, 32);
      renderer.drawCenteredText(UI_10_FONT_ID, rowY, text, false, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, rowY, text);
    }
  }
}

bool LegacyGridDiagnosticsActivity::isPieceValid(const Piece& piece) {
  return piece.type < PIECE_COUNT && piece.rotation < 4 && piece.row >= -1 && piece.row < ROWS &&
         piece.col >= -3 && piece.col < COLS;
}
