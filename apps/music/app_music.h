/*
 * app_music.h — the music player's screen.
 *
 * A view onto musicPlayerXxx(), which owns the library, the queue and whatever is
 * playing. This file owns only what a person sees and presses, which is why backing out
 * of it does not stop the music: there is nothing here to stop.
 *
 * Library -> Now playing, plus "Add music over WiFi" on the shared transfer server and a
 * menu for shuffle and repeat.
 *
 * ⚠ Unlike the reader, this does NOT hold the screen awake. A book needs the screen on
 * because reading it is the point; music does not, and holding a phone's screen on for
 * the length of an album is how you arrive somewhere with a flat battery.
 */

#ifndef APP_MUSIC_H
#define APP_MUSIC_H

#include "GUI.h"
#include "music_player.h"

class MusicApp : public WindowedApp {
public:
  MusicApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~MusicApp();

  ActionID_t getId() {
    return GUI_APP_MUSIC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  typedef enum {
    MUSIC_LIB,        // the track list
    MUSIC_NOW,        // now playing, with the transport keys
    MUSIC_XFER,       // "Add music over WiFi" (the shared transfer server)
    MUSIC_MENU,       // shuffle, repeat, stop
  } MusicState_t;

  MusicState_t appState;
  MenuWidget*  menu;
  bool         xferClean;      // transfer screen already painted (the 1 Hz refresh overdraws)
  bool         nowClean;       // same, for the now-playing screen
  int          titleShown;     // track whose title is on the panel (-1 none playing, -2 nothing drawn)
  char         headerTitle[64];  // ⚠ HeaderWidget::setTitle KEEPS THE POINTER, it does not copy

  void freeWidgets();
  void enterState(MusicState_t state);
  MenuWidget* newMenu(const char* emptyMessage, SmoothFont* font, uint8_t perScreen);

  void buildLibrary();
  void buildMenu();
  void drawNowPlaying();
  void drawXfer();
};

#endif // APP_MUSIC_H
