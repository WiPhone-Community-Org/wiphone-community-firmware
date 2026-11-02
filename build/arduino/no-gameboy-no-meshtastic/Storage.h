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

#ifndef _STORAGE_H_
#define _STORAGE_H_

/*
 *  Class Storage encapsulates saving data to flash.
 *
 *  Three backends that can be supported:
 *  - NVS (Non-Volatile Storage)
 *  - SPIFFS
 *  - SD card
 *
 *  NVS:
 *    uses a separate partition in SPI flash of ESP32 module (4 MB for ESP32-WROVER)
 *    typically it can store of 20 KB of data, intended for configuration and preferences
 *  SPIFFS:
 *    uses a separate partition in SPI flash of ESP32 module with a file system on it
 *    typically it can store 60KB (Minimal SPIFFS) or 1.43 MB (Default) of data
 *  SD card:
 *    file system on SD card
 *
 *  This class is built on top of Preferences class which encapsulates NVS interface.
 *
 *  See "Storage API" of ESP-IDF:
 *    https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/storage/index.html
 */

// Internal flash
#include <Preferences.h>
// SD card
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "SPIFFS.h"
#include "nvs.h"
#include "NanoINI.h"
#include "clock_source.h"   // ClockMsgStamp only (pure; clock.h itself cannot be included - below)
//#include "clock.h"      // CANNOT USE CLOCK HERE: since it depends on Networks, which depends on Storage

/*
 *  Description:
 *      DEPRECATED: used by deprecated Phonebook class.
 *      Parsed representation of a phonebook record.
 */
class PhonebookRecord {
public:
  PhonebookRecord() {};
  PhonebookRecord(const char* phonebook, const char* sip);
  bool operator<=(const PhonebookRecord& other);
  ~PhonebookRecord();

  void parse(const char* phonebook, int& skip);
  int serialize(char* buff);
  int length();

  const char* getName() {
    return nameDyn;
  };
  const char* getSip() {
    return sipDyn;
  };

protected:
  char* nameDyn = NULL;
  char* sipDyn = NULL;
};


/*
 *  Description:
 *      DEPRECATED: old phonebook interface. Stores phonebook as a single string in NVS only.
 *      Kept only for decoding the old phonebook.
 */
class Phonebook {
public:
  void parse(char* str);
  bool isLoaded() {
    return phonebookDyn!=NULL;
  };
  bool reformat();         // reformat old format into newer (TODO: needs to be removed in production)
  unsigned int addRecord(PhonebookRecord* rec);
  unsigned int addRecord(const char* name, const char* sip);
  unsigned int replaceRecord(unsigned int pos, const char* name, const char* sip);
  bool removeRecord(unsigned int pos);
  void show();

  const char* c_str() {
    return phonebookDyn;
  };
  int length() {
    return phonebookLen;
  };

  // Phonebook "iterators"
  bool next();                     // advance phonebookOff to the start of next record, return true if new offset points to a record
  bool find(unsigned int pos);     // set phonebookOff to a certain record number

  // Phonebook "iterators" returning a dynamically allocated object (need to be deleted!) TODO: check
  PhonebookRecord* firstRecord();
  PhonebookRecord* nextRecord();
  PhonebookRecord* findRecord(unsigned int pos);

protected:
  // Phonebook variables
  char* phonebookDyn = NULL;
  int phonebookLen = 0;
  int phonebookOff = 0;
};

/* Description:
 *     configuration or data file stored and loaded from SPIFFS through NanoINI interface
 */
class IniFile : public NanoIni::Config {
public:
  IniFile() {}
  IniFile(const char* fn);

  ~IniFile();

  bool load();                  // load from permanent storage (SPIFFS or SD)
  bool load(const char* fn);    // change associated filename and load
  bool store();                 // store to permanent storage (SPIFFS or SD)
  void show();
  void remove();

