/*
 * app_ai.cpp - Menu > AI. See app_ai.h for the screens and the keys; ai_net.h for the worker,
 * the card and the gates; gemini.h for every rule that can be tested on the Mac.
 */

#include "app_ai.h"
#include "app_books.h"          // bookRenderRun + fontMeasure: the Files viewer's and the e-reader's
#include "ai_net.h"
#include "gemini.h"

#include <esp_heap_caps.h>
#include <string.h>

/* Row keys. 0 is never a key (addOption refuses it: tests/check_menu_keys.py). */
static const MenuOption::keyType ROW_ASK     = 1;
static const MenuOption::keyType ROW_TOPIC   = 2;
static const MenuOption::keyType ROW_CLEAR   = 3;
static const MenuOption::keyType ROW_KEYINFO = 4;
static const MenuOption::keyType ROW_YES     = 5;
static const MenuOption::keyType ROW_NO      = 6;
static const MenuOption::keyType ROW_UPLOAD  = 7;
static const MenuOption::keyType ROW_RELOAD  = 8;
static const MenuOption::keyType ROW_RETRY   = 9;

#define AI_FONT         AKROBAT_BOLD_18        // the transcript: the Files viewer's face
#define AI_SMALL_FONT   AKROBAT_BOLD_16        // the model under an answer, the strips, the status box
#define AI_MARGIN       6
#define AI_STRIP_H      18                     // a strip under the transcript
#define AI_COMPOSE_H    (2 * AI_STRIP_H)       // ...and under the compose field: a refusal takes two rows
#define AI_STAGE_ROWS   3                      // the quota notice ("... until about 00:00 tomorrow") needs three
#define AI_BOX_LINES    (AI_STAGE_ROWS + 2)    // the WAITING status box: the stage, the model and time, the keys
#define AI_TEXT_CAP     (GEM_CHAT_FILE_MAX + GEM_Q_MAX + 6 * 1024)   // the saved chat + its decoration + one in flight
#define AI_POLL_MS      250u                   // the app timer while a question is in flight
#define AI_XFER_POLL_MS 1000u                  // ...and while the key uploader runs (a key landing is noticed)
#define AI_DIVIDER      "- - - - -  New topic  - - - - -"

AiApp::AiApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create AiApp");
  /* ⚠ msAppTimerEventPeriod is device-wide and the last app may have left its own value in it. */
  controlState.msAppTimerEventPeriod = 0;
  appState = AI_TRANSCRIPT;
  menu = NULL;
  textArea = NULL;
  text = (char*)heap_caps_malloc(AI_TEXT_CAP, MALLOC_CAP_SPIRAM);
  textCap = text ? AI_TEXT_CAP : 0;
  textLen = 0;
  nSegs = 0;
  page = 0;
  newestStart = 0;
  builtGen = 0;
  builtPending = false;
  memset(&pg, 0, sizeof(pg));
  line[0] = '\0';
  note = NULL;
  noteBuf[0] = '\0';
  memset(&prog, 0, sizeof(prog));
  progGen = progSec = 0;
  memset(&key, 0, sizeof(key));
  xferSeen = 0;
  /* Read gemini.txt now, every time the app opens (a key put on the card since works at once).
   * No key: the first screen says how to get one. A question still in flight from a visit
   * before: back to its status. */
  aiKeyReload();
  aiKeyInfo(&key);
  aiLoopTick();
  if (aiRequestActive()) {
    aiProgress(&prog);
    enterState(prog.cancelled ? AI_TRANSCRIPT : AI_WAITING);
  } else if (!key.usable && (!aiChat() || aiChat()->n == 0)) {
    enterState(AI_KEYINFO);
  } else {
    enterState(AI_TRANSCRIPT);
  }
}

AiApp::~AiApp() {
  log_d("destroy AiApp");
  if (appState == AI_XFER) {
    xferStop();                          // never leave the server up with no screen owning it
  }
  clearInputClaims();
  freeWidgets();
  free(text);
  text = NULL;
  controlState.msAppTimerEventPeriod = 0;    // do not leave the CPU waking for a closed app
}

