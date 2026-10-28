/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/


#include "Storage.h"
#include "config.h"
#include "helpers.h"
#include "sms_mirror.h"   // ingestMirrored(): the CSM1 record and smsMirrorDigits()

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  PHONEBOOK CLASS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

void Phonebook::parse(char* str) {
  // TODO
  if (str!=NULL) {
    freeNull((void **) &phonebookDyn);
    phonebookDyn = str;
    phonebookLen = strlen(str);
  }
}

bool Phonebook::next() {
  if (phonebookOff >= phonebookLen) {
    return false;
  }
  if (!phonebookDyn) {
    return false;
  }
  phonebookOff++;
  while (phonebookOff+1 < phonebookLen && !(phonebookDyn[phonebookOff-1]=='\n' && phonebookDyn[phonebookOff]=='n' && phonebookDyn[phonebookOff+1]=='=')) {
    phonebookOff++;
  }
  if (phonebookOff+1 >= phonebookLen) {
    phonebookOff = phonebookLen;
    return false;
  }
  return true;
}

bool Phonebook::find(unsigned int key) {
  phonebookOff = 0;
  unsigned int pos = 1;
  while (pos < key) {
    if (!this->next()) {
      break;
    }
    pos++;
  }
  return pos == key;
}

PhonebookRecord* Phonebook::nextRecord() {
  if (phonebookDyn) {
    int skip = 0;
    PhonebookRecord* r = new PhonebookRecord();
    if (r) {
      r->parse(phonebookDyn + phonebookOff, skip);
      if (r->length()>0 && skip>0) {
        phonebookOff += skip;
        return r;
      }
      delete r;
      r = NULL;
    }
  }
  return NULL;
}

PhonebookRecord* Phonebook::firstRecord() {
  phonebookOff = 0;
  return this->nextRecord();
}

PhonebookRecord* Phonebook::findRecord(unsigned int key) {
  if (this->find(key)) {
    return this->nextRecord();
  }
  return NULL;
}

// TODO: remove, use NanoINI
bool Phonebook::reformat() {
  // Backwards compatibility reformatting
  if (1 || (phonebookDyn && strstr(phonebookDyn, "name="))) {      // simplistic rule
    // Phonebook format needs to be updated
    char* d = (char *) extMalloc(phonebookLen+1);       // allocate new copy of phonebook
    if (d) {
      // Parse and serialize all the records into the new space
      int len = 0;
      PhonebookRecord* rec = this->firstRecord();
      while (rec) {
        if (len + rec->length() <= phonebookLen) {
          len += rec->serialize(d+len);
          d[len] = '\0';      // terminate after the newline
          delete rec;
          rec = this->nextRecord();
        } else {
          // error: phonebook did not shrink
          break;
        }
      }
      if (!rec && len) {
        // Update size of d
        d = (char*) extRealloc(d, len + 1);
        if (d) {
          log_d("phonebook reformatted: %d -> %d", phonebookLen, len);
          free(phonebookDyn);
          phonebookDyn = d;
          phonebookLen = len;
          d = NULL;
        }
      } else {
        // free memory after error
        delete rec;
      }
      freeNull((void **) &d);
    }
    return true;
  }
  return false;
}

unsigned int Phonebook::addRecord(PhonebookRecord* newRec) {
  // Add serialized phonebook record, place in alphabetic order
  unsigned int newPos = 0;
  unsigned int len = newRec->length();
  if (len) {
    // Extend current phonebook
    char* p = (char *) extRealloc(phonebookDyn, phonebookLen + 1 + len);
    if (p) {
      // Find position to insert phonebook record
      int lastOff = 0;
      phonebookDyn = p;
      unsigned int pos = 1;
      PhonebookRecord* rec = this->firstRecord();
      while (rec && *rec <= *newRec) {
        lastOff = phonebookOff;
        rec = this->nextRecord();
        pos++;
      }
      // Actually insert
      if (rec) {
        // record at lastOff is "greater" than newRec
        // - move contents from lastOff by `len` bytes forward
        for (int k = phonebookLen+len; k>=lastOff+len; k--) {
          phonebookDyn[k] = phonebookDyn[k-len];
        }
        // - serialize into the middle of phonebook (no need to NUL-terminate)
        if (newRec->serialize(phonebookDyn + lastOff) == len) {
          phonebookLen += len;
          newPos = pos;
        }
      } else {
        // insert new record at the end of phonebook
        if (newRec->serialize(phonebookDyn + phonebookLen) == len) {
          phonebookLen += len;
          phonebookDyn[phonebookLen] = '\0';
          newPos = pos;
        }
      }
    }
  }
  return newPos;
}

unsigned int Phonebook::addRecord(const char* name, const char* sip) {
  unsigned int newPos = 0;
  PhonebookRecord* rec = new PhonebookRecord(name, sip);
  if (rec) {
    newPos = addRecord(rec);
    delete rec;
  }
  return newPos;
}

bool Phonebook::removeRecord(unsigned int key) {
  if (!phonebookLen || !phonebookOff) {
    return false;  // TODO: why phonebookOff here?
  }

  // Allocate memory for a new copy of phonebook      (TODO: no necessity to make a new copy)
  char* copy = (char*) extMalloc(phonebookLen+1);
  if (!copy) {
    return false;
  }

  // Find the record by position/key (1..N)
  bool succ = false;
  unsigned int pos = 1;
  phonebookOff = 0;
  while (pos < key) {
    if (!this->next()) {
      break;
    }
    pos++;
  }

  // Found?
  if (pos == key) {

    // Copy the part before record
    unsigned int newLen = phonebookOff;
    if (newLen) {
      strncpy(copy, phonebookDyn, newLen);
    }

    // Skip the record
    unsigned int off0 = phonebookOff;
    if (this->next()) {
      // Copy the part after the record
      strncpy(copy + newLen, phonebookDyn + phonebookOff, phonebookLen - phonebookOff);
      newLen += phonebookLen - phonebookOff;
    }

    // Terminate string
    copy[newLen] = '\0';

    // Shrink memory usage
    char* p = (char *) extRealloc(copy, newLen+1);
    if (p) {
      // Replace phonebook
      phonebookDyn = p;
      phonebookLen = newLen;
      copy = NULL;

      succ = true;
    }
  }

  freeNull((void **) &copy);
  return succ;
}

unsigned int Phonebook::replaceRecord(unsigned int pos, const char* name, const char* sip) {
  if (!phonebookDyn || !phonebookLen || !pos || (!name && !sip)) {
    return 0;
  }

  // Make a "backup" copy of a phonebook
  char* backup = (char *) extMalloc(phonebookLen+1);
  int backupLen = phonebookLen;
  if (!backup) {
    return false;
  }
  strncpy(backup, phonebookDyn, phonebookLen);

  // Remove & insert
  unsigned int newPos = 0;
  if (this->removeRecord(pos)) {
    this->show();
    newPos = this->addRecord(name, sip);
  }

  // Remove second copy of phonebook
  if (newPos > 0) {
    freeNull((void **) &backup);
  } else {
    // error: restoring original phonebook value
    freeNull((void **) &phonebookDyn);
    phonebookDyn = backup;
    phonebookLen = backupLen;
  }
  return newPos;
}