  const char* filename() {
    return filenameDyn;
  }
  bool isLoaded() {
    return loaded;
  }
  void unload();    // unload from RAM

protected:
  char* filenameDyn = nullptr;
  bool loaded = false;

  void setFilename(const char* fn);
};

/* Description:
 *     same as IniFile, but has interface to backup and restore from NVS
 */
class CriticalFile : public IniFile, Preferences {
public:
  CriticalFile(const char* fn) : IniFile(fn), Preferences() {};

  bool backup(uint32_t unixTime=0);
  bool restore();
  void showBackup();    // for debugging

protected:
  static constexpr const char* backupKey = "backup";
  char* pagename();       // NVS page name is derived from SPIFFS filename. For example: "/sip_accounts.ini" -> "sip_accounts"
};

/*
 * Description:
 *     wrapper over raw message data in INI sections (handled via NanoIni::Section)
 */
class MessageData : public NanoIni::Section {
public:
  MessageData(NanoIni::Section& message);
  MessageData(NanoIni::Section& message, int partn);
  MessageData(const char* fromUri, const char* toUri, const char* text, uint32_t time, bool incoming);

  bool isRead()             {
    return !this->hasKey("u");
  }
  void setRead()            {
    this->remove("u");
  }
  const char* getOwnUri()   {
    return this->getValueSafe("s", "");
  }
  const char* getOtherUri() {
    return this->getValueSafe("o", "");
  }
  const char* getMessageText();
  unsigned long getTime();
  unsigned long getAckTime();

protected:
  std::string decodedText;
  unsigned long time = 0;
  unsigned long ackTime = 0;
};

typedef LinearArray<MessageData*, LA_EXTERNAL_RAM> MessagesArray;

/*
 * Description:
 *     messages database is split accross multiple files (handled by NanoIni interface), indexed by a single index file.
 *     This is a higher level interface that is meant to abstract from actual storage.
 *     (Incidentally, this is probably the most complex class in WiPhone firmware.)
 */
class Messages {
public:
  /*
   * This allows iterating over preloaded messages like:
   *     for (auto it = arr.iteratorCount(-1, 5); it.valid(); ++it) { op(*it); }
   *     for (auto it = arr.iteratorCount(0, 5);  it.valid(); ++it) { op(*it); }
   */
  class MessagesIterator {

    typedef MessagesIterator iterator;

  public:
    MessagesIterator(MessagesArray& arr, int32_t off, int32_t i, int32_t cnt)
      : arr_(arr), offset_(off), cnt_(cnt), pos_(i) {
      delta_ = pos_ < 0 ? -1 : 1;
    }
    ~MessagesIterator() {}

    iterator  operator++(int32_t) { /* postfix */
      int32_t p = pos_ + delta_;
      cnt_--;
      return MessagesIterator(arr_, offset_, p, cnt_);
    }
    iterator& operator++() {      /* prefix */
      pos_ += delta_;
      cnt_--;
      return *this;
    }
    MessageData& operator* () const               {
      return *(arr_[abs(pos_-offset_)]);
    }
    MessageData* operator->() const               {
      return arr_[abs(pos_-offset_)];
    }
    operator  int32_t() const                     {
      return pos_;
    }
    bool      valid()                             {
      return cnt_ > 0 && ((delta_ >= 0) ? (pos_ < arr_.size()) : (arr_.size() >= -pos_));
    }

  protected:
    int32_t offset_;
    int32_t pos_;
    int32_t cnt_;
    int8_t delta_ = 1;
    MessagesArray& arr_;
  };

  /*
   * Actual Messages class methods
   */
  typedef uint32_t hash_t;           // we are using Murmur3_32 hash (seed 5381) to hashing text of the messages

