#include "WordleActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include <cctype>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

// --- Keyboard helpers ---

int WordleActivity::getKbRowLength(int row) const {
  switch (row) {
    case 0: return 10;  // QWERTYUIOP
    case 1: return 9;   // ASDFGHJKL
    case 2: return 8;   // Z X C V B N M DEL
    default: return 0;
  }
}

char WordleActivity::getKbChar(int row, int col) const {
  switch (row) {
    case 0: return kbRow0[col];
    case 1: return kbRow1[col];
    case 2: return kbRow2[col];
    default: return '\0';
  }
}

// --- Word list management ---
// JSON format: ["word1","word2",...] — flat array of lowercase 5-letter strings.
// Parsing uses a lightweight state machine (no JSON library needed).

// Count quoted 5-letter lowercase words in a JSON array file.
int WordleActivity::countJsonWords(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("WORDLE", path, file)) return 0;

  int count = 0;
  int wordLen = 0;
  bool inWord = false;

  while (true) {
    int b = file.read();
    if (b < 0) break;
    if (b == '"') {
      if (inWord) {
        if (wordLen == WORD_LENGTH) count++;
        inWord = false;
      } else {
        inWord = true;
        wordLen = 0;
      }
    } else if (inWord) {
      if (b >= 'a' && b <= 'z') {
        wordLen++;
      } else {
        inWord = false;  // non-lowercase char inside quotes — not a word entry
      }
    }
  }

  file.close();
  return count;
}

bool WordleActivity::loadWordList() {
  if (!Storage.exists(WORDLES_PATH) || !Storage.exists(NONWORDLES_PATH)) {
    return false;
  }
  wordCount = countJsonWords(WORDLES_PATH);
  LOG_DBG("WORDLE", "Wordle answer count: %d", wordCount);
  return wordCount > 0;
}

bool WordleActivity::loadRandomWord() {
  if (wordCount <= 0) return false;

  int targetIdx = static_cast<int>(esp_random() % wordCount);

  HalFile file;
  if (!Storage.openFileForRead("WORDLE", WORDLES_PATH, file)) return false;

  int validCount = 0;
  int wordLen = 0;
  bool inWord = false;
  char wordBuf[WORD_LENGTH + 1];
  bool found = false;

  while (true) {
    int b = file.read();
    if (b < 0) break;
    if (b == '"') {
      if (inWord) {
        if (wordLen == WORD_LENGTH) {
          if (validCount == targetIdx) {
            wordBuf[wordLen] = '\0';
            found = true;
            break;
          }
          validCount++;
        }
        inWord = false;
      } else {
        inWord = true;
        wordLen = 0;
      }
    } else if (inWord) {
      if (b >= 'a' && b <= 'z' && wordLen < WORD_LENGTH) {
        wordBuf[wordLen++] = static_cast<char>(b);
      } else {
        inWord = false;
        wordLen = 0;
      }
    }
  }

  file.close();

  if (found) {
    for (int i = 0; i < WORD_LENGTH; i++) {
      targetWord[i] = static_cast<char>(toupper(wordBuf[i]));
    }
    targetWord[WORD_LENGTH] = '\0';
    LOG_DBG("WORDLE", "Target word: %s", targetWord);
    return true;
  }
  return false;
}

bool WordleActivity::isValidWord(const char* word) {
  // Convert guess to lowercase for comparison
  char lowerWord[WORD_LENGTH + 1];
  for (int i = 0; i < WORD_LENGTH; i++) {
    lowerWord[i] = static_cast<char>(tolower(word[i]));
  }
  lowerWord[WORD_LENGTH] = '\0';

  // Scan both files — valid if found in either wordles.json or nonwordles.json
  const char* paths[] = {WORDLES_PATH, NONWORDLES_PATH};
  for (const char* path : paths) {
    HalFile file;
    if (!Storage.openFileForRead("WORDLE", path, file)) continue;

    int wordLen = 0;
    bool inWord = false;
    char wordBuf[WORD_LENGTH + 1];
    bool found = false;

    while (true) {
      int b = file.read();
      if (b < 0) break;
      if (b == '"') {
        if (inWord) {
          if (wordLen == WORD_LENGTH) {
            wordBuf[wordLen] = '\0';
            if (strncmp(wordBuf, lowerWord, WORD_LENGTH) == 0) {
              found = true;
              break;
            }
          }
          inWord = false;
        } else {
          inWord = true;
          wordLen = 0;
        }
      } else if (inWord) {
        if (b >= 'a' && b <= 'z' && wordLen < WORD_LENGTH) {
          wordBuf[wordLen++] = static_cast<char>(b);
        } else {
          inWord = false;
          wordLen = 0;
        }
      }
    }

    file.close();
    if (found) return true;
  }
  return false;
}

