/*
 * app_ai.h - Menu > AI: ask Google's Gemini a question from the keypad, on WiFi, with the
 * owner's own free key on the card (README "AI"). The talking is ai_net.cpp's (a worker of its
 * own, TLS with Google's roots pinned); the rules are gemini.cpp's (host-tested).
 *
 * SCREENS (one enterState() door, which also drops any claim on predictive text):
 *   TRANSCRIPT  the saved chat, paged like the Files viewer (bookLayoutPage + bookRenderRun over
 *               ONE PSRAM buffer): "> question" in blue, the answer in white, under it the model
 *               that answered ("Flash") in grey, a grey "New topic" line where a topic starts.
 *               Opens on the newest page. Up/Down page; OK asks; Select = the menu; Back leaves.
 *   MENU        Ask a question / New topic / Clear chat... / Key info / Retry if it times out:
 *               On|Off (OK flips it; NVS - ai_net.h aiRetryTimeouts; on by default).
 *   CONFIRM     Clear chat: "Yes, delete it" / "No".
 *   COMPOSE     the Meshtastic compose screen's rules: a MultilineTextWidget (500 characters),
 *               predictive text claimed AFTER setInputState, the counter strip. Send = CALL or the
 *               left soft key ("Ask"); END cancels; OK is a typing key and is swallowed; Back is
 *               backspace ("Clear"). A refused question stays typed, the reason in the strip.
 *   WAITING     the transcript with the question at the bottom and a status box: the stage
 *               ("Asking Gemini...", "Google is busy - trying again... 2 s"), the model, the
 *               seconds since it was asked. END cancels (the answer is thrown away when it lands);
 *               Back leaves the app and the question carries on - the answer is in the chat when
 *               the app is opened again.
 *   KEYINFO     where the key comes from and what the phone found: the file, "key: set (N chars)",
 *               the model - NEVER the key or any part of it (screenshots get shared). The first
 *               screen when there is no key. "Add key file over WiFi" starts the uploader into
 *               /API Keys (XFER).
 *   XFER        the uploader's address, like Files' upload screen, with the warning that the
 *               phone's own hotspot is OPEN. Back stops it and reads the key again.
 *
 * MEMORY. This runs on the loop task, whose 8 KB stack a Books picture page already takes ~7.6 KB
 * of: no big locals here. The transcript, its styles, the page and the line being drawn are
 * members (the app object is PSRAM: WiPhoneApp's operator new).
 */
#ifndef APP_AI_H
#define APP_AI_H

#include "GUI.h"
#include "book_layout.h"
#include "app_gbc_xfer.h"
#include "ai_net.h"

class AiApp : public WindowedApp {
public:
  AiApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~AiApp();

  ActionID_t getId() {
    return GUI_APP_AI;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll = false);

protected:
  enum AiState { AI_TRANSCRIPT = 0, AI_MENU, AI_CONFIRM, AI_COMPOSE, AI_WAITING, AI_KEYINFO, AI_XFER };
  enum { SEG_QUESTION = 0, SEG_ANSWER, SEG_META };
  struct Seg {
    uint32_t start;           // byte offset where this style begins (sorted)
    uint8_t  style;           // SEG_*
  };
  static const int SEG_MAX = 4 * GEM_CHAT_MAX + 8;

  AiState  appState;
  MenuWidget*          menu;
  MultilineTextWidget* textArea;

  char*    text;              // the transcript (PSRAM)
  size_t   textLen, textCap;
  Seg      segs[SEG_MAX];
  int      nSegs;
  uint32_t page;              // where the shown page starts
  uint32_t newestStart;       // where the newest exchange starts (its divider, or its "> " line)
  uint32_t builtGen;          // the chat generation the transcript was built from
  bool     builtPending;      // ...and whether it had a question in flight at the bottom
  BookPage pg;                // the page being drawn / stepped (a member: ~500 bytes)
  char     line[256];         // one line through bookRenderRun

  const char* note;           // a refused question's reason (strip), or NULL
  char     noteBuf[96];
  AiProgress prog;            // the worker's progress, as last read
  uint32_t progGen;           // ...the record last drawn
  uint32_t progSec;           // ...and its seconds
  AiKeyInfo key;              // what the last read of gemini.txt found (never the key itself)
  int      xferSeen;          // files the uploader had taken when the key was last read

  void     enterState(AiState s);
  void     clearInputClaims();
  void     freeWidgets();
  MenuWidget* newMenu();
  void     buildMenu();
  void     buildConfirm();
  void     buildKeyInfo();
  void     buildCompose();
  void     buildTranscript();
  void     segAdd(uint8_t style);
  void     put(const char* s);
  int      styleAt(uint32_t off) const;
  int      viewLines() const;
  int      viewWidth() const;
  int      viewBottom() const;    // where the text area ends (the strip/box under it)
  void     toLastPage();
  void     armTimer();
  bool     transcriptStale() const;
  void     ask();
  void     drawTranscript();
  void     drawStrip(const char* s, uint16_t color);
  int      drawWrapped(SmoothFont* f, const char* s, int x, int y, uint16_t w, int lh, int maxRows);
  void     drawStatus();
  void     drawComposeStrip();
  void     drawXfer();
};

#endif // APP_AI_H
