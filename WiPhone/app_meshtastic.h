/*
 * app_meshtastic.h — WiPhone Meshtastic UI (Phase 4b: conversations)
 *
 * Main menu -> Chats / Nodes / Status. "Chats" lists conversations: a pinned
 * "Main Channel" (broadcast) plus one thread per DM peer, most-recent first.
 * Selecting a chat opens that thread; a thread offers "Write" (compose targeting
 * the thread) and opening any message for a full, word-wrapped, scrollable view.
 */

#ifndef APP_MESHTASTIC_H
#define APP_MESHTASTIC_H

#include "GUI.h"
#include "meshtastic_service.h"   // MESH_NAME_LEN (title buffers)

// Max distinct DM conversations we can list (bounded by the message ring size).
#define MESH_APP_MAX_CHATS  24

class MeshtasticApp : public WindowedApp {
public:
  MeshtasticApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~MeshtasticApp();

  ActionID_t getId() {
    return GUI_APP_MESHTASTIC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  typedef enum {
    MESH_MAIN,
    MESH_CHATS,
    MESH_THREAD,
    MESH_NODES,
    MESH_STATUS,
    MESH_COMPOSE,
    MESH_VIEWMSG,
    MESH_MYNODE,
    MESH_EDITNAME,
    MESH_EDITSHORT,     // the 4-character short name other radios display
    MESH_PLACES,        // waypoints heard from the mesh (camp, the truck, ...)
    MESH_PLACE_OPTS,    // one waypoint: set as reference / declare "I'm here"
    MESH_SUN,           // legal light at the reference place (the serial `sun`, on screen)
    MESH_POSCHAN,       // which channel the GPS position beacon goes to
  } MeshAppState_t;

  MeshAppState_t appState;
  MeshAppState_t returnState;       // where Compose/View return to on Back/Cancel

  MenuWidget*          menu;        // active list widget
  MultilineTextWidget* textArea;    // active text widget (Compose/View)

  // Current thread / compose target.
  bool     threadIsChannel;         // true = channel thread, false = DM thread
  uint8_t  threadChannelHash;       // channel thread's channel hash
  uint32_t threadPeer;              // DM thread's peer node

  int      viewMsgIndex;            // message index shown in the View screen
  int      pendingClear;            // My node: 0 none, 1 messages, 2 nodes (confirm)
  /* Send-position picker: the row key armed for a second press, 0 = none. The
   * PUBLIC channel takes two presses for the same reason "Clear nodes" does —
   * it is irreversible enough to mean it — except what is at stake here is
   * where a person's live location goes, so one stray D-pad press must not be
   * able to change it. */
  int      pendingPubRow;
  uint32_t selectedWaypointId;      // Places: the waypoint whose options are open
  /* Header titles are STORED POINTERS (HeaderWidget::setTitle never copies), so
   * they must point at memory that outlives the screen — never at a stack local
   * and never into the service's mutable tables. */
  char     placeTitle[20];          // MESH_PLACE_OPTS title (waypoint name copy)
  char     threadTitle[MESH_NAME_LEN];   // MESH_THREAD title (peer label copy)

  /* Compose: the cap the field was built with (meshComposeCap, per thread type) and the
   * reason the last Send was refused — a static literal from the service, NULL when the
   * strip should show the "N/MAX" counter instead. See drawComposeStrip(). */
  uint16_t    composeCap;
  const char* composeNote;

  // Chats-list entries: a channel (isChannel true, id = channel hash) or a DM
  // peer (isChannel false, id = peer node number). Row key = index + 1.
  bool     chatIsChannel[MESH_APP_MAX_CHATS];
  uint32_t chatId[MESH_APP_MAX_CHATS];
  int      chatCount;

  void freeWidgets();
  void clearInputClaims();      // predictive text belongs to Compose; see the .cpp
  /* ⚠ Delete JUST the menu, for an in-place rebuild. Every build*() starts with
   * `menu = newMenu(...)` and, until this existed, simply dropped the previous
   * one on the floor: freeWidgets() is called only from the destructor and
   * enterState(). MenuWidget itself is PSRAM (AbstractWidget::operator new ->
   * ps_malloc), but MenuOption does NOT derive from AbstractWidget, so every
   * row leaked a plain-new object plus its strdup'd title on the INTERNAL heap
   * — the ~25 KB one with a ~20 KB largest block. Estimated 600-900 bytes per
   * press; CONFIRMED by reading, UNKNOWN: not measured on hardware. It matters
   * now because a cycling interval row is a row people tap repeatedly.
   * Not freeWidgets(): that also deletes textArea, which is always NULL on the
   * menu screens but need not stay that way. */
  void freeMenu();
  void enterState(MeshAppState_t state);

  MenuWidget* newMenu(const char* emptyMessage);
  void buildMainMenu();
  void buildChats();
  void buildThread();
  void buildNodes();
  void buildStatus();
  void buildCompose();
  void drawComposeStrip();      // the counter / "Not sent" band above the footer
  void buildViewMessage(int msgIndex);
  void buildMyNode();
  void buildEditName();
  void buildEditShortName();
  void buildPlaces();
  void buildPlaceOpts();
  void buildSun();
  void buildPosChannel();
};

#endif // APP_MESHTASTIC_H