void WordleActivity::downloadWordLists() {
  Storage.ensureDirectoryExists("/wordle");

  auto r1 = HttpDownloader::downloadToFile(WORDLES_URL, WORDLES_PATH);
  if (r1 != HttpDownloader::OK) {
    LOG_ERR("WORDLE", "Failed to download wordles.json: %d", r1);
    return;
  }

  auto r2 = HttpDownloader::downloadToFile(NONWORDLES_URL, NONWORDLES_PATH);
  if (r2 != HttpDownloader::OK) {
    LOG_ERR("WORDLE", "Failed to download nonwordles.json: %d", r2);
    return;
  }

  LOG_DBG("WORDLE", "Word lists downloaded successfully");
  wordListReady = loadWordList();
  if (wordListReady) loadRandomWord();
}

// --- Game logic ---

void WordleActivity::checkGuess() {
  const char* guess = guesses[currentRow];

  // Track which target letters have been "used" (for duplicate handling)
  bool targetUsed[WORD_LENGTH] = {};

  // First pass: mark exact matches (Correct)
  for (int i = 0; i < WORD_LENGTH; i++) {
    if (guess[i] == targetWord[i]) {
      tileStates[currentRow][i] = TileState::Correct;
      targetUsed[i] = true;

      int letterIdx = guess[i] - 'A';
      letterStates[letterIdx] = LetterState::Correct;
    }
  }

  // Second pass: mark wrong-position letters
  for (int i = 0; i < WORD_LENGTH; i++) {
    if (tileStates[currentRow][i] == TileState::Correct) continue;

    bool foundWrongPos = false;
    for (int j = 0; j < WORD_LENGTH; j++) {
      if (!targetUsed[j] && guess[i] == targetWord[j]) {
        foundWrongPos = true;
        targetUsed[j] = true;
        break;
      }
    }

    int letterIdx = guess[i] - 'A';
    if (foundWrongPos) {
      tileStates[currentRow][i] = TileState::WrongPosition;
      // Only upgrade from Unknown or Absent
      if (letterStates[letterIdx] != LetterState::Correct) {
        letterStates[letterIdx] = LetterState::WrongPosition;
      }
    } else {
      tileStates[currentRow][i] = TileState::Absent;
      // Only set Absent if not already better
      if (letterStates[letterIdx] == LetterState::Unknown) {
        letterStates[letterIdx] = LetterState::Absent;
      }
    }
  }

  // Check win
  bool allCorrect = true;
  for (int i = 0; i < WORD_LENGTH; i++) {
    if (tileStates[currentRow][i] != TileState::Correct) {
      allCorrect = false;
      break;
    }
  }

  if (allCorrect) {
    won = true;
    gameOver = true;
  } else {
    currentRow++;
    currentCol = 0;
    if (currentRow >= MAX_GUESSES) {
      gameOver = true;
    }
  }
}

void WordleActivity::submitGuess() {
  if (currentCol < WORD_LENGTH) return;

  // Null-terminate the guess
  guesses[currentRow][WORD_LENGTH] = '\0';

  if (!isValidWord(guesses[currentRow])) {
    showingPopup = true;
    popupStartTime = millis();
    requestUpdate();
    return;
  }

  checkGuess();
  requestUpdate();
}