  Messages();
  /* `st` is ntpClock.msgStamp(): the time the unknown-time repair stamps with (UTC; 0 = do not
   * repair), whether that is a MESH clock's (its stamps are then marked provisional, "tm"),
   * and, under a trusted clock, the offset that finalises the provisional ones. See
   * clock_source.h, "THE SIP MESSAGE STORE UNDER A MESH CLOCK". */
  bool load(const ClockMsgStamp& st);
  void unload();
  bool isLoaded() {
    return this->loaded;
  }
  bool hasUnread() {
    return this->loaded && this->index.isLoaded() && !this->index.isEmpty() && this->index[0].getIntValueSafe("u", 0) > 0;
  }

  // Access interfaces
  int32_t inboxTotalSize();
  int32_t sentTotalSize();
  MessagesIterator iteratorCount(int32_t offset, int32_t cnt) {
    return MessagesIterator(preloaded, preloadedRangeStart, offset, cnt);
  }

  void clearPreloaded();
  int32_t preload(bool incoming, int32_t offset, int32_t count);       // this method accepts negative offsets (in Python style)

  // Modification interfaces
  /* `voipId` is the VoIP.ms message id, stored under "v", and 0 means "none" — which is the
   * case for every message the phone itself sends or receives over SIP or LoRa. It exists
   * so a text mirrored from COVEY can be recognised as one already held; see
   * ingestMirrored() and WiPhone/sms_mirror.h. */
  /* `meshId` nonzero: `time` was read off a MESH clock (ClockMsgStamp::meshId), so the stamp
   * is PROVISIONAL — marked "tm" and finalised by the first load under NTP/GPS. Only the
   * phone's OWN stamps pass it (a SIP arrival, a text composed here); a mirrored record's
   * time is COVEY's and final. */
  hash_t saveMessage(const char* text, const char* fromUri, const char* toUri,
                     bool incoming, unsigned long time, unsigned long ackTime=0,
                     int64_t voipId=0, uint32_t meshId=0);

  /* Fold one text mirrored from COVEY into this store.
   *
   * COVEY holds the whole account history (it reads the VoIP.ms REST API, which this phone
   * cannot do — it has no working TLS) and relays it here over the LAN and over LoRa. Both
   * transports carry the same record and both may deliver the same one, so this has to be
   * idempotent.
   *
   *   returns  1  stored a new message
   *            0  already had it — either by id, or paired with the phone's own copy
   *           -1  refused (database not loaded, or a record with no correspondent)
   *
   * ⚠ TWO KINDS OF DUPLICATE, and only the first is easy. A record we have already ingested
   * carries the same VoIP.ms id, so it is caught exactly. But a text this phone sent or
   * received ITSELF over SIP has no id at all, and COVEY will mirror it back because
   * `getSMS` returns the whole account's history. That copy is paired by correspondent +
   * direction + exact text, and the id is then written onto the message we already had, so
   * the pairing only ever has to happen once.
   *
   * ⚠ TIME IS NOT PART OF THE PAIRING. VoIP.ms dates do not reconcile to any single offset —
   * COVEY measured them landing up to an HOUR in the future — so any window tight enough to
   * discriminate would reject genuine matches.
   *
   * ⚠ THE SCAN IS BOUNDED to the newest `scanDepth` messages in the record's own direction,
   * because an unbounded scan means loading every partition on a phone with ~20 KB of
   * contiguous internal heap. The consequence, stated plainly: if a mirrored text pairs with
   * a copy OLDER than that window, it is stored twice. That needs a burst of more than
   * `scanDepth` messages between COVEY sending and the phone hearing it, which is far
   * outside normal use — but it is a real limit, not an impossible one.
   *
   * ⚠ Disturbs the preloaded cache. Callers showing a message list must clearPreloaded()
   * and rebuild after a batch, exactly as the delete path already does.
   */
  int ingestMirrored(const struct SmsMirrorRecord& rec, const char* ownUri,
                     const char* peerUri, int scanDepth=60);
  bool deleteMessage(MessageData&);
  bool deleteMessage(int32_t messageOffset);
  void setRead(MessageData&);