/* Predictive text belongs to COMPOSE and dies with it (app_meshtastic.cpp's lesson: a t9Field
 * left set swallowed the digits and Up/Down on every later screen). */
void AiApp::clearInputClaims() {
  controlState.t9Field = false;
  controlState.t9.clear();
}

void AiApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
  if (textArea) {
    delete textArea;
    textArea = NULL;
  }
}

MenuWidget* AiApp::newMenu() {
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(), NULL,
                                 fonts[AKROBAT_BOLD_20], N_MAX_ITEMS, 8);
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

void AiApp::armTimer() {
  if (appState == AI_XFER) {
    controlState.msAppTimerEventPeriod = AI_XFER_POLL_MS;
  } else {
    controlState.msAppTimerEventPeriod = aiRequestActive() ? AI_POLL_MS : 0;
  }
}

/* The one door every screen change goes through. */
void AiApp::enterState(AiState s) {
  clearInputClaims();
  freeWidgets();
  appState = s;
  note = NULL;
  switch (s) {
  case AI_TRANSCRIPT:
    header->setTitle("AI");
    footer->setButtons("Menu", "Back");
    buildTranscript();
    toLastPage();
    break;
  case AI_WAITING:
    header->setTitle("AI");
    footer->setButtons("", "Back");
    buildTranscript();
    toLastPage();
    progGen = progSec = 0;
    break;
  case AI_MENU:
    header->setTitle("AI");
    footer->setButtons("Select", "Back");
    buildMenu();
    break;
  case AI_CONFIRM:
    header->setTitle("Clear chat?");
    footer->setButtons("Select", "Back");
    buildConfirm();
    break;
  case AI_COMPOSE:
    header->setTitle("Ask Gemini");
    // "Clear", not "Cancel": the key under it is BACK, which is backspace in a text field.
    // Cancel is END (app_meshtastic.cpp's compose, the same rules).
    footer->setButtons("Ask", "Clear");
    buildCompose();
    break;
  case AI_KEYINFO:
    header->setTitle("Key info");
    footer->setButtons("Select", "Back");
    buildKeyInfo();
    break;
  case AI_XFER:
    header->setTitle("Add key file");
    footer->setButtons("", "Stop");
    xferSeen = xferFilesAdded();
    xferStart(xferKeysConfig());          // /API Keys, made if missing (no card at boot = no folder)
    break;
  }
  armTimer();
}

// ---- the menus ----------------------------------------------------------------------------------

void AiApp::buildMenu() {
  menu = newMenu();
  menu->addOption("Ask a question", ROW_ASK);
  menu->addOption("New topic", ROW_TOPIC);
  menu->addOption("Clear chat...", ROW_CLEAR);
  menu->addOption("Key info", ROW_KEYINFO);
  /* Round 3: a toggle, OK flips it (NVS, ai_net.h). Its words come from gemini.h, where the suite
   * measures both states against this menu's Akrobat 20 (one row, 183 of 220 px). */
  menu->addOption(aiRetryTimeouts() ? GEM_RETRY_LABEL ": On" : GEM_RETRY_LABEL ": Off", ROW_RETRY);
  menu->addNoteWrapped("New topic: Gemini forgets the earlier questions (they stay on screen). A "
                       "topic also starts by itself two hours after the last question.");
  menu->addNoteWrapped("Retry if it times out: when Gemini sends nothing in time, ask once more, then "
                       "Flash-Lite once.");
}

void AiApp::buildConfirm() {
  menu = newMenu();
  menu->addNoteWrapped("Delete the whole saved chat, on screen and on the card?");
  menu->addOption("No", ROW_NO);
  menu->addOption("Yes, delete it", ROW_YES);
  menu->select(ROW_NO);                    // a note row is selectable: OK on it did nothing
}