void WordleActivity::newGame() {
  currentRow = 0;
  currentCol = 0;
  gameOver = false;
  won = false;
  showingPopup = false;

  memset(guesses, 0, sizeof(guesses));
  memset(tileStates, 0, sizeof(tileStates));
  for (int i = 0; i < 26; i++) {
    letterStates[i] = LetterState::Unknown;
  }

  selectedRow = 0;
  selectedCol = 0;

  loadRandomWord();
  requestUpdate();
}

// --- Activity lifecycle ---

void WordleActivity::onEnter() {
  Activity::onEnter();

  wordListReady = loadWordList();

  if (wordListReady) {
    loadRandomWord();
  }

  if (!legendShownOnce) {
    legendShownOnce = true;
    showLegend = true;
  }

  requestUpdate();
}

void WordleActivity::onExit() { Activity::onExit(); }

void WordleActivity::loop() {
  // Handle popup timeout
  if (showingPopup && (millis() - popupStartTime > POPUP_DURATION_MS)) {
    showingPopup = false;
    requestUpdate();
  }

  // If word list is not ready, handle download prompt
  if (!wordListReady && !downloadPromptShown) {
    downloadPromptShown = true;
    // Render will show the download prompt
    requestUpdate();
    return;
  }

  if (!wordListReady) {
    // Waiting for user to confirm download or exit
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED) {
        // Show downloading popup and do download
        requestUpdateAndWait();
        downloadWordLists();
        if (wordListReady) {
          newGame();
        }
        requestUpdate();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      activityManager.goHome();
    }
    return;
  }

  // Legend overlay - any key dismisses it
  if (showLegend) {
    if (mappedInput.wasAnyPressed()) {
      showLegend = false;
      requestUpdate();
    }
    return;
  }

  // Game over - any key starts new game
  if (gameOver) {
    if (mappedInput.wasAnyPressed()) {
      newGame();
    }
    return;
  }

  // Don't process input while popup is showing
  if (showingPopup) return;

  // Back button = exit
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  // Keyboard navigation — pressing Up from the top row opens the legend
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (selectedRow == 0) {
      showLegend = true;
    } else {
      selectedRow = ButtonNavigator::previousIndex(selectedRow, KB_ROWS);
      int maxCol = getKbRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, KB_ROWS);
    int maxCol = getKbRowLength(selectedRow) - 1;
    if (selectedCol > maxCol) selectedCol = maxCol;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    int maxCol = getKbRowLength(selectedRow);
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    int maxCol = getKbRowLength(selectedRow);
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol);
    requestUpdate();
  });

  // Confirm = select keyboard key; auto-submit when 5 letters typed unless DEL is selected
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    char c = getKbChar(selectedRow, selectedCol);
    if (c == KEY_DELETE) {
      if (currentCol > 0) {
        currentCol--;
        guesses[currentRow][currentCol] = '\0';
        requestUpdate();
      }
    } else if (currentCol == WORD_LENGTH) {
      submitGuess();
    } else if (c >= 'A' && c <= 'Z') {
      if (currentCol < WORD_LENGTH) {
        guesses[currentRow][currentCol] = c;
        currentCol++;
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

void WordleActivity::drawGrid() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Grid sizing
  constexpr int tileSize = 50;
  constexpr int tileGap = 6;
  const int gridWidth = WORD_LENGTH * tileSize + (WORD_LENGTH - 1) * tileGap;
  const int gridStartX = (pageWidth - gridWidth) / 2;
  const int gridStartY = metrics.topPadding + metrics.headerHeight + 30;  // matches drawKeyboard offset

  for (int row = 0; row < MAX_GUESSES; row++) {
    for (int col = 0; col < WORD_LENGTH; col++) {
      int x = gridStartX + col * (tileSize + tileGap);
      int y = gridStartY + row * (tileSize + tileGap);

      TileState state = tileStates[row][col];
      char letter = guesses[row][col];
      bool hasLetter = letter >= 'A' && letter <= 'Z';

      // For current row with typed-but-not-submitted letters
      if (row == currentRow && !gameOver && state == TileState::Empty && hasLetter) {
        state = TileState::Filled;
      }

      // Revealed tiles all get black fill; unguessed stay white
      switch (state) {
        case TileState::Correct:
        case TileState::WrongPosition:
        case TileState::Absent:
          renderer.fillRect(x, y, tileSize, tileSize, true);  // Black fill for all revealed
          break;
        case TileState::Filled:
          renderer.fillRect(x, y, tileSize, tileSize, false);  // White fill
          renderer.drawRect(x, y, tileSize, tileSize, 2, true);  // Thick border
          break;
        case TileState::Empty:
        default:
          renderer.fillRect(x, y, tileSize, tileSize, false);  // White fill
          renderer.drawRect(x, y, tileSize, tileSize, 1, true);  // Thin border
          break;
      }

      // Draw letter
      if (hasLetter) {
        char text[2] = {letter, '\0'};
        int textWidth = renderer.getTextWidth(BOOKERLY_16_FONT_ID, text);
        int textX = x + (tileSize - textWidth) / 2;
        int textHeight = renderer.getLineHeight(BOOKERLY_16_FONT_ID);
        bool revealed = (state == TileState::Correct || state == TileState::WrongPosition ||
                         state == TileState::Absent);
        // Shift text up slightly on revealed tiles to leave room for indicator
        int textY = y + (tileSize - textHeight) / 2 - (revealed ? 4 : 0);
        renderer.drawText(BOOKERLY_16_FONT_ID, textX, textY, text, !revealed);  // white on black, black on white
      }

      // B&W state indicators drawn in white on the black tile background:
      // Correct  → underline bar at bottom
      // WrongPos → filled square dot at bottom
      if (state == TileState::Correct) {
        renderer.fillRect(x + 8, y + tileSize - 9, tileSize - 16, 3, false);  // white underline
      } else if (state == TileState::WrongPosition) {
        renderer.fillRect(x + (tileSize / 2) - 3, y + tileSize - 10, 7, 7, false);  // white dot
      }
    }
  }
}

void WordleActivity::drawKeyboard() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  constexpr int keyWidth = 40;
  constexpr int keyHeight = 40;
  constexpr int keySpacing = 5;
  constexpr int tileSize = 50;
  constexpr int tileGap = 6;
  const int gridStartY = metrics.topPadding + metrics.headerHeight + 30;
  const int gridHeight = MAX_GUESSES * tileSize + (MAX_GUESSES - 1) * tileGap;
  const int kbStartY = gridStartY + gridHeight + 10;

  const char* rows[] = {kbRow0, kbRow1, kbRow2};
  // Row 2: Z X C V B N M DEL(7) = 8 keys; DEL is wider
  const int rowLengths[] = {10, 9, 8};
  constexpr int extraWidth = 16;  // Extra width for DEL key (col 7) in row 2

  for (int row = 0; row < KB_ROWS; row++) {
    int rowWidth = rowLengths[row] * keyWidth + (rowLengths[row] - 1) * keySpacing;
    if (row == 2) {
      rowWidth += extraWidth;  // DEL is wider
    }

    int startX = (pageWidth - rowWidth) / 2;
    int y = kbStartY + row * (keyHeight + keySpacing);

    for (int col = 0; col < rowLengths[row]; col++) {
      char c = rows[row][col];
      bool isSelected = (row == selectedRow && col == selectedCol);

      // DEL (col 7) in row 2 gets extra width
      int thisKeyWidth = keyWidth;
      if (row == 2 && col == 7) {
        thisKeyWidth = keyWidth + extraWidth;
      }

      int x = startX;
      for (int k = 0; k < col; k++) {
        int w = keyWidth;
        if (row == 2 && k == 7) {
          w = keyWidth + extraWidth;
        }
        x += w + keySpacing;
      }

      // Determine letter state for this key
      LetterState keyState = LetterState::Unknown;
      if (c >= 'A' && c <= 'Z') {
        keyState = letterStates[c - 'A'];
      }

      bool revealed = (keyState == LetterState::Correct || keyState == LetterState::WrongPosition ||
                       keyState == LetterState::Absent);

      // Draw key background:
      // - Selected: white fill + thick black border
      // - Revealed (Correct/WrongPos/Absent): black fill
      // - Unknown: white fill + thin black border
      if (isSelected) {
        renderer.fillRect(x, y, thisKeyWidth, keyHeight, false);   // White fill
        renderer.drawRect(x, y, thisKeyWidth, keyHeight, 3, true);  // Thick black border
      } else if (revealed) {
        renderer.fillRect(x, y, thisKeyWidth, keyHeight, true);    // Black fill
      } else {
        renderer.fillRect(x, y, thisKeyWidth, keyHeight, false);   // White fill
        renderer.drawRect(x, y, thisKeyWidth, keyHeight, 1, true);  // Thin black border
      }

      // Draw key label
      const char* label;
      char letterBuf[2] = {c, '\0'};
      label = (c == KEY_DELETE) ? "DEL" : letterBuf;

      int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
      int textX = x + (thisKeyWidth - textWidth) / 2;
      int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
      // Shift text up on revealed keys to leave room for indicator dot/underline
      bool hasIndicator = (keyState == LetterState::Correct || keyState == LetterState::WrongPosition);
      int textY = y + (keyHeight - textHeight) / 2 - (hasIndicator && !isSelected ? 3 : 0);

      bool whiteText = !isSelected && revealed;
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, !whiteText);

      // B&W state indicators (white marks on black key background)
      if (!isSelected) {
        if (keyState == LetterState::Correct) {
          renderer.fillRect(x + 4, y + keyHeight - 7, thisKeyWidth - 8, 2, false);  // white underline
        } else if (keyState == LetterState::WrongPosition) {
          renderer.fillRect(x + (thisKeyWidth / 2) - 2, y + keyHeight - 8, 5, 5, false);  // white dot
        }
      }
    }
  }
}

