#pragma once

#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class WordleActivity final : public Activity {
 public:
  explicit WordleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wordle", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  static constexpr const char* WORDLES_PATH = "/wordle/wordles.json";
  static constexpr const char* NONWORDLES_PATH = "/wordle/nonwordles.json";
  // Update these URLs to point to your hosted JSON word list files
  static constexpr const char* WORDLES_URL = "https://raw.githubusercontent.com/stuartpb/wordles/main/wordles.json";
  static constexpr const char* NONWORDLES_URL = "https://raw.githubusercontent.com/stuartpb/wordles/main/nonwordles.json";
  static constexpr int WORD_LENGTH = 5;
  static constexpr int MAX_GUESSES = 6;

  // Tile state for each cell in the grid
  enum class TileState { Empty, Filled, Correct, WrongPosition, Absent };

  // Letter state for keyboard coloring (best known state per letter)
  enum class LetterState { Unknown, Absent, WrongPosition, Correct };

  // Game state
  char targetWord[WORD_LENGTH + 1] = {};
  char guesses[MAX_GUESSES][WORD_LENGTH + 1] = {};
  TileState tileStates[MAX_GUESSES][WORD_LENGTH] = {};
  int currentRow = 0;
  int currentCol = 0;
  bool gameOver = false;
  bool won = false;
  int wordCount = 0;

  // Keyboard state
  LetterState letterStates[26] = {};  // A-Z
  int selectedRow = 0;
  int selectedCol = 0;
  ButtonNavigator buttonNavigator;

  // Keyboard layout
  static constexpr int KB_ROWS = 3;
  static constexpr const char* kbRow0 = "QWERTYUIOP";   // 10 keys
  static constexpr const char* kbRow1 = "ASDFGHJKL";     // 9 keys
  static constexpr const char* kbRow2 = "ZXCVBNM\x02";  // 7 letters + DEL = 8 positions
  static constexpr char KEY_DELETE = '\x02';

  int getKbRowLength(int row) const;
  char getKbChar(int row, int col) const;

  // Word list management
  bool wordListReady = false;
  bool downloadPromptShown = false;
  bool loadWordList();
  int countJsonWords(const char* path);
  bool loadRandomWord();
  bool isValidWord(const char* word);
  void downloadWordLists();

  // Game logic
  void submitGuess();
  void checkGuess();
  void newGame();

  // Rendering helpers
  void drawGrid();
  void drawKeyboard();
  void drawOverlay();

  // Popup state
  bool showingPopup = false;
  unsigned long popupStartTime = 0;
  static constexpr unsigned long POPUP_DURATION_MS = 1500;

  // Legend state
  bool showLegend = false;
  bool legendShownOnce = false;
  void drawLegend();
};
