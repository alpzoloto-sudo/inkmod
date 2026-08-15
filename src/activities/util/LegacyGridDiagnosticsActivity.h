#pragma once

#include <array>
#include <cstdint>

#include "../Activity.h"

class LegacyGridDiagnosticsActivity final : public Activity {
  struct CellOffset {
    int8_t row;
    int8_t col;
  };

  struct Piece {
    uint8_t type = 0;
    uint8_t rotation = 0;
    int8_t row = 1;
    int8_t col = 0;
  };

  enum class OverlayMode : uint8_t { None, Pause, GameOver };

  struct Layout {
    int boardX = 0;
    int boardY = 0;
    int boardWidth = 0;
    int boardHeight = 0;
    int cellSize = 0;
    int panelX = 0;
    int panelWidth = 0;
    int previewBoxSize = 0;
    int previewCellSize = 0;
  };

  static constexpr int ROWS = 20;
  static constexpr int COLS = 9;
  static constexpr int PIECE_COUNT = 7;
  static constexpr uint32_t CHILL_DROP_MS = 1150;
  static constexpr uint32_t NORMAL_BASE_DROP_MS = 900;
  static constexpr uint32_t NORMAL_DROP_STEP_MS = 60;
  static constexpr uint32_t NORMAL_MIN_DROP_MS = 220;
  static constexpr unsigned long FULL_REFRESH_HOLD_MS = 1500;

  struct SaveData {
    uint32_t magic = 0;
    uint8_t version = 0;
    uint8_t chillModeEnabled = 1;
    uint8_t gameOver = 0;
    uint8_t reserved = 0;
    uint32_t score = 0;
    uint32_t linesCleared = 0;
    uint32_t level = 1;
    Piece currentPiece{};
    Piece nextPiece{};
    int8_t board[ROWS][COLS]{};
  };

  static const CellOffset TETROMINOES[PIECE_COUNT][4][4];

  std::array<std::array<int8_t, COLS>, ROWS> board{};
  Piece currentPiece{};
  Piece nextPiece{};
  uint32_t score = 0;
  uint32_t linesCleared = 0;
  uint32_t level = 1;
  uint32_t lastDropMs = 0;
  int overlaySelection = 0;
  bool gameOver = false;
  bool chillModeEnabled = true;
  bool fullRefreshPending = false;
  OverlayMode overlay = OverlayMode::None;
  uint8_t phraseIndex = 0;
  uint32_t highScore = 0;
  void loadBestScore();
  void saveBestScore() const;
  void nextPhrase();
  const char* currentPhrase() const;

  void startNewGame(bool clearSave = true);
  void clearBoard();
  void spawnPiece();
  void openPauseMenu();
  void closeOverlay();
  bool loadSavedGame();
  void saveGame() const;
  void clearSavedGame() const;
  bool isBoardStateValid() const;
  bool canPlace(const Piece& piece, int rowOffset = 0, int colOffset = 0, int rotationOverride = -1) const;
  bool isCurrentCellActive(int row, int col) const;
  bool tryMove(int rowDelta, int colDelta);
  bool tryRotate();
  void softDrop();
  void hardDrop();
  void stepGravity();
  void lockPiece();
  uint8_t clearFullRows();
  uint32_t getDropIntervalMs() const;
  Piece randomPiece() const;
  void handleGameplayInput();
  void handleOverlayInput();
  Layout computeLayout() const;
  void renderBoard(const Layout& layout) const;
  void renderSidePanel(const Layout& layout) const;
  void renderPreview(const Layout& layout, int y) const;
  void renderOverlay() const;
  static bool isPieceValid(const Piece& piece);

 public:
  explicit LegacyGridDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LegacyGridDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