void WordleActivity::drawOverlay() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (won || gameOver) {
    const char* msg = won ? "You won!" : nullptr;
    char lossBuf[32];
    if (!won) {
      snprintf(lossBuf, sizeof(lossBuf), "The word was: %s", targetWord);
      msg = lossBuf;
    }
    const char* hint = "Press any key for new game";

    const int titleH = renderer.getLineHeight(BOOKERLY_16_FONT_ID);
    const int hintH = renderer.getLineHeight(UI_12_FONT_ID);
    constexpr int pad = 18;
    constexpr int gap = 12;
    const int panelH = pad + titleH + gap + hintH + pad;
    const int panelW = 370;
    const int panelX = (pageWidth - panelW) / 2;
    const int panelY = pageHeight / 2 - panelH / 2;

    renderer.fillRect(panelX, panelY, panelW, panelH, false);
    renderer.drawRect(panelX, panelY, panelW, panelH, 2, true);

    int msgW = renderer.getTextWidth(BOOKERLY_16_FONT_ID, msg);
    renderer.drawText(BOOKERLY_16_FONT_ID, panelX + (panelW - msgW) / 2, panelY + pad, msg, true);

    int hintW = renderer.getTextWidth(UI_12_FONT_ID, hint);
    renderer.drawText(UI_12_FONT_ID, panelX + (panelW - hintW) / 2, panelY + pad + titleH + gap, hint, true);
  }

  if (showingPopup) {
    GUI.drawPopup(renderer, "Not in word list");
  }
}

