/**
 * FloodMesh V4 - SOS state: the sender's retries and the responder's SOS list.
 * docs/architecture.md §7.6, §13.4 and §13.8.
 *
 * Sender (any unit)
 * -----------------
 * An SOS starts an "episode" on one channel. It is sent at once and then
 * retried at most FM_SOS_RETRY_MAX (5) times, after 1, 2, 4, 8 and 15 min,
 * until a responder unit answers with an ACK or the user stops it (decided
 * 28 Sep, §7.6). If the last retry is not answered within FM_SOS_ACK_WAIT_MS
 * the state becomes NOT_DELIVERED and the UI says so; an ACK that still
 * arrives later turns it into DELIVERED. Every retry is a fresh frame with a
 * new msgId; an ACK matches if it names any msgId of the current episode.
 * "Delivered" ends the retries but not the SOS: until a responder sends
 * "Help is coming" (or the user stops it), the unit keeps re-sending every
 * FM_SOS_KEEPALIVE_MS (decided 28 Sep). That keeps the SOS alive if the only
 * responder unit that heard it is switched off or carried away: a responder
 * that already has it just updates its entry, one that arrives later gets it
 * fresh.
 *
 * Responder
 * ---------
 * One entry per SOS sender (call sign). A repeat from the same sender updates
 * the entry; after FM_SOS_EPISODE_MS of silence a new SOS starts a new
 * episode. The responder selects channels; General is always selected. The
 * main list shows the selected channels plus any escalated entry. An entry
 * nobody has answered with "Help coming" for FM_SOS_ESCALATE_MS escalates and
 * shows on every responder unit, whatever its filter. The selection is kept
 * in NVS, so it survives reboot and sleep.
 */
#pragma once
#include <Arduino.h>

#include "fm_alarm.h"
#include "fm_packet.h"

#ifndef FM_SOS_RETRY_MAX
#define FM_SOS_RETRY_MAX 5                             // §7.6: 5 retries, then "not delivered"
#endif
#ifndef FM_SOS_RETRY_MAX_GAP_MS
#define FM_SOS_RETRY_MAX_GAP_MS (15UL * 60UL * 1000UL) // gaps 1, 2, 4, 8, then 15 min
#endif
#ifndef FM_SOS_KEEPALIVE_MS
#define FM_SOS_KEEPALIVE_MS (15UL * 60UL * 1000UL)     // after DELIVERED, until HELP IS COMING
#endif
#ifndef FM_SOS_ACK_WAIT_MS
#define FM_SOS_ACK_WAIT_MS 60000UL                     // after the last retry; > worst ACK round trip
#endif
#ifndef FM_SOS_ESCALATE_MS
#define FM_SOS_ESCALATE_MS (15UL * 60UL * 1000UL)      // §13.8 safeguard 4
#endif
#ifndef FM_SOS_EPISODE_MS
#define FM_SOS_EPISODE_MS (60UL * 60UL * 1000UL)       // silence that ends an episode
#endif
#ifndef FM_SOS_TABLE
#define FM_SOS_TABLE 16
#endif

// ================================================================ sender
enum FmSosState : uint8_t {
  FM_SOS_IDLE = 0,
  FM_SOS_WAITING,      // sent, no answer yet; retrying
  FM_SOS_DELIVERED,    // a responder unit received it; re-sent every 15 min until HELP
  FM_SOS_HELP,         // a responder pressed "Help coming"
  FM_SOS_NOT_DELIVERED, // no answer to the last retry
  FM_SOS_STOPPED,      // the user stopped it
};

void        fmSosStart(uint8_t ch, uint32_t now);   // new episode, due at once
bool        fmSosDue(uint32_t now);                 // time to send (or re-send)
void        fmSosSent(int msgId, uint32_t now);     // msgId < 0: failed, retry soon
/** An ACK addressed to us. Returns true if it answers the current episode. */
bool        fmSosOnAck(uint8_t kind, uint8_t targetMsgId, const char *by);
void        fmSosStop();
FmSosState  fmSosState();
uint8_t     fmSosChannel();
uint8_t     fmSosTries();                           // transmissions so far, first send included
const char *fmSosAnsweredBy();                      // responder call sign, or ""
/** ms until the next send (retry or keep-alive), or UINT32_MAX if none is scheduled. */
uint32_t    fmSosNextInMs(uint32_t now);
bool        fmSosActive();                          // WAITING, DELIVERED or HELP

// ================================================================ responder
struct FmSosEntry {
  bool     used;
  char     cs[FM_CALLSIGN_LEN + 1];   // space padded, as on air
  uint8_t  ch;
  uint8_t  msgId;       // latest SOS frame from this sender
  uint8_t  hops;
  int16_t  rssi;
  float    snr;
  uint32_t firstAt;     // start of the episode
  uint32_t lastAt;
  uint16_t count;       // SOS frames received this episode
  bool     seen;        // shown on this unit's SOS screen
  bool     escalated;
  bool     helpMine;    // this unit sent "Help coming"
  char     helpBy[FM_CALLSIGN_LEN + 1];   // who sent "Help coming", or ""
};

/** Load the channel selection from NVS. Call once from setup(). */
void fmSosBegin();

/**
 * Record an SOS. Returns the entry index (or -1 if the table cannot take it)
 * and sets *isNew for a new episode or a changed channel, which is what should
 * alert the responder.
 */
int  fmSosRxAdd(const char *cs, uint8_t ch, uint8_t msgId, uint8_t hops, float rssi, float snr,
                uint32_t now, bool *isNew);
/** "Help coming" heard (or sent, mine = true) for this SOS sender. */
void fmSosRxHelp(const char *targetCs, const char *byCs, bool mine);
/** Returns the index of an entry that has just escalated, or -1. Call often. */
int  fmSosRxEscalateTick(uint32_t now);

FmSosEntry *fmSosEntryAt(int idx);
/**
 * Entries to show, newest first. tab < 0: the main list (selected channels and
 * escalated entries); otherwise one channel, whatever the selection.
 */
uint8_t fmSosList(int8_t tab, uint8_t *idxOut, uint8_t cap);
/** Unseen entries in a channel the responder has NOT selected (the tab dot). */
bool    fmSosTabDot(uint8_t ch);
/** Entries in the main list that have not been seen yet. */
uint8_t fmSosUnseenVisible();
/** Entries in the main list (for the home screen). */
uint8_t fmSosVisibleCount();

// ---------------------------------------------------------------- selection
bool    fmSosSelected(uint8_t ch);       // General is always selected
/** Add or remove a channel. Returns false for General, which cannot be removed. */
bool    fmSosToggle(uint8_t ch);
uint8_t fmSosSelectionMask();
bool    fmSosEntryVisible(const FmSosEntry &e);