void AiApp::buildKeyInfo() {
  aiKeyReload();
  aiKeyInfo(&key);
  menu = newMenu();
  char buf[160];
  if (key.state == AI_KEY_OK) {
    snprintf(buf, sizeof(buf), "Key found in %s", key.path ? key.path : "?");
    menu->addNoteWrapped(buf);
    menu->addNoteWrapped(key.line);        // "key: set (53 chars), model ..." - never the key
  } else if (key.state == AI_KEY_BAD) {
    snprintf(buf, sizeof(buf), "%s was refused:", key.path ? key.path : "gemini.txt");
    menu->addNoteWrapped(buf);
    menu->addNoteWrapped(key.line);
  } else if (key.state == AI_KEY_UNREADABLE) {
    menu->addNoteWrapped(key.usable ? "The card would not give up gemini.txt just now - the key "
                                      "read before is still used."
                                    : "The card would not give up gemini.txt - try again.");
  } else {
    menu->addNoteWrapped("No key yet. The AI needs your own (free) Gemini API key, in a file on "
                         "the card.");
  }
  if (aiQuotaNote(buf, sizeof(buf)) > 0) {
    menu->addNoteWrapped(buf);
  }
  menu->addOption("Add key file over WiFi", ROW_UPLOAD);
  menu->addOption("Read the key file again", ROW_RELOAD);
  menu->addNoteWrapped("1. On a computer open aistudio.google.com, sign in with a Google account, "
                       "choose Get API key, then Create API key.");
  menu->addNoteWrapped("2. Make a text file named gemini.txt with one line: key=YOUR_KEY");
  menu->addNoteWrapped("Optional lines: model=gemini-flash-latest, thinking=0 to 8192 (0 = off), "
                       "fallback=none, lookup=off.");
  menu->addNoteWrapped("3. Put it in the API Keys folder on the card: card reader, Files > API "
                       "Keys > Upload, or the row above. Use your home WiFi - the phone's own "
                       "hotspot is open.");
  menu->addNoteWrapped("The key stays on the card. It is never shown on screen and goes only to "
                       "Google.");
  /* The Wikipedia lookup (wiki_lookup.h), in two short notes: addNoteWrapped stops at 8 rows. */
  menu->addNoteWrapped("Before each question a few of its words - only those - are looked up on "
                       "Wikipedia, and Gemini is given what it finds.");
  menu->addNoteWrapped("Such an answer ends with a Wikipedia: line. lookup=off in gemini.txt "
                       "stops the lookup.");
  menu->select(ROW_UPLOAD);
}

// ---- compose ------------------------------------------------------------------------------------

void AiApp::buildCompose() {
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height() - AI_COMPOSE_H,
                                     "Type a question", controlState, GEM_Q_CAP,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
  /* AFTER setInputState, which clears it: a field gets predictive text by opting in. */
  controlState.t9Field = true;
}

/* Send. The text stays typed when it is refused - the reason goes in the strip, and Ask again is
 * the retry. */
void AiApp::ask() {
  const char* q = textArea ? textArea->getText() : NULL;   // (one internal malloc, owned by the widget)
  if (!q || !q[0]) {
    return;
  }
  if (aiAsk(q, noteBuf, sizeof(noteBuf))) {
    enterState(AI_WAITING);
  } else {
    note = noteBuf;
  }
}

/* Two rows under the field: a refusal's reason (wrapped - most are wider than one row, measured
 * with the firmware's Akrobat 16), or the counter. Orange says it was not asked. */
void AiApp::drawComposeStrip() {
  const int16_t y = (int16_t)(lcd.height() - footer->height() - AI_COMPOSE_H);
  lcd.fillRect(0, y, lcd.width(), AI_COMPOSE_H, WP_COLOR_0);
  lcd.setTextFont(fonts[AI_SMALL_FONT]);
  if (note) {
    lcd.setTextColor(WP_ACCENT_S, WP_COLOR_0);
    lcd.setTextDatum(TL_DATUM);
    drawWrapped(fonts[AI_SMALL_FONT], note, 4, y + 1, (uint16_t)(lcd.width() - 8), AI_STRIP_H, 2);
    return;
  }
  const size_t n = textArea ? textArea->textLength() : 0;
  const bool full = n >= GEM_Q_CAP;
  char s[24];
  snprintf(s, sizeof(s), full ? "%u/%u full" : "%u/%u", (unsigned)n, (unsigned)GEM_Q_CAP);
  lcd.setTextColor(full ? WP_ACCENT_S : WP_DISAB_1, WP_COLOR_0);
  lcd.setTextDatum(MR_DATUM);
  lcd.drawString(s, lcd.width() - 4, y + AI_STRIP_H + AI_STRIP_H / 2);
}