void Phonebook::show() {
#ifdef DEBUG_MODE
  log_d("phonebookLen = %d, strlen = %d", phonebookLen, phonebookLen ? strlen(phonebookDyn) : -1);
  log_d("PHONEBOOK:");
  if (phonebookLen > 0) {
    bool succ;
    uint32_t last = phonebookOff = 0;
    do {
      succ = this->next();
      char* s = strndup(phonebookDyn + last, phonebookOff-last);
      log_d("%s", s);
      free(s);
      last = phonebookOff;
    } while (succ);
  }
#endif
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  MESSAGES CLASS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

MessageData::MessageData(NanoIni::Section& message) : NanoIni::Section(message) {}

MessageData::MessageData(NanoIni::Section& message, int partn) : NanoIni::Section(message) {
  (*this)["p"] = partn;
}

MessageData::MessageData(const char* fromUri, const char* toUri, const char* text, uint32_t time, bool incoming)
  : NanoIni::Section() {
  // NOTE: similar code is in Messages::saveMessage
  this->putValueFullHex("t", time);
  if (fromUri) {
    (*this)[incoming ? "o" : "s"] = fromUri;
  }
  if (toUri) {
    (*this)[incoming ? "s" : "o"] = toUri;
  }
  if (text) {
    if (NanoIni::isSafeString(text)) {
      (*this)["m"] = text;
    } else {
      this->putValueBase64("b", text);
    }
  }
}

const char* MessageData::getMessageText() {
  if (this->decodedText.size()) {
    return this->decodedText.c_str();
  } else if (this->hasKey("m")) {
    return (*this)["m"];
  } else if (this->hasKey("b")) {
    this->decodedText = this->getValueBase64("b", "");
    return this->decodedText.c_str();
  }
  return "";
}

unsigned long MessageData::getTime() {
  if (!this->time) {
    this->time = this->getHexValueSafe("t", 1);  // 1 - to avoid repeating unhexing
  }
  return this->time;
}

unsigned long MessageData::getAckTime() {
  if (!this->ackTime) {
    this->ackTime = this->getHexValueSafe("a", 1);  // "a" is the ack time; "t" is the message time
  }
  return this->ackTime;
}

Messages::Messages()
  : index(indexFile) {
  preloadedRangeStart = 0;
  preloadedRangeEnd = 0;
};

/* An 8-hex-digit stamp as the store writes it (putValueFullHex). ⚠ NOT getHexValueSafe():
 * that is strtol() into this chip's 32-bit long, which SATURATES at 0x7fffffff — every stamp
 * after 2038-01-19, including the 2079 a replayed mesh time can set (clock_source.h), would
 * read back as 2038 and be "corrected" from the wrong number. */
static bool msgHexStamp(const char* s, uint32_t* out) {
  if (!s || !*s) {
    return false;
  }
  char* end = NULL;
  const unsigned long v = strtoul(s, &end, 16);
  if (!end || *end != '\0') {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

bool Messages::load(const ClockMsgStamp& st) {
  // TODO: make sure not to mess things up with unixTime if it is zero
  const uint32_t unixTime = st.utc;               // UTC; 0 = unknown (see Storage.h)

  // Load index file
  this->loaded = false;
  if (index.load() && !index.isEmpty()) {
    // Check version of the file format
    if (index[0].hasKey("v") && !strcmp(index[0]["v"], "1")) {
      log_v("Messages index found");
      IF_LOG(VERBOSE)
      index.show();
      this->loaded = true;
    } else {
      log_e("Messages index file corrup or unknown format");
      IF_LOG(VERBOSE)
      index.show();
      return false;
    }
  } else {
    log_d("creating Messages index file");
    index[0]["desc"] = "WiPhone messages index";
    index[0]["v"] = "1";
    index.store();
    this->loaded = true;
  }

  // Check correctness

  bool removePart = false;
  bool indexUpdated = false;
  bool defDirToggle = false;      // used to initialize empty partitions
  for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {       // traverse all partitions

    // TODO: mark old empty partitions for deletion

    // Ensure that all partitions have message count
    int32_t n = ipart->getIntValueSafe("n", -1);
    if (n < 0) {
      // No message count: load the partition file
      IniFile ini;
      if (this->loadPartition(ini, ipart->getIntValueSafe("p", -1)) && !ini.isEmpty()) {
        // Updage message count in the index
        n = ini.nSections() - 1;
        (*ipart)["n"] = n;
        log_v("messages found: %d, partition: %s", n, (*ipart).getValueSafe("p", ""));
        ini.store();
        indexUpdated = true;
      } else {
        if (!ini.isLoaded()) {
          log_e("partition %s not found", ipart->getValueSafe("p", "\"\""));
          (*ipart)["remove"] = "1";   // TODO
          removePart = true;
        } else {
          log_e("partition %s is empty", ipart->getValueSafe("p", "\"\""));
        }
      }
    }

    // Ensure that all partitions have time fields in the index
    bool markedTm = false;
    if (!ipart->hasKey("t1") || !ipart->hasKey("t2")) {
      if (ipart->hasKey("t2")) {
        log_d("t2 present, but not t1");
      } else if (ipart->hasKey("t2")) {
        log_d("t1 present, but not t2");
      } else {
        log_d("t1 & t2 absent");
      }
      // Load partition file, find min/max time, update
      IniFile ini;
      if (this->loadPartition(ini, ipart->getIntValueSafe("p", -1)) && ini.nSections() > 1) {
        if (!(ini[0].hasKey("t1") && ini[0].hasKey("t2"))) {
          // Traverse all messages to find min/max time
          const char* t1 = NULL;
          const char* t2 = NULL;
          for (auto im = ini.iterator(1); im.valid(); ++im) {
            const char* t = im->getValueSafe("t", NULL);
            if (!t) {
              // this message has no time -> set to current minute
              im->putValueFullHex("t", unixTime);
              if (unixTime && st.meshId) {
                im->putValueFullHex("tm", st.meshId);     // a mesh clock's: provisional (SA-3)
                markedTm = true;
              }
              t = im->getValueSafe("t", NULL);
            } else if (strlen(t)<8) {
              // this value must have been modified by a human -> align
              char buff[9];
              memset(buff, '0', sizeof(buff)-1);
              buff[8] = '\0';
              strcpy(buff + 8 - strlen(t), t);
              (*im)["t"] = buff;
            }
            // Find minimum
            t1 = (t1 && strncasecmp(t, t1, 8) < 0 || !t1) ? t : t1;
            // Find maximum
            t2 = (t2 && strncasecmp(t, t2, 8) > 0 || !t2) ? t : t2;
          }
          // Update partition time range
          if (t1 && t2) {
            ini[0]["t1"] = t1;
            ini[0]["t2"] = t2;
            ini.store();
          }
        }
      } else if (ini.isLoaded()) {
        // if partition file is empty for some reason (should not happen) -> assign it current time (as it must be a "lost" new partition)
        if (ini.isEmpty() || !ini[0].hasKey("v") && !ini[0].hasKey("desc")) {
          // if we are here, something is either terribly wrong and/or files were edited by a user
          if (ini.isEmpty()) {
            ini.addSection();
          } else {
            ini[0].remove(nullptr);  // remove values with empty key
          }
          log_e("initializing partiton");
          ini[0]["desc"] = "WiPhone messages partition";
          ini[0]["v"] = "1";
          ini[0]["d"] = ipart->getValueSafe("d", (defDirToggle = !defDirToggle) ? "i" : "o");
        }
        log_e("partiton file has no messages");
        ini[0].putValueFullHex("t1", unixTime);
        ini[0]["t2"] = ini[0]["t1"];
        if (!ini[0].hasKey("p") && ipart->hasKey("p")) {
          ini[0]["p"] = (*ipart)["p"];
        }
        ini.store();
      } else {
        // Partition file does not exist and shoud be deleted from the index
        (*ipart)["remove"] = "1";
        removePart = true;
      }

      // Update index
      log_d("updating index");
      (*ipart)["t1"] = ini[0].getValueSafe("t1", "386d4380");      // default = 2000-01-01
      (*ipart)["t2"] = ini[0].getValueSafe("t2", "386d4380");
      if (markedTm) {
        (*ipart)["tm"] = "1";                     // holds provisional stamps (SA-3, below)
      }
      indexUpdated = true;
    }
  }
  /* One descending band of repair stamps ending at `unixTime`, shared by the two passes below
   * so no stamp is handed out twice (see the sentinel pass for why that matters). The sentinel
   * pass takes its TOP — those are THIS boot's texts — and the finalise pass's re-stamps the
   * band under it: an EARLIER boot's texts, which are older (clock_source.h). So the sentinel
   * pass runs first; the two touch different keys, so neither can undo the other. */
  uint32_t bandEnd = unixTime;

  /* ⚠ REPAIR THE UNKNOWN-TIME SENTINELS, now that the clock is known.
   *
   * saveMessage() stores 0xFFFFFFFF for a message whose arrival time was unknown — every
   * text that lands between boot and the first NTP answer. The sentinel keeps partition
   * SELECTION correct (an unknown time must count as newest), but as a DISPLAY order it is
   * actively wrong: maximum uint32 sorts as the newest message in the thread FOREVER, so
   * everything arriving later draws above it — "a new text lands mid-thread".
   *
   * This runs only when `unixTime` is real, which in practice is the reloadMessages() call
   * the main loop already makes on the FIRST NTP sync of a session. It is cheap to target:
   * saveMessage() maintains the index's per-partition t2 as a running max, so a partition
   * contains a sentinel if and only if its t2 IS the sentinel.
   *
   * Order among the repaired: within a partition, equal "ffffffff" keys sit in ARRIVAL
   * order (reorderLast() inserts a new section after its equals), so stamping ascending
   * times in file order preserves the order the texts actually arrived in. The stamps are
   * DISTINCT so no downstream sort ever has to break this tie again. The time shown is the
   * sync moment rather than the true arrival — the honest best available on a board with
   * no RTC, and hours closer than "pinned newest forever".
   *
   * UNDER A MESH CLOCK (0.9.79) this still runs — a woods trip's texts must not sit pinned
   * newest until WiFi — but every stamp it writes is PROVISIONAL ("tm", the pass below), and
   * the loop's forced reload stays armed for NTP/GPS (WiPhone.ino), which finalises them. */
  if (unixTime) {
    /* One descending band of stamps across ALL repaired partitions. Incoming and outgoing
     * live in separate partitions, and giving each its own band ending at unixTime would
     * hand the same stamps out twice — a boot-era exchange (received, replied) could then
     * interleave wrongly forever. Bands are assigned in index-scan order: arbitrary across
     * directions (nothing on disk records the true interleave), but distinct and stable.
     * (`bandEnd` is declared above; the finalise pass below takes the band under this one.) */
    for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {
      if (strcasecmp(ipart->getValueSafe("t2", ""), "ffffffff")) {
        continue;                        // running max isn't the sentinel: none inside
      }
      IniFile ini;
      if (!this->loadPartition(ini, ipart->getIntValueSafe("p", -1)) || ini.nSections() < 2) {
        continue;
      }
      int32_t k = 0;
      for (auto im = ini.iterator(1); im.valid(); ++im) {
        if (!strcasecmp(im->getValueSafe("t", ""), "ffffffff")) {
          k++;
        }
      }
      if (k > 0) {
        uint32_t stamp = bandEnd - (uint32_t)(k - 1);     // oldest arrival, earliest stamp
        for (auto im = ini.iterator(1); im.valid(); ++im) {
          if (!strcasecmp(im->getValueSafe("t", ""), "ffffffff")) {
            im->putValueFullHex("t", stamp++);
            if (st.meshId) {
              im->putValueFullHex("tm", st.meshId);       // a mesh clock's: provisional (SA-3)
            }
          }
        }
        bandEnd -= (uint32_t)k;                           // next partition: older band
        /* ⚠ This re-sort is qsort under messageCompare, which still ties on equal times —
         * a one-time shuffle of any equal-time NON-sentinel pair in this partition is
         * accepted here: their on-disk order was arbitrary to begin with, the shuffle
         * happens once, and display order is decided by sip_threads' deterministic
         * comparator, not file order. The repaired stamps themselves are distinct, so the
         * sentinels land exactly where they should regardless of stability. */
        ini.sortFrom(1, Storage::messageCompare);
        // Recompute the partition's time range from what is now actually in it
        const char* t1 = NULL;
        const char* t2 = NULL;
        for (auto im = ini.iterator(1); im.valid(); ++im) {
          const char* t = im->getValueSafe("t", NULL);
          if (!t) {
            continue;
          }
          t1 = (!t1 || strncasecmp(t, t1, 8) < 0) ? t : t1;
          t2 = (!t2 || strncasecmp(t, t2, 8) > 0) ? t : t2;
        }
        if (t1 && t2) {
          ini[0]["t1"] = t1;
          ini[0]["t2"] = t2;
        }
        /* ⚠ THE INDEX FOLLOWS THE FILE, never leads it. If the partition write fails
         * (full SPIFFS, pulled card), updating the index's t2 anyway would clear the
         * one flag that makes this repair run — the sentinels would stay on disk and
         * never be repaired again. Leave t2 at ffffffff so the next load retries. */
        if (ini.store()) {
          if (t1 && t2) {
            (*ipart)["t1"] = t1;
            (*ipart)["t2"] = t2;
            indexUpdated = true;
          }
          if (st.meshId) {
            (*ipart)["tm"] = "1";                         // the provisional pass finds it here
            indexUpdated = true;
          }
          log_e("MSG: repaired %d unknown-time message(s) in partition %s%s",
                (int)k, ipart->getValueSafe("p", "?"),
                st.meshId ? " - PROVISIONAL (mesh clock): NTP/GPS will finalise them" : "");
        } else {
          log_e("MSG: repair of partition %s did NOT store - will retry next load",
                ipart->getValueSafe("p", "?"));
        }
      } else {
        /* No sentinels in the file but the index says ffffffff: the index is stale (for
         * example, power was lost between the partition store and the index store on a
         * previous repair). Heal it here, or this partition gets re-parsed on every load
         * forever. The file itself is fine — only the index range needs recomputing. */
        const char* t1 = NULL;
        const char* t2 = NULL;
        bool anyTm = false;
        for (auto im = ini.iterator(1); im.valid(); ++im) {
          anyTm = anyTm || im->getValueSafe("tm", NULL) != NULL;
          const char* t = im->getValueSafe("t", NULL);
          if (!t) {
            continue;
          }
          t1 = (!t1 || strncasecmp(t, t1, 8) < 0) ? t : t1;
          t2 = (!t2 || strncasecmp(t, t2, 8) > 0) ? t : t2;
        }
        if (t1 && t2) {
          (*ipart)["t1"] = t1;
          (*ipart)["t2"] = t2;
          /* The same lost index write can have lost a mesh-clock repair's "tm" row: without it
           * the provisional pass would never look in this partition again (SA-3). */
          if (anyTm) {
            (*ipart)["tm"] = "1";
          }
          indexUpdated = true;
          log_e("MSG: healed stale sentinel range on partition %s",
                ipart->getValueSafe("p", "?"));
        }
      }
    }
  }

  /* ⚠ FINALISE THE PROVISIONAL STAMPS — a MESH clock's — now that NTP or GPS is here (review
   * SA-3; the rules and the reasons are clock_source.h, "THE SIP MESSAGE STORE UNDER A MESH
   * CLOCK", host-tested in test_clocksrc).
   *
   * A stamp this store took from a mesh clock carries "tm" = that mesh set's id, and its
   * partition's index row carries "tm" = 1, so only those partitions are ever re-read — the
   * same trick as t2 == ffffffff for sentinels above. Under a trusted clock each marked stamp
   * is moved by the offset NTP/GPS found when it replaced that mesh clock (this boot), kept
   * (an earlier boot's, in the past), or — in the future with no offset to fix it — re-stamped
   * like an unknown time, in the band BELOW the sentinel pass's (above: an earlier boot's texts
   * are older than this boot's). The mark goes either way: the stamp is final.
   *
   * 🛑 THE RE-STAMP KEEPS THE ORDER THE TEXTS CAME IN, which is NOT "ascending in file order"
   * (the sentinel pass's rule, right there only because every sentinel is the same ffffffff and
   * so sits in arrival order). These are DISTINCT stamps in a NEWEST-FIRST file: ascending in
   * file order handed the newest text the earliest stamp, and — the mark gone — reversed that
   * conversation for good (the misc repair round). clockMsgRestamp() ranks them instead:
   * host-tested against the store's own insertion rule in test_clocksrc.
   *
   * 🛑 NEVER UNDER A MESH CLOCK (st.meshId != 0): that is the clock these stamps may be wrong
   * by. A mesh load leaves them marked and waits.
   * ⚠ THE INDEX FOLLOWS THE FILE, as in the sentinel pass: the row's "tm" goes only once the
   * partition stored, so a failed write is retried by the next load (with the same offset —
   * the file still holds the provisional numbers — so nothing is moved twice). */
  if (unixTime && !st.meshId) {
    for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {
      if (!ipart->hasKey("tm")) {
        continue;
      }
      IniFile ini;
      if (!this->loadPartition(ini, ipart->getIntValueSafe("p", -1))) {
        continue;                        // retried by the next load
      }
      int32_t moved = 0, kept = 0, restamped = 0;
      for (auto im = ini.iterator(1); im.valid(); ++im) {
        const char* tm = im->getValueSafe("tm", NULL);
        if (!tm) {
          continue;
        }
        uint32_t t = 0, id = 0, fin = 0;
        msgHexStamp(im->getValueSafe("t", NULL), &t);   // unreadable: 0, which is not final
        msgHexStamp(tm, &id);                           // unreadable: 0, which matches nothing
        if (clockMsgFinal(t, id, &st, &fin)) {
          if (fin != t) {
            im->putValueFullHex("t", fin);
            moved++;
          } else {
            kept++;
          }
          im->remove("tm");
        } else {
          restamped++;                   // still marked: stamped just below
        }
      }
      if (restamped > 0) {
        /* prov[] then out[], in file order: at most a partition's worth (100 texts, 800 B), and
         * PSRAM first — internal heap is the one that panics. No memory: nothing is stored, so
         * the next load does all of this again, with the same offset. */
        uint32_t* prov = (uint32_t*)extMalloc(2 * (size_t)restamped * sizeof(uint32_t));
        if (!prov) {
          log_e("MSG: finalising partition %s: no memory for %d re-stamp(s) - will retry next load",
                ipart->getValueSafe("p", "?"), (int)restamped);
          continue;
        }
        uint32_t* out = prov + restamped;
        int32_t i = 0;
        for (auto im = ini.iterator(1); im.valid() && i < restamped; ++im) {
          if (im->getValueSafe("tm", NULL)) {
            prov[i] = 0;                                   // unreadable: the oldest of them
            msgHexStamp(im->getValueSafe("t", NULL), &prov[i]);
            i++;
          }
        }
        clockMsgRestamp(prov, (uint32_t)i, bandEnd, out);
        const int32_t got = i;
        i = 0;
        for (auto im = ini.iterator(1); im.valid() && i < got; ++im) {
          if (im->getValueSafe("tm", NULL)) {
            im->putValueFullHex("t", out[i++]);
            im->remove("tm");
          }
        }
        free(prov);
        bandEnd -= (uint32_t)got;
      }
      if (moved + kept + restamped == 0) {
        ipart->remove("tm");             // a stale row (nothing marked in the file): heal it
        indexUpdated = true;
        continue;
      }
      if (moved + restamped > 0) {
        ini.sortFrom(1, Storage::messageCompare);
      }
      const char* t1 = NULL;
      const char* t2 = NULL;
      for (auto im = ini.iterator(1); im.valid(); ++im) {
        const char* t = im->getValueSafe("t", NULL);
        if (!t) {
          continue;
        }
        t1 = (!t1 || strncasecmp(t, t1, 8) < 0) ? t : t1;
        t2 = (!t2 || strncasecmp(t, t2, 8) > 0) ? t : t2;
      }
      if (t1 && t2) {
        ini[0]["t1"] = t1;
        ini[0]["t2"] = t2;
      }
      if (ini.store()) {
        if (t1 && t2) {
          (*ipart)["t1"] = t1;
          (*ipart)["t2"] = t2;
        }
        ipart->remove("tm");
        indexUpdated = true;
        log_e("MSG: finalised %d mesh-clock stamp(s) in partition %s: %d moved %+lds, %d kept, "
              "%d re-stamped", (int)(moved + kept + restamped), ipart->getValueSafe("p", "?"),
              (int)moved, (long)st.corrS, (int)kept, (int)restamped);
      } else {
        log_e("MSG: finalising partition %s did NOT store - will retry next load",
              ipart->getValueSafe("p", "?"));
      }
    }
  }

  if (indexUpdated || removePart) {
    if (removePart) {
      for (int32_t i = 1; i < index.nSections();) {       // traverse all partitions
        if (index[i].hasKey("remove")) {
          log_e("removing partition %d from index", i);
          index.removeSection(i);
        } else {
          i++;
        }
      }
    }
    index.store();
  }

  // Sort partitions by time in descending order
  log_v("sorting index");
  index.sortFrom(1, Storage::messagePartitionCompare);

  IF_LOG(VERBOSE)
  index.show();

  return true;
}

/* Description:
 *     clear messages data from cache (and, as a consequence, free memory currently occupied by cache)
 */
void Messages::unload() {
  this->clearPreloaded();
  this->index.unload();
  if (this->part1.isLoaded()) {
    this->part1.unload();
  }
  if (this->part2.isLoaded()) {
    this->part2.unload();
  }
}

int32_t Messages::inboxTotalSize() {
  return this->countAll(INCOMING);
}

int32_t Messages::sentTotalSize() {
  return this->countAll(SENT);
}

/*
 * Description:
 *     Return current count of incoming/sent messages.
 *     If no such number is recorded in the database, parse partition files one-by-one if necessary.
 */
int32_t Messages::countAll(bool incoming) {
  int32_t cnt = 0;
  bool indexChange = false;
  for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {
    if (!ipart->hasKey("d")) {
      log_e("partition description without `d` key %d", (int32_t)ipart);
      continue;
    }
    if (strcmp(ipart->getValueSafe("d",""), incoming ? "i" : "o")) {
      continue;
    }
    int32_t n = ipart->getIntValueSafe("n", -1);
    if (n>=0) {
      cnt += n;  // avoid negative count at all cost
    }
  }
  return cnt;
}

void Messages::clearPreloaded() {
  log_v("clearing preloaded");
  for (auto it = preloaded.iterator(); it.valid(); ++it)
    if (*it != nullptr) {
      delete (*it);
    }
  preloaded.clear();      // frees memory
  this->preloadedRangeStart = this->preloadedRangeEnd = 0;
}

/*
 * Description:
 *     Resolve up to two partitions and make sure `count` of messages from position `offset` are pre-loaded
 *     to an in-memory NanoIni structure (`MessagesArray preloaded`).
 * Parameters:
 *     This method accepts negative offsets (in Python style).
 * Return:
 *     Number of messages actually preloaded (in addition to those, that happened to be preloaded already).
 */
int32_t Messages::preload(bool incoming, int32_t offset, int32_t count) {
  log_i("incoming? %d / offset: %d / count: %d / range: %d..%d", incoming, offset, count, preloadedRangeStart, preloadedRangeEnd);

  if (incoming != preloadedIncoming || preloadedRangeStart == preloadedRangeEnd ||
      ((offset >= 0) ? (offset < preloadedRangeStart || offset > preloadedRangeEnd) : (offset > preloadedRangeStart || offset < preloadedRangeEnd))) {
    // Range needs to be reloaded entirely
    this->clearPreloaded();
    this->preloadedRangeStart = this->preloadedRangeEnd = offset;
    this->preloadedIncoming = incoming;
  } else {
    // Partially loaded already?
    int32_t diff = this->preloadedRangeEnd - offset;
    offset += diff;
    if (offset < 0) {
      diff = -diff;
    }
    count -= diff;
    if (count <= 0) {
      log_v("range fully loaded already");
      return 0;   // fully loaded already
    }
  }

  // Load needed partitions
  if (offset >= 0) {
    // currently this assumes that offset is negative (we are counting from the end)
    log_e("positive offsets not implemented");
    return 0;
  }
  int32_t skip = -offset - 1;         // how many message to skip in the database? offset == -1 -> 0, -2 -> 1, ...
  int32_t skipFirst = 0;              // how many messages to skip in the first partition?
  bool firstPartFound = false;
  for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {      // traverse index for partitions
    ipart->show();
    // Choose partitions according to `incoming`
    log_d("d=%s", ipart->getValueSafe("d",""));
    if (strcmp(ipart->getValueSafe("d",""), incoming ? "i" : "o")) {
      continue;
    }

    // Get number of messages in the partition
    log_d("number of messages n=%s, skip=%d", ipart->getValueSafe("n",""), skip);
    int32_t n = ipart->getIntValueSafe("n", 0);

    if (skip < n && !firstPartFound) {
      // First partition located
      skipFirst = skip;
      int32_t partn = ipart->getIntValueSafe("p", -1);
      log_v("located first partition %d", partn);
      firstPartFound = true;
      if (partn < 0) {
        log_e("illegal partition number: %s", ipart->getValueSafe("p", ""));
      } else if (!part1.isLoaded() || part1.isEmpty() || part1[0].getIntValueSafe("p", -2) != partn) {
        this->loadPartition(this->part1, partn);
        // If we reloaded first partition, second partition must be also irrelevand if it's loaded
        part2.unload();
      }
      /* ⚠ OUTSIDE the reload branch, deliberately. This break used to live inside the
       * `else if` above, so whenever part1 was already CACHED the loop fell through and
       * loaded part2 unconditionally — a whole second partition parsed on every page after
       * the first, for a window that fits entirely in part1. Whether one partition is
       * enough depends on skip and count, not on how part1 got here. */
      if (skip + count <= n) {
        log_v("only one partition needed");
        break;   // no need to load next partition
      }
    } else if (firstPartFound) {
      // Second partition directly follows first partition (if it is non-empty)
      if (n <= 0) {
        continue;
      }
      int32_t partn = ipart->getIntValueSafe("p", -1);
      log_v("located second partition %d", partn);
      if (partn < 0) {
        log_e("illegal partition number: %s", ipart->getValueSafe("p", ""));
      } else if (!part2.isLoaded() || part2.isEmpty() || part2[0].getIntValueSafe("p", -2) != partn) {
        this->loadPartition(part2, partn);
      }
      break;
    }
    skip -= n;
  }
  if (!firstPartFound) {
    log_d("nothing to load");
    return 0;
  }
  if (!part1.isLoaded() || part1.isEmpty()) {
    log_e("part1 not loaded or empty");
    return 0;
  }

  // Preload messages from the located partition(s)
  preloaded.ensure(abs(preloadedRangeEnd - preloadedRangeStart) + count);
  int32_t cnt = 0;
  log_d("loading from part1 %d", skipFirst);
  int partn = part1[0].getIntValueSafe("p", -1);
  for (auto im = part1.iterator(1+skipFirst); im.valid() && cnt < count; ++im) {
    // WARNING: repeating code below
    preloaded.add(new MessageData(*im, partn));      // index == abs(preloadedRangeEnd - preloadedRangeStart)
    preloadedRangeEnd += (offset < 0) ? -1 : 1;
    cnt++;
  }
  if (part2.isLoaded() && cnt < count) {
    partn = part2[0].getIntValueSafe("p", -1);   // was read off part1 — wrong object
    log_d("loading from part2");
    for (auto im = part2.iterator(1); im.valid() && cnt < count; ++im) {
      // WARNING: repeating code from above
      preloaded.add(new MessageData(*im, partn));
      preloadedRangeEnd += (offset < 0) ? -1 : 1;
      cnt++;
    }
  }
  log_i("preloaded: %d, from: %d, to: %d", cnt, preloadedRangeStart, preloadedRangeEnd);
  return cnt;
}

bool Messages::loadPartition(IniFile& ini, int32_t part) {
  log_d("loadPartition %d", part);
  if (part < 0) {
    log_e("invalid partition number: %d", part);
    return false;
  }
  char fn[20];
  snprintf(fn, sizeof(fn), partitionFileFormat, part);
  log_v("messages file: %s", fn);
  return ini.load(fn);
}

/* Description:
 *     store message in the message database.
 *     Encode message in Base64 if it has non-printable characters or newlines.
 * Return:
 *     hash of the message text (or message data).
 */
Messages::hash_t Messages::saveMessage(const char* text, const char* fromUri, const char* toUri,
                                       bool incoming, unsigned long time, unsigned long ackTime,
                                       int64_t voipId, uint32_t meshId) {
  log_v("saving message to %s, time = %d, d = %c", toUri ? toUri : "nil", time, incoming ? 'i' : 'o');

  if (!time) {
    time--;  // store 0xFFFFFFFF insted of 0x00000000 so that sorting is still correct
    meshId = 0;   // the sentinel is repaired by load(), not finalised: never both
  }

  // Find first partition of the right type in index
  int32_t msgcnt = -1;
  int32_t partn  = -1;
  int partPos = 0;
  bool io = false;
  index.show();
  for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {       // traverse all partitions (start at section 1 because we want to skip 0, the header partition)
    if (strchr(ipart->getValueSafe("d", ""), incoming ? 'i' : 'o')) {
      msgcnt = ipart->getIntValueSafe("n", -1);
      partn  = ipart->getIntValueSafe("p", -1);
      partPos = (int32_t)ipart;
      if (strchr(ipart->getValueSafe("d", ""), incoming ? 'o' : 'i')) {
        io = true;
      }
      break;
    }
  }

  // Load or create the partition
  IniFile ini;
  bool inited = false;
  if (partn >=0 && msgcnt < Messages::PARTITION_SIZE) {

    log_d("selected partition: %d", partn);
    // Non-filled partition found -> add message into it
    this->loadPartition(ini, partn);
    log_d("partition loaded: %d", partn);

  } else {
    log_d("creating new partition");
    // Else -> create new partition

    // Determine the new partition number
    partn = index[0].getIntValueSafe("x", 1);
    for (auto ipart = index.iterator(1); ipart.valid(); ++ipart) {       // traverse all partitions
      int pn = ipart->getIntValueSafe("p", 0) + 1;
      if (pn > partn) {
        partn = pn;
      }
    }
    log_d("partn = %d", partn);

    // Assign partition filename, make sure the file does not exist
    char filename[20];
    do {
      snprintf(filename, sizeof(filename), partitionFileFormat, partn);
      log_v("messages file: %s", filename);
      if (ini.load(filename)) {
        log_e("file exists");
        partn++;
      }
    } while (ini.isLoaded());

    // Initialize partition
    ini.addSection();
    ini[0]["desc"] = "WiPhone messages partition";
    ini[0]["v"] = "1";
    ini[0]["n"] = "1";
    ini[0]["d"] = incoming ? "i" : "o";
    ini[0].putValueFullHex("t1", time);
    ini[0]["t2"] = ini[0]["t1"];
    ini[0]["p"] = partn;
    inited = true;
  }

  if (!ini.isLoaded() && !inited) {
    log_e("no partition to store");
    return 0;
  }

  // Actually store message into the partition
  log_d("storing new message: %d", partn);
  ini.addSection();
  ini[-1].putValueFullHex("t", time);
  if (meshId) {
    /* Read off a MESH clock: provisional until NTP/GPS finalises it (load(); SA-3). The index
     * row is marked below, once the partition has stored. */
    ini[-1].putValueFullHex("tm", meshId);
  }
  if (fromUri) {
    ini[-1][incoming ? "o" : "s"] = fromUri;
  }
  if (toUri) {
    ini[-1][incoming ? "s" : "o"] = toUri;
  }
  if (ackTime) {
    ini[-1].putValueFullHex("a", ackTime);
  }
  if (voipId) {
    // VoIP.ms message id — decimal, not hex, so it reads the same here as in COVEY's store
    // and in a CSM1 line. Absent on anything this phone sent or received itself.
    char vid[24];
    snprintf(vid, sizeof(vid), "%lld", (long long)voipId);
    ini[-1]["v"] = vid;
  }
  if (io) {
    ini[-1]["d"] = incoming ? "i" : "o";
  }
  if (incoming) {
    ini[-1]["u"] = "1";  // this message was not read yet
  }
  if (text) {
    if (NanoIni::isSafeString(text)) {
      ini[-1]["m"] = text;
    } else {
      ini[-1].putValueBase64("b", text);
    }
  }
  ini.reorderLast(1, Storage::messageCompare);
  if (incoming) {
    ini[0]["u"] = ini[0].getIntValueSafe("u", 0) + 1;  // increase the unread messages counter for entire partition
  }

  // Store partition
  if (ini.store()) {

    // Update index
    log_d("updating index");
    if (inited) {

      // Register new partition
      log_d("register new partition: %d", partn);
      index.addSection();
      index[-1]["p"] = partn;
      index[-1]["d"] = incoming ? "i" : "o";
      index[-1]["n"] = "1";
      index[-1].putValueFullHex("t1", time);
      index[-1]["t2"] = index[-1]["t1"];
      partPos = -1;

    } else if (partPos!=0) {

      // Update existing index section
      log_d("updated existing index");
      index[partPos]["n"] = index[partPos].getIntValueSafe("n", 0) + 1;
      auto tt = index[partPos].getHexValueSafe("t1", 2);
      if (time < tt) {
        index[partPos].putValueFullHex("t1", time);
      }
      tt = index[partPos].getHexValueSafe("t2", 2);
      if (time > tt) {
        index[partPos].putValueFullHex("t2", time);
      }
    }

    // Update unread counters in the index
    if (incoming) {
      index[partPos]["u"] = ini[0]["u"];
      index[0]["u"] = index[0].getIntValueSafe("u", 0) + 1;         // increase the unread messages counter for entire database
    }
    if (meshId) {
      index[partPos]["tm"] = "1";      // this partition holds a provisional stamp (load(), SA-3)
    }

    // Restore sorting by time in the index
    log_v("sorting index");
    index.sortFrom(1, Storage::messagePartitionCompare);

    if (!index.store()) {
      log_e("failed to save index");
    }

  } else {
    log_e("failed to save appended partition");
  }
  return 0;
}

int Messages::ingestMirrored(const struct SmsMirrorRecord& rec, const char* ownUri,
                             const char* peerUri, int scanDepth) {
  if (!this->loaded || !rec.peer[0]) {
    return -1;
  }

  // A mirrored record's direction is COVEY's: `out` means the ACCOUNT sent it, which from
  // this phone's point of view is a sent message, whoever typed it.
  const bool incoming = !rec.out;

  char vidStr[24];
  snprintf(vidStr, sizeof(vidStr), "%lld", (long long)rec.id);

  int32_t total = this->countAll(incoming);
  int32_t depth = (total < (int32_t)scanDepth) ? total : (int32_t)scanDepth;

  /* Walk newest-first, but remember the OLDEST unclaimed match rather than the first one
   * found. Two identical texts to the same person ("ok", twice) are two real messages, and
   * they arrive from COVEY in id order — so pairing each against the oldest candidate still
   * free is what keeps them lined up. Taking the newest would pair both to the same one. */
  int32_t adoptOffset = 0;              // 0 = none; offsets from preload() are negative
  bool haveById = false;

  if (depth > 0) {
    this->preload(incoming, -1, depth);
    for (auto it = this->iteratorCount(-1, depth); it.valid(); ++it) {
      MessageData& m = *it;

      const char* v = m.getValueSafe("v", "");
      if (v[0]) {
        if (!strcmp(v, vidStr)) {
          haveById = true;
          break;                        // exact: this record is already in the store
        }
        continue;                       // carries a different id, so it is not our pair
      }

      // No id: a candidate for the phone's own copy of this same text.
      char peerDigits[SMS_MIRROR_PEER_MAX];
      if (!smsMirrorDigits(m.getOtherUri(), peerDigits, sizeof(peerDigits))) {
        continue;
      }
      if (strcmp(peerDigits, rec.peer)) {
        continue;
      }
      if (strncmp(m.getMessageText(), rec.text, SMS_MIRROR_TEXT_MAX)) {
        continue;
      }
      adoptOffset = (int32_t)it;        // keep going: a later iteration finds an older one
    }
  }

  if (haveById) {
    return 0;
  }

  if (adoptOffset) {
    /* The phone already has this text; it simply did not know VoIP.ms's name for it. Write
     * the id onto the message we have, so the next mirror of the same text is caught by the
     * cheap exact check above instead of being paired again. */
    int32_t idx = abs(adoptOffset - preloadedRangeStart);
    if (idx >= 0 && idx < preloaded.size() && preloaded[idx]) {
      MessageData& mine = *preloaded[idx];
      mine["v"] = vidStr;               // the in-RAM copy, so a redraw agrees with the file

      IniFile* part = NULL;
      int32_t key = 0;
      if (this->findMessage(mine, part, key) && key > 0) {
        (*part)[key]["v"] = vidStr;
        if (!part->store()) {
          log_e("failed to store adopted VoIP id");
        }
      } else {
        /* Not fatal, and deliberately not an error return: the message IS in the store and
         * the user sees it exactly once, which is the whole point. The cost is that the
         * next mirror of this text pairs by text again instead of by id. */
        log_e("could not locate own copy to stamp id %s", vidStr);
      }
    }
    return 0;
  }

  // Genuinely new. Store it the same way a SIP message is stored, so nothing downstream
  // needs to know it came from COVEY.
  const char* fromUri = incoming ? peerUri : ownUri;
  const char* toUri   = incoming ? ownUri  : peerUri;
  this->saveMessage(rec.text, fromUri, toUri, incoming, rec.ts, 0, rec.id);
  return 1;
}

bool Messages::findMessage(MessageData& msg, IniFile*& part, int32_t& section) {
  log_i("<-- Messages");

  // Check if messages has all the necessary fields
  if (!msg.hasKey("m") && !msg.hasKey("b") || !msg.hasKey("t") || !msg.hasKey("p")) {
    log_e("missing field(s) in MessageData");
    IF_LOG(ERROR) msg.show();
    return false;
  }

  int partn = msg.getIntValueSafe("p", -1);
  log_v("looking for partition: %d", partn);

  /* ⚠ POINTER selection, not object assignment. This took `IniFile& part` and did
   * `part = part2;` meaning "switch to the other partition" — but a C++ reference cannot
   * be reseated, so that line invoked the default copy-assignment and SHALLOW-COPIED
   * part2's Section pointers into part1: two owners for every section in the partition,
   * a double free waiting at the next unload, and part1's own sections leaked. It stayed
   * latent only because it needed both caches loaded and mismatched at once. */
  part = &part1;
  if (!part->isLoaded() || part->isEmpty() || (*part)[0].getIntValueSafe("p", -2) != partn) {
    part = &part2;
    if (!part->isLoaded() || part->isEmpty() || (*part)[0].getIntValueSafe("p", -2) != partn) {
      log_v("loading partition %d", partn);
      part = &part1;
      this->loadPartition(*part, partn);
    }
  }

  // Find the actual message in the partition by time ("t") and text ("m")
  if (part->isLoaded() && !part->isEmpty()) {
    const char* field = msg.hasKey("m") ? "m" : "b";
    section = part->query("t", msg["t"], field, msg[field]);
    return section > 0;
  }
  log_e("not found");
  return false;
}

/* Description:
 *      take negative message offset (global message offset in the database), find the message in the cache (`preloaded` array)
 *      and pass it for deletion.
 */
bool Messages::deleteMessage(int32_t messageOffset) {
  log_i("messageOffset = %d", messageOffset);
  if (messageOffset <= preloadedRangeEnd || messageOffset > preloadedRangeStart) {
    log_e("wrong message offset %d, not in |%d..%d>", messageOffset, preloadedRangeStart, preloadedRangeEnd);
    return false;
  }
  messageOffset = abs(messageOffset - preloadedRangeStart);
  log_v("delete: preloaded[%d]", messageOffset);
  return this->deleteMessage(*preloaded[messageOffset]);
}

/* Description:
 *     find message by partition, text and time and delete it. Remove partition if it becomes empty as a result.
 * Return:
 *     true on success
 */
bool Messages::deleteMessage(MessageData& msg) {
  log_i("deleting message");
  IF_LOG(VERBOSE)
  msg.show();

  // Find message
  IniFile* part = NULL;
  int key = 0;
  bool found = this->findMessage(msg, part, key);
  log_v("found == %d", found);

  // Remove message and update index
  if (found && key > 0) {
    IniFile& ini = *part;              // whichever cache actually holds the partition

    // Remove message section from the partition
    ini.removeSection(key);

    // Update message counter in the index
    int partn = msg.getIntValueSafe("p", -1);
    int section = index.query("p", partn);    // key of the partition inside index
    index[section]["n"] = (int32_t)(ini.nSections() - 1);

    if (ini.nSections() > 1) {

      // There are more messages left in this partition -> only update times

      // Find min/max time
      uint32_t t1 = 0xFFFFFFFF, t2 = 0;
      for (auto it = ini.iterator(1); it.valid(); ++it) {
        if (!it->hasKey("t")) {
          continue;
        }
        uint32_t t = it->getHexValueSafe("t", 0);
        if (t < t1) {
          t1 = t;
        }
        if (t > t2) {
          t2 = t;
        }
      }

      if (ini[0].getHexValueSafe("t1", 0xFFFFFFFF) != t1 || ini[0].getHexValueSafe("t2", 0) != t2) {

        // Update partition times

        // - in partition file
        ini[0].putValueFullHex("t1", t1);
        ini[0].putValueFullHex("t2", t2);
        // - in index file
        if (section > 0) {
          index[section].putValueFullHex("t1", t1);
          index[section].putValueFullHex("t2", t2);
        }

      }

      // Save partition
      ini.store();

    } else {

      // No messages left in this partition -> remove the partition

      // Remove partition file
      ini.remove();

      // Remove partition section
      index.removeSection(section);

    }

    // Restore sorting by time in the index
    log_v("sorting index");
    index.sortFrom(1, Storage::messagePartitionCompare);

    // Save index
    index.store();

    return true;
  }
  return false;
}

void Messages::setRead(MessageData& msg) {
  if (!msg.hasKey("u")) {
    log_e("message already read");
    return;
  }

  // Change "read" state in the preloaded array
  msg.setRead();

  // Find message
  IniFile* part = NULL;
  int key = 0;
  bool found = this->findMessage(msg, part, key);

  // Change unread ("u") fields
  if (found && key > 0) {
    IniFile& ini = *part;              // whichever cache actually holds the partition
    // Remove unread flag from the message
    ini[key].remove("u");

    // Update unread messages counters
    // - unread counter in this partition
    int unread = ini[0].getIntValueSafe("u", 1) - 1;
    if (unread) {
      ini[0]["u"] = unread;
    } else {
      ini[0].remove("u");
    }
    // - unread counter in the index for this partition
    int partn = msg.getIntValueSafe("p", -1);
    key = index.query("p", partn);    // key of the partition inside index
    if (key>0) {
      if (unread) {
        index[key]["u"] = unread;
      } else {
        index[key].remove("u");
      }
    }
    // - unread counter of the index
    int globalUnread = index[0].getIntValueSafe("u", unread+1) - 1;
    if (globalUnread) {
      // globalUnread, NOT the per-partition remainder: writing `unread` here set the
      // database-wide counter to one partition's count, so with unread texts spread
      // across partitions the badge under-counted after any single message was read.
      index[0]["u"] = globalUnread;
    } else {
      index[0].remove("u");
    }

    // Save files to flash
    ini.store();
    index.store();
  } else {
    log_e("message not found");
  }
}

/* Drop the cached partitions and the preloaded window.
 *
 * ⚠ THE BUG THIS EXISTS FOR, measured on hardware 2026-08-19: recountUnread and
 * markAllRead write partition FILES directly (through a local IniFile), while
 * `part1`/`part2` may still hold those same partitions in RAM with the OLD "u"
 * flags. Any later store of a cached partition — an ingest, a setRead, a new
 * message — then writes the stale copy back and RESURRECTS every unread flag.
 * Symptom: `unread clear` reported "icon off", a recount agreed, and after the
 * next reboot all 31 were back. Invalidate around every direct write. */
void Messages::dropPartitionCaches() {
  part1.unload();
  part2.unload();
  this->clearPreloaded();
  preloadedRangeStart = 0;
  preloadedRangeEnd = 0;
}

int32_t Messages::recountUnread(bool repair, char* fromOut, size_t fromCap) {
  if (!this->loaded || !index.isLoaded()) {
    return -1;
  }
  dropPartitionCaches();      // read fresh, and leave nothing stale to flush
  if (fromOut && fromCap) {
    fromOut[0] = '\0';
  }

  int32_t total = 0;
  bool indexChanged = false;

  for (size_t s = 1; s < index.nSections(); s++) {
    if (strcmp(index[s].getValueSafe("d", ""), "i") != 0) {
      continue;                                   // unread lives on incoming only
    }
    int partn = index[s].getIntValueSafe("p", -1);
    if (partn < 0) {
      continue;
    }

    /* A LOCAL IniFile per partition — deliberately NOT the part1/part2 caches,
     * whose aliasing history (the 0.9.4 shallow-copy double-free) is exactly
     * why this walk must not reseat them. NanoIni allocates in PSRAM, so the
     * cost here is PSRAM churn, never internal heap. */
    IniFile part;
    if (!loadPartition(part, partn)) {
      log_e("unread: partition %d unreadable, skipped", partn);
      continue;
    }

    int32_t actual = 0;
    for (size_t k = 1; k < part.nSections(); k++) {
      if (part[k].hasKey("u")) {
        actual++;
        if (fromOut && fromCap && !fromOut[0]) {
          snprintf(fromOut, fromCap, "%s", part[k].getValueSafe("o", "?"));
        }
      }
    }
    total += actual;

    // Copy 1: the partition file's own header counter.
    int32_t claimed = part[0].getIntValueSafe("u", 0);
    if (claimed != actual) {
      log_e("unread: partition %d header claims %d, records hold %d%s",
            partn, (int)claimed, (int)actual, repair ? " - repaired" : "");
      if (repair) {
        if (actual) {
          part[0]["u"] = (int)actual;
        } else {
          part[0].remove("u");
        }
        part.store();
      }
    }
    // Copy 2: the index's per-partition counter.
    int32_t idxClaimed = index[s].getIntValueSafe("u", 0);
    if (idxClaimed != actual && repair) {
      if (actual) {
        index[s]["u"] = (int)actual;
      } else {
        index[s].remove("u");
      }
      indexChanged = true;
    }
  }

  // Copy 3: the global counter — the one hasUnread() and the white icon read.
  int32_t globalClaimed = index[0].getIntValueSafe("u", 0);
  if (globalClaimed != total) {
    log_e("unread: index total claims %d, records hold %d%s",
          (int)globalClaimed, (int)total, repair ? " - repaired" : "");
    if (repair) {
      if (total) {
        index[0]["u"] = (int)total;
      } else {
        index[0].remove("u");
      }
      indexChanged = true;
    }
  }
  if (repair && indexChanged) {
    index.store();
  }
  if (repair) {
    dropPartitionCaches();    // ...and nothing stale may outlive the repair
  }
  return total;
}

int32_t Messages::markAllRead() {
  if (!this->loaded || !index.isLoaded()) {
    return -1;
  }
  dropPartitionCaches();
  int32_t cleared = 0;
  bool indexChanged = false;

  for (size_t s = 1; s < index.nSections(); s++) {
    if (strcmp(index[s].getValueSafe("d", ""), "i") != 0) {
      continue;
    }
    int partn = index[s].getIntValueSafe("p", -1);
    if (partn < 0) {
      continue;
    }
    IniFile part;                        // local, not part1/part2 — see recountUnread
    if (!loadPartition(part, partn)) {
      continue;
    }
    int32_t inPart = 0;
    for (size_t k = 1; k < part.nSections(); k++) {
      if (part[k].hasKey("u")) {
        part[k].remove("u");
        inPart++;
      }
    }
    if (inPart > 0 || part[0].hasKey("u")) {
      part[0].remove("u");
      const bool ok = part.store();
      /* Kept: this line is the only thing that can tell a failed WRITE apart
       * from flags being re-marked afterwards — the two look identical from
       * the outside ("I cleared it and it came back"), and telling them apart
       * is what solved the stale-cache bug dropPartitionCaches() now prevents. */
      log_e("unread clear: partition %d, %d flag(s), store %s",
            partn, (int)inPart, ok ? "OK" : "FAILED");
      cleared += inPart;
    }
    if (index[s].hasKey("u")) {
      index[s].remove("u");
      indexChanged = true;
    }
  }
  if (index[0].hasKey("u")) {
    index[0].remove("u");
    indexChanged = true;
  }
  if (indexChanged) {
    const bool ok = index.store();
    log_e("unread clear: index store %s", ok ? "OK" : "FAILED");
  }
  /* ⚠ MUST be the last thing: part1/part2 may hold these very partitions with
   * their "u" flags intact, and the next store of a cached partition would
   * write them straight back over what we just cleared. */
  dropPartitionCaches();
  return cleared;
}

void Messages::setSent(MessageData& msg) {
  // TODO
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  STORAGE CLASS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

int Storage::messagePartitionCompare(NanoIni::Section** a, NanoIni::Section** b) {
  int res = strcasecmp((*b)->getValueSafe("t2", ""), (*a)->getValueSafe("t2", ""));
  if (!res) {
    res = strcasecmp((*b)->getValueSafe("t1", ""), (*a)->getValueSafe("t1", ""));
  }
  if (!res) {
    res = strcasecmp((*b)->getValueSafe("p", ""), (*a)->getValueSafe("p", ""));
  }
  return res;
}

int Storage::messageCompare(NanoIni::Section** a, NanoIni::Section** b) {
  return strcasecmp((*b)->getValueSafe("t", ""), (*a)->getValueSafe("t", ""));
}

// - - - - - - - - - - - - - - - - - - - - -  Phonebook  - - - - - - - - - - - - - - - - - - - - -

int Storage::phonebookCompare(NanoIni::Section** a, NanoIni::Section** b) {
  int res = strcasecmp((*a)->getValueSafe("n", ""), (*b)->getValueSafe("n", ""));
  if (!res) {
    res = strcasecmp((*a)->getValueSafe("s", ""), (*b)->getValueSafe("s", ""));
  }
  return res;
}

bool Storage::loadPhonebook() {
  if ((phonebook.load() || phonebook.restore()) && !phonebook.isEmpty()) {
    if (phonebook[0].hasKey("v") && !strcmp(phonebook[0]["v"], "2")) {    // check version of the file format
      log_v("phonebook loaded");
      return true;
    } else {
      log_d("phonebook format error");
      return false;
    }
  } else if (this->loadPhonebookOld()) {
    // Migrate phonebook data to an INI file
    log_d("reformatting phonebook from NVS to INI:");
    phonebook.addSection();
    phonebook[0]["desc"] = "WiPhone phonebook";
    phonebook[0]["v"] = "2";
    int sect = 1;
    for (PhonebookRecord* rec = phonebookOld.firstRecord(); rec != NULL; rec = phonebookOld.nextRecord()) {
      phonebook.addSection();
      phonebook[sect]["n"] = rec->getName();
      phonebook[sect]["s"] = rec->getSip();
      sect++;
    }
    log_d("new phonebook:");
    phonebook.store();
    phonebook.backup();
    return true;
  } else {
    log_d("phonebook not found");
    phonebook[0]["desc"] = "WiPhone phonebook";
    phonebook[0]["v"] = "2";
    return true;
  }
}

bool Storage::loadPhonebookOld() {
  log_d("loadPhonebookOld");

  this->end();
  this->begin("addr");
  bool succ = true;
  {
    // Load phonebook from NVS

    // Load the "max" value
    uint16_t addrMaxId = this->getUShort("max",  0);
    log_d("maxId: %d", addrMaxId);

    // Load individual "subpages" and merge them
    char* strDyn = NULL;
    int strLen = 0;
    for (uint16_t key=1; key<=addrMaxId; key++) {
      char addrId[6];   // up to 5 decimal numbers
      sprintf(addrId, "%d", key);
      // if success (any bytes) -> merge
      char buff[Storage::nvsMaxStringLen+2];
      uint16_t bytes = this->getString(addrId,  buff, sizeof(buff)-1);
      if (bytes) {
        // merge into single phonebook (in PSRAM)
        int len = strlen(buff);
        if (len && buff[len-1]!='\n') {
          buff[len++] = '\n';  // terminate string with NL (backwards compatibility)
          buff[len] = '\0';
        }
        char* p = (char *) extRealloc(strDyn, strLen + 1 + len);
        if (p) {
          strDyn = p;
          memcpy(strDyn + strLen, buff, len);
          strLen += len;
          strDyn[strLen] = '\0';
        } else {
          succ = false;
        }
      }
    }

    // Pass the obtained string to phonebook class
    phonebookOld.parse(strDyn);
    //freeNull((void **) &strDyn);    // TODO: why is this commented?
  }

  IF_LOG(VERBOSE)
  phonebookOld.show();

  return succ;
}

// - - - - - - - - - - - - - - - - - - - - -  IniFile class  - - - - - - - - - - - - - - - - - - - - -

IniFile::IniFile(const char* fn) {
  this->setFilename(fn);
}

IniFile::~IniFile() {
  freeNull((void **) &filenameDyn);
}

bool IniFile::load() {
  if (loaded) {
    log_e("file is already loaded");
    return false;
  }
  if (filenameDyn == nullptr || *filenameDyn=='\0') {
    log_e("could not load: filename empty");
    return false;
  }
  if (!SPIFFS.exists(filenameDyn)) {
    log_e("could not load: file \"%s\" does not exist", filenameDyn);
    return false;
  }
  File file = SPIFFS.open(filenameDyn);
  if (file && file.available()) {

    // Read entire INI file into the external RAM
    int bytes;
    char buff[1025];
    LinearArray<char, LA_EXTERNAL_RAM> fileContent;
    do {
      bytes = file.readBytes(buff, sizeof(buff)-1);
      if (bytes > 0) {
        fileContent.extend(buff, bytes);  // NOTE: this creates small unnecessary overhead for small files
      }
    } while (bytes == sizeof(buff)-1);
    log_d("Read %d bytes from \"%s\"", fileContent.size(), filenameDyn);
    fileContent.add('\0');    // terminate with nul-character to form a C-string

    // Parse INI
    if (fileContent.size() > 1) {
      log_v("INI file before parsing:\n%s", &fileContent[0]);
      this->parse(&fileContent[0]);
      this->loaded = true;
    }
    return true;
  } else {
    log_e("could not load or empty: file \"%s\"", filenameDyn);
    return false;
  }
}

bool IniFile::load(const char* filename) {
  this->setFilename(filename);
  return this->load();
}

void IniFile::setFilename(const char* filename) {
  if (this->loaded) {
    this->unload();
  }
  freeNull((void **) &filenameDyn);
  filenameDyn = extStrdup(filename);
}

/* Description:
 *     unload from RAM
 */
void IniFile::unload() {
  this->clear();
  this->loaded = false;
}

bool IniFile::store() {
  if (filenameDyn == nullptr || *filenameDyn=='\0') {
    log_e("could not store: filename empty");
    return false;
  }
  log_d("writing file \"%s\"", filenameDyn);
  File file = SPIFFS.open(filenameDyn, FILE_WRITE);
  if (file) {
    size_t len = this->length();
    char* str = (char*) extMalloc(len+1);
    this->sprint(str);
    log_v("writing file:\n%s", str);            // prints entire file contents to logs
    log_v("-------------\nfile size: %d", len);
    file.write((const uint8_t*) str, len);
    file.close();
    free(str);
    log_v("wrote %d bytes to \"%s\"", len, filenameDyn);
    loaded = true;    // in-memory data corresponds to disk data
    return true;
  } else {
    log_e("failed to create a file");
    return false;
  }
}

void IniFile::show() {
  log_d("IniFile \"%s\" contents:\r\n%s", filenameDyn, this->p_c_str().get());
}

void IniFile::remove() {
  SPIFFS.remove(filenameDyn);
  this->clear();
}

// - - - - - - - - - - - - - - - - - - - - -  CriticalFile class  - - - - - - - - - - - - - - - - - - - - -

/* Description:
 *     try to save the NanoINI string into the NVS on backup() (but as long as it's not already stored)
 */
bool CriticalFile::backup(uint32_t unixTime) {
  // Get NVS page name
  char* pageDyn = this->pagename();
  if (!pageDyn) {
    log_e("empty pagename, cannot backup");
    return false;
  }
  bool success = false;

  // Check length
  size_t len = this->length();
  if (len > 0 && len < 4000) {

    // Load the backup string and compute hash
    HASHHEX storedHash;
    storedHash[0] = '\0';     // initialize as an empty string
    this->end();
    if (this->begin(pageDyn, true)) {      // read-only
      const int maxStringSize = 4001;          // including the nul-character
      char* tmp = (char*) extMalloc(maxStringSize);
      if (tmp!=NULL) {
        int len1 = this->getString(backupKey, tmp, maxStringSize);
        if (len1 > 0 && len1 != maxStringSize) {
          md5Compress(tmp, len1-1, storedHash);
        }
        /* ⚠ WAS NEVER FREED. backup() runs from the DESTRUCTOR of every settings app, so
         * this 4,001-byte buffer leaked every time you backed out of a settings screen,
         * changed or not. PSRAM, so it was not a panic mechanism — but it is a monotone
         * leak on a routine path, reclaimed only by reboot. restore() and store() have
         * always freed theirs. */
        freeNull((void **) &tmp);
      }
    }

    // Serialize this INI
    char* str = (char*) extMalloc(len+1);
    if (str == NULL) {
      log_e("backup: no memory to serialize %d bytes", len);
      freeNull((void **) &pageDyn);
      return false;                    // was passed straight to sprint() unchecked
    }
    this->sprint(str);

    // Calculate hash to compare to the previous one
    if (storedHash[0] != '\0') {
      HASHHEX thisHash;
      md5Compress(str, len, thisHash);
      if (!strcmp(storedHash, thisHash)) {
        log_i("%d bytes to \"%s\": same data, skipping", len, pageDyn);
        success = true;
      }
    }

    // String is sufficiently short and differs from the already strored -> save to NVS
    if (!success) {
      this->end();
      if (this->begin(pageDyn, false)) {
        if (this->putString(backupKey, str)>0) {
          log_i("%d bytes to \"%s\": successful", len, pageDyn);
          success = true;
        }
      }
    }
    freeNull((void **) &str);          // the other half of the same leak; last use is above
  }

  if (!success) {
    log_e("%d bytes to \"%s\": FAILED", len, pageDyn);
  }
  freeNull((void **) &pageDyn);
  return success;
}

/* Description:
 *     restore NanoINI file from the NVS
 */
bool CriticalFile::restore() {
  log_d("restoring INI");

  // Get NVS page name
  char* pageDyn = this->pagename();
  if (!pageDyn) {
    log_e("empty NVS page name, cannot restore");
    return false;
  }
  bool success = false;

  // Load from NVS
  this->end();
  if (this->begin(pageDyn, true)) {         // read-only
    const int maxStringSize = 4001;         // including the nul-character
    char* tmp = (char*) extMalloc(maxStringSize);
    if (tmp != NULL) {
      int len1 = this->getString(backupKey, tmp, maxStringSize);
      if (len1 > 0 && len1 != maxStringSize) {
        this->parse(tmp);
        if (!this->isEmpty()) {
          log_i("%d bytes from \"%s\": successful", len1-1, pageDyn);
          success = true;
        }
      }
      freeNull((void **) &tmp);
    }
  }
  if (!success) {
    log_e("page \"%s\": FAILED", pageDyn);
  }
  freeNull((void **) &pageDyn);

  // Finally: save to SPIFFS
  if (success) {
    this->store();
  }

  return success;
}

void CriticalFile::showBackup() {
  log_d("showing contents of INI backup");

  // Get NVS page name
  char* pageDyn = this->pagename();
  if (!pageDyn) {
    log_e("empty NVS page name, cannot restore");
    return;
  }
  bool success = false;

  // Load from NVS
  this->end();
  if (this->begin(pageDyn, true)) {         // read-only
    const int maxStringSize = 4001;         // including the nul-character
    char* tmp = (char*) extMalloc(maxStringSize);
    if (tmp != NULL) {
      int len1 = this->getString(backupKey, tmp, maxStringSize);
      if (len1 > 0) {
        log_i("%d bytes in backup", len1-1);
        log_i("contents:\n%s", tmp);
        success = true;
      }
      freeNull((void **) &tmp);
    }
  }
  if (!success) {
    log_e("page \"%s\": FAILED", pageDyn);
  }
  freeNull((void **) &pageDyn);
}

/* Description:
 *     return dynamically allocated string for the page name
 */
char* CriticalFile::pagename() {
  if (filenameDyn == NULL || *filenameDyn=='\0') {
    return NULL;  // empty name
  }

  // Take part following the rightmost slash
  const char* tmp = strrchr(filenameDyn, '/');
  tmp = (*tmp=='/') ? tmp + 1 : filenameDyn;

  // Take part before the first dot; if starts with the dot - take entire name
  size_t len = strcspn(tmp, ".");
  if (!len) {
    len = strlen(tmp);
  }
  if (!len) {
    return NULL;  // empty basename
  }

  return strndup(tmp, len);
}

// - - - - - - - - - - - - - - - - - - - - -  Configs for UdpSenderApp  - - - - - - - - - - - - - - - - - - - - -

void Storage::loadUdpSender(const char*& ip, int32_t& port, const char*& text) {
  char str[defaultMaxText+1];
  this->end();
  if (this->begin("app_udp_send", true)) {       // read only
    ip   = this->getString("ip",   str, sizeof(str)-1) ? strdup(str) : strdup("");
    text = this->getString("text", str, sizeof(str)-1) ? strdup(str) : strdup("");
    port = this->getInt("port", -1);    // not UShort because default value is negative
  } else {
    ip = text = NULL;
    port = -1;
  }
  this->end();
}

void Storage::storeUdpSender(const char* ip, const int32_t port, const char* text) {
  this->end();
  if (this->begin("app_udp_send", false)) {       // not read-only
    if (ip!=NULL) {
      this->putString("ip", ip);
    }
    if (text!=NULL) {
      this->putString("text", text);
    }
    this->putInt("port", port);           // not UShort because default value is negative
  }
  this->end();
}

// - - - - - - - - - - - - - - - - - - - - -  Generic helpers  - - - - - - - - - - - - - - - - - - - - -

Storage::Storage()
  : phonebook(Storage::PhonebookFile)
{};

void Storage::storeString(const char* page, const char* variable, const char* val) {
  this->end();
  if (this->begin(page, false)) {       // not read-only
    this->putString(variable, val ? val : "");      // if empty or NULL - overwrite with empty string
  }
  this->end();
}

/* Description:
 *     load a string from NVS
 * Return:
 *     dynamically allocated string is stored into val
 */
void Storage::loadString(const char* page, const char* variable, const char*& val) {
  this->end();
  if (this->begin(page, true)) {       // read only
    log_v("page found: %s", page ? page : "(nil)");
    char str[defaultMaxText+1];
    val = this->getString(variable, str, sizeof(str)-1) ? strdup(str) : strdup("");     // return empty string on error   // TODO: use str::move
    log_v("loaded: %s", str);
  } else {
    log_e("page not found: %s / %s", page ? page : "(nil)", variable ? variable : "(nil)");
    val = strdup("");
  }
  this->end();
}

void Storage::storeInt(const char* page, const char* variable, int32_t val) {
  this->end();
  if (this->begin(page, false)) {       // not read-only
    this->putInt(variable, val);
  }
  this->end();
}

void Storage::loadInt(const char* page, const char* variable, int32_t& val) {
  this->end();
  if (this->begin(page, true)) {              // read only
    val = this->getInt(variable, val);        // return what provided on error
  }
  this->end();
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  SUPPORT CLASSES  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

// - - - - - - - - - - - - - - - - - - - - -  Phonebook record  - - - - - - - - - - - - - - - - - - - - -

PhonebookRecord::PhonebookRecord(const char* phonebook, const char* sip) {
  if (phonebook) {
    this->nameDyn = strdup(phonebook);
  }
  if (sip) {
    this->sipDyn = strdup(sip);
  }
}

PhonebookRecord::~PhonebookRecord() {
  freeNull((void **) &this->nameDyn);
  freeNull((void **) &this->sipDyn);
}

bool PhonebookRecord::operator<=(const PhonebookRecord& other) {
  if (this->nameDyn && other.nameDyn && nameDyn[0] && other.nameDyn[0]) {
    return strcasecmp(this->nameDyn, other.nameDyn) <= 0;
  }
  if (this->nameDyn && nameDyn[0]) {
    return true;
  }
  if (other.nameDyn && other.nameDyn[0]) {
    return false;
  }
  if (this->sipDyn && other.sipDyn && sipDyn[0] && other.sipDyn[0]) {
    return strcasecmp(this->sipDyn, other.sipDyn) <= 0;
  }
  if (this->sipDyn && sipDyn[0]) {
    return true;
  }
  return false;
}

void PhonebookRecord::parse(const char* str, int& skip) {
  skip = 0;
  if (str[0]=='n') {
    const char* eq = str + strcspn(str, "=");
    const char* nl = str + strcspn(str, "\n");
    if (((eq-str)==1 || ((eq-str)==4 && !strncmp(str, "name=", 5))) && nl>eq) {
      //log_d("name found");
      skip = nl-str;
      this->nameDyn = strndup(eq+1, nl-eq-1);
      if (*nl!='\0') {    // are there more fields?
        // Next field
        skip++;
        str = nl + 1;
        if (str[0] == 's') {
          eq = str + strcspn(str, "=");
          nl = str + strcspn(str, "\n");
          if (((eq-str)==1 || ((eq-str)==3 && !strncmp(str, "sip=", 4))) && nl>eq) {
            //log_d("sip found");
            skip += nl-str;
            this->sipDyn = strndup(eq+1, nl-eq-1);
            if (*nl!='\0') {
              skip++;
            }
          }
        }
      }
    }
    //log_d("PhonebookRecord: name=%s, sip=%s, skip=%d", this->nameDyn ? this->nameDyn : "", this->sipDyn ? this->sipDyn : "", skip);
  }
}

int PhonebookRecord::length() {
  int r = 6;    // n=\ns=\n
  if (this->nameDyn) {
    r += strlen(this->nameDyn);
  }
  if (this->sipDyn) {
    r += strlen(this->sipDyn);
  }
  return r;
}

int PhonebookRecord::serialize(char* buff) {
  char* start = buff;
  buff += sprintf(buff, "n=%s\n", this->nameDyn ? this->nameDyn : "");
  buff += sprintf(buff, "s=%s", this->sipDyn ? this->sipDyn : "");
  *buff++ = '\n';   // terminate with NL, not NUL
  return buff-start;
}