void WordleActivity::drawLegend() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // White panel centered on screen
  constexpr int panelW = 310;
  constexpr int panelH = 310;
  const int panelX = (pageWidth - panelW) / 2;
  const int panelY = (pageHeight - panelH) / 2;

  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH, 2, true);

  // Title
  const char* title = "Wordle Guide";
  int titleW = renderer.getTextWidth(BOOKERLY_16_FONT_ID, title);
  renderer.drawText(BOOKERLY_16_FONT_ID, panelX + (panelW - titleW) / 2, panelY - 2, title, true);
  renderer.fillRect(panelX + 10, panelY + 38, panelW - 20, 1, true);  // separator

  // Four example rows: tile + description
  constexpr int exSize = 36;
  const int exX = panelX + 16;
  const int descX = exX + exSize + 14;
  constexpr int rowSpacing = 62;
  int exY = panelY + 53;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);

  // Row 1: Correct (black + underline)
  renderer.fillRect(exX, exY, exSize, exSize, true);
  renderer.fillRect(exX + 4, exY + exSize - 7, exSize - 8, 2, false);  // underline
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH * 2) / 2, "Right letter,", true);
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH * 2) / 2 + lineH, "correct position", true);
  exY += rowSpacing;

  // Row 2: Wrong position (black + dot)
  renderer.fillRect(exX, exY, exSize, exSize, true);
  renderer.fillRect(exX + (exSize / 2) - 2, exY + exSize - 8, 5, 5, false);  // dot
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH * 2) / 2, "Right letter,", true);
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH * 2) / 2 + lineH, "wrong position", true);
  exY += rowSpacing;

  // Row 3: Absent (solid black)
  renderer.fillRect(exX, exY, exSize, exSize, true);
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH) / 2, "Letter not in word", true);
  exY += rowSpacing-15;

  // Row 4: Empty (white + border)
  renderer.fillRect(exX, exY, exSize, exSize, false);
  renderer.drawRect(exX, exY, exSize, exSize, 1, true);
  renderer.drawText(UI_12_FONT_ID, descX, exY + (exSize - lineH) / 2, "Not guessed yet", true);

  // Dismiss hint
  renderer.fillRect(panelX + 10, panelY + panelH - 35, panelW - 20, 1, true);
  const char* hint = "Press any key to dismiss";
  int hintW = renderer.getTextWidth(UI_12_FONT_ID, hint);
  renderer.drawText(UI_12_FONT_ID, panelX + (panelW - hintW) / 2, panelY + panelH - 32, hint, true);
}

void WordleActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!wordListReady) {
    // Show download prompt
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Wordle");

    const int midY = renderer.getScreenHeight() / 2;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    if (WiFi.status() == WL_CONNECTED) {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Word lists not found.");
      renderer.drawCenteredText(UI_12_FONT_ID, midY + 2, "Press OK to download.");
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH * 2, "Word lists not found.");
      // renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH / 2, "Connect to WiFi to download, or");
      renderer.drawCenteredText(UI_12_FONT_ID, midY + lineH, "place in /wordle/ manually:");
      renderer.drawCenteredText(UI_12_FONT_ID, midY + lineH * 2 + 4, "wordles.json + nonwordles.json");
    }

    const auto labels = mappedInput.mapLabels("Back", "OK", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    return;
  }

  // Draw header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Wordle");

  // Draw "?" help icon in top-right of header (press Up from top keyboard row to open)
  constexpr int helpSize = 22;
  const int helpX = pageWidth - helpSize - 8;
  const int helpY = metrics.topPadding + (metrics.headerHeight - helpSize) / 2;
  renderer.fillRect(helpX, helpY, helpSize, helpSize, false);
  renderer.drawRect(helpX, helpY, helpSize, helpSize, 1, true);
  {
    int qW = renderer.getTextWidth(UI_12_FONT_ID, "?");
    int qH = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, helpX + (helpSize - qW) / 2, helpY + (helpSize - qH) / 2, "?", true);
  }

  // Draw game grid
  drawGrid();

  // Draw keyboard
  if (!gameOver) {
    drawKeyboard();
  }

  // Draw overlay (win/lose/popup)
  drawOverlay();

  // Draw button hints — label changes to "Submit" once 5 letters are entered
  const char* confirmLabel = (wordListReady && !gameOver && currentCol == WORD_LENGTH) ? "Submit" : "Select";
  const auto labels = mappedInput.mapLabels("Exit", confirmLabel, "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Legend overlays everything else
  if (showLegend) {
    drawLegend();
  }

  renderer.displayBuffer();
}