/* `s` in up to `maxRows` rows of `w` in `f` (TL_DATUM and the colour already set), broken at a
 * space; the last row is ellipsized. Returns the rows drawn. */
int AiApp::drawWrapped(SmoothFont* f, const char* s, int x, int y, uint16_t w, int lh, int maxRows) {
  lcd.setTextFont(f);
  int rows = 0;
  while (*s && rows < maxRows) {
    const int len = (int)strlen(s);
    const int fit = f->fitTextLength(s, w, 1);
    if (rows == maxRows - 1 || fit <= 0 || fit >= len) {
      guiDrawEllipsized(lcd, s, w, x, y);
      return rows + 1;
    }
    int cut = fit;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = fit;
    snprintf(line, sizeof(line), "%.*s", cut, s);
    lcd.drawString(line, x, y);
    s += cut;
    while (*s == ' ') s++;
    y += lh;
    rows++;
  }
  return rows;
}

// ---- the transcript -----------------------------------------------------------------------------

void AiApp::segAdd(uint8_t style) {
  if (nSegs > 0 && segs[nSegs - 1].style == style) {
    return;
  }
  if (nSegs > 0 && segs[nSegs - 1].start == textLen) {
    segs[nSegs - 1].style = style;         // nothing was written in the last one
    return;
  }
  if (nSegs < SEG_MAX) {
    segs[nSegs].start = (uint32_t)textLen;
    segs[nSegs].style = style;
    nSegs++;
  }
}

void AiApp::put(const char* s) {
  if (!text || !s) {
    return;
  }
  size_t n = strlen(s);
  if (textLen + n >= textCap) {
    n = textCap - 1 - textLen;
  }
  memcpy(text + textLen, s, n);
  textLen += n;
  text[textLen] = '\0';
}

int AiApp::styleAt(uint32_t off) const {
  int st = SEG_META;
  for (int i = 0; i < nSegs && segs[i].start <= off; i++) {
    st = segs[i].style;
  }
  return st;
}

bool AiApp::transcriptStale() const {
  return builtGen != aiChatGen() || builtPending != (aiPendingQuestion() != NULL);
}

void AiApp::buildTranscript() {
  textLen = 0;
  nSegs = 0;
  newestStart = 0;
  if (!text) {
    return;
  }
  text[0] = '\0';
  const GeminiChat* c = aiChat();
  const char* pending = aiPendingQuestion();
  builtGen = aiChatGen();
  builtPending = pending != NULL;
  if ((!c || c->n == 0) && !pending) {
    segAdd(SEG_META);
    put("Ask Gemini anything.\n\nOK: type a question.\nMenu: New topic, Clear chat, Key info.\n"
        "Up/Down: page through the chat.\n\nThe chat is saved on the card. Gemini sees the "
        "questions of the current topic; a topic starts by itself two hours after the last "
        "question, or by hand from the menu.");
    return;
  }
  for (int i = 0; c && i < c->n; i++) {
    const GeminiExchange* e = &c->ex[i];
    if (i == c->n - 1) {
      newestStart = (uint32_t)textLen;     // every exchange begins on a line of its own
    }
    if ((e->flags & GEM_EX_TOPIC) && i > 0) {
      segAdd(SEG_META);
      put(AI_DIVIDER "\n\n");
    }
    segAdd(SEG_QUESTION);
    put("> ");
    put(e->q);
    put("\n");
    if (e->flags & GEM_EX_FAILED) {
      segAdd(SEG_META);
      put("(no answer: ");
      put(e->a);
      put(")\n\n");
    } else {
      segAdd(SEG_ANSWER);
      put(e->a);
      put("\n");
      segAdd(SEG_META);
      put("    ");
      put(e->label);
      put("\n\n");
    }
  }
  if (c && c->breakPending) {
    segAdd(SEG_META);
    put(AI_DIVIDER "\n\n");
  }
  if (pending) {
    segAdd(SEG_QUESTION);
    put("> ");
    put(pending);
    put("\n");
  }
  // No blank rows under the newest line: the last page ends on text.
  while (textLen > 0 && text[textLen - 1] == '\n') {
    textLen--;
  }
  text[textLen] = '\0';
}