  /* Recount unread across EVERY incoming partition from the messages themselves
   * (the "u" key on each record is the truth) and repair the three derived
   * copies — partition header, index per-partition, index global — wherever
   * they disagree. The counters have drifted before (0.9.4 fixed an under-count;
   * an OVER-count leaves the white unread icon lit forever with nothing to
   * read). Returns the true unread total, or -1 if the store isn't loaded.
   * `fromOut`/`fromCap` (optional) receive the from-URI of the OLDEST unread
   * message, so the caller can NAME the thread to open.
   * ⚠ Loads each incoming partition once into a LOCAL IniFile (PSRAM, freed on
   * return) — deliberately not through part1/part2, whose aliasing history is
   * why findMessage takes a pointer out-param. Bounded by partition count. */
  int32_t recountUnread(bool repair, char* fromOut = NULL, size_t fromCap = 0);

  /* Mark EVERY incoming message read, at any depth, thread or no thread.
   * Exists for exactly one situation: unread flags belonging to a conversation
   * that no longer has a row in the UI (deleted thread, wiped-then-remirrored
   * history) — there is nothing left to open, so markThreadRead can never
   * reach them and the white icon burns forever. Returns how many it cleared. */
  int32_t markAllRead();

  void setSent(MessageData&);
  void setDelivered(const char* fromUri, const char* toUri, unsigned long time, hash_t hash);

protected:
  static const bool INCOMING = true;
  static const bool SENT = false;
  static const int  PARTITION_SIZE = 100;       // maximum number of messages in a partition

  static constexpr const char* indexFile = "/msg_index.ini";
  static constexpr const char* partitionFileFormat = "/msg_%05d.ini";

  IniFile index;

  MessagesArray preloaded;
  bool preloadedIncoming;
  int32_t preloadedRangeStart;
  int32_t preloadedRangeEnd;      // past last element

  // Cached partitions
  IniFile part1;
  IniFile part2;    // consecutive to part1

  bool loaded = false;

  int32_t countAll(bool incoming);
  /* Invalidate the part1/part2 partition caches and the preloaded window.
   * Required around any code that writes a partition FILE directly — see the
   * comment on the definition; a stale cache silently undid such writes. */
  void dropPartitionCaches();
  bool loadPartition(IniFile& part, int num);
  bool findMessage(MessageData& data, IniFile*& part, int32_t& section);   // part: OUT, points at part1 or part2
};

class Storage : public Preferences {
public:
  Storage();

  // Names of all the configs files
  static constexpr const char* ConfigsFile = "/configs.ini";        // critical
  static constexpr const char* PhonebookFile = "/phonebook.ini";    // critical

  // Phonebook
  CriticalFile phonebook;       // new format
  Phonebook phonebookOld;       // old format
  bool loadPhonebook();
  bool loadPhonebookOld();
  static int phonebookCompare(NanoIni::Section** a, NanoIni::Section** b);  // by name

  // Messages database
  Messages messages;
  static int messageCompare(NanoIni::Section** a, NanoIni::Section** b);              // by time
  static int messagePartitionCompare(NanoIni::Section** a, NanoIni::Section** b);     // by time

  // Configs for UdpSenderApp (only NVS)
  void loadUdpSender(const char*& ipDyn, int32_t& port, const char*& textDyn);
  void storeUdpSender(const char* ip, const int32_t port, const char* text);

  // General helpers
  void storeString(const char* pageName, const char* varName, const char* val);
  void loadString(const char* pageName, const char* varName, const char*& val);

  void storeInt(const char* pageName, const char* varName, int32_t val);
  void loadInt(const char* pageName, const char* varName, int32_t& val);

protected:
  // Constants
  static const int defaultMaxText = 100;
  static const unsigned short nvsMaxKeyLen = 15;
  static const unsigned short nvsMaxStringLen = 1984;
  static const int maxAccountFieldSize = 100;
};

#endif // _STORAGE_H_