int AiApp::viewBottom() const {
  const int bottom = (int)lcd.height() - (int)footer->height();
  if (appState == AI_WAITING) {
    return bottom - AI_BOX_LINES * (fonts[AI_SMALL_FONT]->height() + 2) - 6;
  }
  return bottom - AI_STRIP_H;
}

int AiApp::viewLines() const {
  const int top = header->height();
  int n = (viewBottom() - top - 4) / (fonts[AI_FONT]->height() + 2);
  if (n > BOOK_MAX_LINES) n = BOOK_MAX_LINES;
  return n < 1 ? 1 : n;
}

int AiApp::viewWidth() const {
  return (int)lcd.width() - 2 * AI_MARGIN;
}

/* The page that ends at the newest line. */
void AiApp::toLastPage() {
  if (!text || textLen == 0) {
    page = 0;
    return;
  }
  BookMeasure m = { fonts[AI_FONT], fontMeasure };
  page = bookLayoutPrevPage(text, textLen, (uint32_t)textLen, viewWidth(), viewLines(), &m);
}

void AiApp::drawStrip(const char* s, uint16_t color) {
  const int16_t y = (int16_t)(lcd.height() - footer->height() - AI_STRIP_H);
  lcd.fillRect(0, y, lcd.width(), AI_STRIP_H, WP_COLOR_0);
  lcd.setTextFont(fonts[AI_SMALL_FONT]);
  lcd.setTextColor(color, WP_COLOR_0);
  lcd.setTextDatum(ML_DATUM);
  guiDrawEllipsized(lcd, s, lcd.width() - 8, 4, y + AI_STRIP_H / 2);
}

void AiApp::drawTranscript() {
  SmoothFont* f = fonts[AI_FONT];
  SmoothFont* fs = fonts[AI_SMALL_FONT];
  const int top = header->height();
  const int lh = f->height() + 2;
  lcd.fillRect(0, top, lcd.width(), viewBottom() - top, BLACK);
  if (!text) {
    return;
  }
  BookMeasure m = { f, fontMeasure };
  bookLayoutPage(text, textLen, page, viewWidth(), viewLines(), &m, &pg);
  lcd.setTextDatum(TL_DATUM);
  int y = top + 2;
  for (int i = 0; i < pg.nLines; i++) {
    if (!pg.lines[i].blank) {
      const int st = styleAt(pg.lines[i].off);
      SmoothFont* lf = st == SEG_META ? fs : f;
      lcd.setTextFont(lf);
      lcd.setTextColor(st == SEG_QUESTION ? WP_ACCENT_0 : (st == SEG_META ? GRAY_67 : WHITE), BLACK);
      bookRenderRun(lf, text + pg.lines[i].off, pg.lines[i].len, line, sizeof(line));
      lcd.drawString(line, AI_MARGIN, y + (st == SEG_META ? 1 : 0));
    }
    y += lh;
  }
}

/* WAITING's box under the transcript: the stage (two lines at most), the model and the time,
 * and what the keys do. */
void AiApp::drawStatus() {
  aiProgress(&prog);
  SmoothFont* fs = fonts[AI_SMALL_FONT];
  const int lh = fs->height() + 2;
  const int y0 = viewBottom();
  const int bottom = (int)lcd.height() - (int)footer->height();
  lcd.fillRect(0, y0, lcd.width(), bottom - y0, WP_COLOR_0);
  lcd.drawFastHLine(AI_MARGIN, y0 + 2, lcd.width() - 2 * AI_MARGIN, GRAY_33);
  lcd.setTextFont(fs);
  lcd.setTextDatum(TL_DATUM);
  const uint16_t w = (uint16_t)(lcd.width() - 2 * AI_MARGIN);
  int y = y0 + 5;
  // The stage, wrapped at spaces: the quota notice and its local time need all three rows.
  const char* st = prog.stage[0] ? prog.stage : "Asking Gemini...";
  lcd.setTextColor(prog.cancelled ? WP_DISAB_1 : WP_ACCENT_S, WP_COLOR_0);
  y += drawWrapped(fs, st, AI_MARGIN, y, w, lh, AI_STAGE_ROWS) * lh;
  const uint32_t now = millis();
  const uint32_t secs = (now - prog.startMs) / 1000;
  if (prog.waitUntilMs && (int32_t)(prog.waitUntilMs - now) > 0) {
    snprintf(line, sizeof(line), "%s - %u s - again in %u s", prog.label[0] ? prog.label : "Gemini",
             (unsigned)secs, (unsigned)((prog.waitUntilMs - now + 999) / 1000));
  } else {
    snprintf(line, sizeof(line), "%s - %u s", prog.label[0] ? prog.label : "Gemini", (unsigned)secs);
  }
  lcd.setTextColor(WP_DISAB_1, WP_COLOR_0);
  lcd.drawString(line, AI_MARGIN, y);
  y += lh;
  lcd.setTextColor(GRAY_67, WP_COLOR_0);
  // 215 px of the 228 (Akrobat 16, measured): "(it carries on)" ran off the glass.
  lcd.drawString(prog.cancelled ? "Cancelling..." : "End: cancel  Back: leave (runs on)", AI_MARGIN, y);
}

void AiApp::drawXfer() {
  SmoothFont* f = fonts[AKROBAT_BOLD_18];
  const int top = header->height();
  const int bottom = (int)lcd.height() - (int)footer->height();
  const int lh = f->height() + 2;
  const uint16_t w = (uint16_t)(lcd.width() - 2 * AI_MARGIN);
  lcd.fillRect(0, top, lcd.width(), bottom - top, BLACK);
  lcd.setTextFont(f);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(WHITE, BLACK);
  int y = top + 4;
  lcd.drawString("Uploading into: /API Keys", AI_MARGIN, y); y += lh + 4;
  if (xferOn()) {
    lcd.drawString("On your computer, open:", AI_MARGIN, y); y += lh;
    snprintf(line, sizeof(line), "  http://%s/", xferAddr());
    lcd.drawString(line, AI_MARGIN, y); y += lh;
    lcd.drawString("  (or http://wiphone.local)", AI_MARGIN, y); y += lh + 4;
    if (xferUsingAP()) {
      snprintf(line, sizeof(line), "Join hotspot: %s", xferApName());
      lcd.setTextColor(WP_ACCENT_S, BLACK);
      lcd.drawString(line, AI_MARGIN, y); y += lh;
      // Two rows: as one (278 px in Akrobat 18) the warning lost "the key".
      guiDrawEllipsized(lcd, "It is OPEN: anyone nearby", w, AI_MARGIN, y); y += lh;
      guiDrawEllipsized(lcd, "can read the key.", w, AI_MARGIN, y); y += lh;
      lcd.setTextColor(WHITE, BLACK);
    } else {
      lcd.setTextColor(GRAY_67, BLACK);
      guiDrawEllipsized(lcd, "Use your home WiFi - this", w, AI_MARGIN, y); y += lh;
      guiDrawEllipsized(lcd, "phone's own hotspot is open.", w, AI_MARGIN, y); y += lh;
      lcd.setTextColor(WHITE, BLACK);
    }
    lcd.drawString("Send gemini.txt (key=...).", AI_MARGIN, y); y += lh + 4;
    aiKeyInfo(&key);
    lcd.setTextColor(key.usable ? WP_ACCENT_G : GRAY_67, BLACK);
    lcd.drawString(key.usable ? "Key found - Back to use it." : "Back stops the server.", AI_MARGIN, y);
  } else {
    const char* err = xferStartError();
    lcd.drawString("Server did not start.", AI_MARGIN, y); y += lh;
    guiDrawEllipsized(lcd, err ? err : "Check WiFi and try again.", w, AI_MARGIN, y);
  }
}

// ---- events -------------------------------------------------------------------------------------

appEventResult AiApp::processEvent(EventType event) {
  if (event == APP_TIMER_EVENT) {
    if (appState == AI_XFER) {
      armTimer();
      if (xferFilesAdded() != xferSeen) {
        xferSeen = xferFilesAdded();
        aiKeyReload();                    // a file landed: is it the key?
        return REDRAW_SCREEN;
      }
      return DO_NOTHING;
    }
    if (transcriptStale()) {
      const bool showing = appState == AI_TRANSCRIPT || appState == AI_WAITING;
      if (appState == AI_WAITING && !aiRequestActive()) {
        enterState(AI_TRANSCRIPT);         // the answer (or the reason) is in the chat now
        /* An answer longer than a page opens at its START (the last page is its tail): Down
         * then reads it forward. */
        if (newestStart < page) {
          page = newestStart;
        }
        return REDRAW_ALL;
      }
      buildTranscript();
      toLastPage();
      armTimer();
      return showing ? REDRAW_SCREEN : DO_NOTHING;
    }
    armTimer();
    if (appState == AI_WAITING) {
      aiProgress(&prog);
      const uint32_t secs = (millis() - prog.startMs) / 1000;
      if (prog.gen != progGen || secs != progSec) {
        progGen = prog.gen;
        progSec = secs;
        return REDRAW_SCREEN;
      }
    }
    return DO_NOTHING;
  }
  if (!IS_KEYBOARD(event)) {
    return DO_NOTHING;
  }
  armTimer();                             // a question asked from the console (`ai ask`) is picked up

  switch (appState) {
  case AI_TRANSCRIPT:
  case AI_WAITING:
    if (event == WIPHONE_KEY_END && appState == AI_WAITING) {
      aiCancel();                         // the answer is thrown away when it lands
      enterState(AI_TRANSCRIPT);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;                    // a question in flight carries on (ai_net.h)
    }
    if (event == WIPHONE_KEY_DOWN || event == WIPHONE_KEY_UP) {
      if (!text) {
        return DO_NOTHING;
      }
      BookMeasure m = { fonts[AI_FONT], fontMeasure };
      if (event == WIPHONE_KEY_DOWN) {
        bookLayoutPage(text, textLen, page, viewWidth(), viewLines(), &m, &pg);
        if (pg.next >= textLen) {
          return DO_NOTHING;
        }
        page = pg.next;
      } else {
        if (page == 0) {
          return DO_NOTHING;
        }
        page = bookLayoutPrevPage(text, textLen, page, viewWidth(), viewLines(), &m);
      }
      return REDRAW_SCREEN;
    }
    if (appState == AI_WAITING) {
      return DO_NOTHING;
    }
    if (event == WIPHONE_KEY_SELECT) {
      enterState(AI_MENU);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_OK || event == WIPHONE_KEY_CALL) {
      aiLoopTick();
      if (aiRequestActive()) {
        aiProgress(&prog);
        if (!prog.cancelled) {
          enterState(AI_WAITING);
          return REDRAW_ALL;
        }
        note = "Still cancelling the last question";
        return REDRAW_SCREEN;
      }
      aiKeyReload();
      aiKeyInfo(&key);
      enterState(key.usable ? AI_COMPOSE : AI_KEYINFO);
      return REDRAW_ALL;
    }
    return DO_NOTHING;

  case AI_MENU:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(AI_TRANSCRIPT);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_ASK) {
        aiKeyReload();
        aiKeyInfo(&key);
        enterState(aiRequestActive() ? AI_WAITING : (key.usable ? AI_COMPOSE : AI_KEYINFO));
      } else if (k == ROW_TOPIC) {
        aiChatNewTopic();
        enterState(AI_TRANSCRIPT);
      } else if (k == ROW_CLEAR) {
        enterState(AI_CONFIRM);
      } else if (k == ROW_KEYINFO) {
        enterState(AI_KEYINFO);
      } else if (k == ROW_RETRY) {
        /* Flipped and saved; the menu is built again with the row's new words, the row kept. The
         * question in flight (if any) keeps the setting it was asked with. */
        aiSetRetryTimeouts(!aiRetryTimeouts());
        enterState(AI_MENU);
        if (menu) {
          menu->select(ROW_RETRY);
        }
      } else {
        return DO_NOTHING;                // a note row
      }
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    return REDRAW_SCREEN;

  case AI_CONFIRM:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(AI_MENU);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_YES) {
        aiChatClear();
        enterState(AI_TRANSCRIPT);
      } else if (k == ROW_NO) {
        enterState(AI_MENU);
      } else {
        return DO_NOTHING;
      }
      return REDRAW_ALL;
    }
    if (menu) {
      menu->processEvent(event);
    }
    return REDRAW_SCREEN;

  case AI_COMPOSE:
    if (event == WIPHONE_KEY_END) {       // cancel (Back is backspace in the field)
      enterState(AI_TRANSCRIPT);
      return REDRAW_ALL;
    }
    /* ⚠ SEND, not OK - the D-pad centre commits a multi-tap letter here and must never ask a
     * half-typed question (app_meshtastic.cpp, {user} 2026-08-17). */
    if (LOGIC_BUTTON_SEND(event)) {
      ask();
      return appState == AI_COMPOSE ? REDRAW_SCREEN : REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_OK) {
      return DO_NOTHING;                  // swallowed: no pending letter was left to commit
    }
    if (textArea) {
      textArea->processEvent(event);
      note = NULL;                        // a keystroke answers the note; the counter is back
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case AI_KEYINFO:
    if (LOGIC_BUTTON_BACK(event)) {
      aiKeyReload();
      aiKeyInfo(&key);
      if (!key.usable && (!aiChat() || aiChat()->n == 0)) {
        return EXIT_APP;                  // nothing to show without a key or a chat
      }
      enterState(AI_TRANSCRIPT);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event) && menu) {
      const MenuOption::keyType k = menu->currentKey();
      if (k == ROW_UPLOAD) {
        enterState(AI_XFER);
        return REDRAW_ALL;
      }
      if (k == ROW_RELOAD) {
        enterState(AI_KEYINFO);           // re-reads the card
        return REDRAW_ALL;
      }
      return DO_NOTHING;
    }
    if (menu) {
      menu->processEvent(event);
    }
    return REDRAW_SCREEN;

  case AI_XFER:
    if (LOGIC_BUTTON_BACK(event) || LOGIC_BUTTON_OK(event)) {
      xferStop();
      enterState(AI_KEYINFO);             // re-reads gemini.txt: a key just sent works now
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;
  }
  return DO_NOTHING;
}

void AiApp::redrawScreen(bool redrawAll) {
  switch (appState) {
  case AI_TRANSCRIPT:
    drawTranscript();
    if (note) {
      drawStrip(note, WP_ACCENT_S);
    } else if (text && pg.next < textLen) {
      drawStrip("Down: more   Up: back", GRAY_67);
    } else {
      drawStrip(aiChat() && aiChat()->n ? "OK: ask   Menu: topic, clear, key"
                                        : "OK: ask a question", GRAY_67);
    }
    break;
  case AI_WAITING:
    drawTranscript();
    drawStatus();
    break;
  case AI_COMPOSE:
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
    drawComposeStrip();
    break;
  case AI_XFER:
    drawXfer();
    break;
  default:
    if (menu) {
      ((GUIWidget*)menu)->redraw(lcd);
    }
    break;
  }
}
