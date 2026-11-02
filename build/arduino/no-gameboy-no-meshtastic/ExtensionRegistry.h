#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wiphone {

struct AppRegistration {
  const char* id;
  const char* title;
  const char* group;
  void* (*create)(void* context);
};

struct AppMenuRegistration {
  const char* id;
  const char* title;
  const char* parent;
  int16_t menuId;
};

enum class SettingType : uint8_t {
  Toggle,
  Integer,
  String,
  Enumeration,
};

struct SettingContext {
  void* value;
  void* storage;
};

struct SettingRegistration {
  const char* id;
  const char* title;
  const char* owner;
  const char* storageKey;
  SettingType type;
  void (*load)(void* context);
  void (*save)(void* context);
};

struct AppLaunchContext {
  void* display;
  void* lcd;
  void* state;
  void* header;
  void* footer;
};

template <typename Registration>
class ExtensionRegistry {
public:
  ExtensionRegistry(const Registration* entries, size_t count)
      : entries_(entries), count_(count) {
  }

  size_t count() const {
    return count_;
  }

  const Registration* entries() const {
    return entries_;
  }

  const Registration* findById(const char* id) const {
    if (!id || !entries_) {
      return NULL;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].id) {
        size_t j = 0;
        while (entries_[i].id[j] != '\0' && id[j] == entries_[i].id[j]) {
          ++j;
        }
        if (entries_[i].id[j] == '\0' && id[j] == '\0') {
          return &entries_[i];
        }
      }
    }
    return NULL;
  }

  bool validateUniqueIds(char* error, size_t errorSize) const;

private:
  const Registration* entries_;
  size_t count_;
};

template <typename Registration>
bool ExtensionRegistry<Registration>::validateUniqueIds(char* error, size_t errorSize) const {
  if (error && errorSize > 0) {
    error[0] = '\0';
  }
  for (size_t i = 0; i < count_; ++i) {
    if (!entries_[i].id) {
      if (error && errorSize > 0) {
        const char message[] = "Extension registration has a null id";
        size_t n = sizeof(message) - 1;
        if (n >= errorSize) n = errorSize - 1;
        for (size_t j = 0; j < n; ++j) error[j] = message[j];
        error[n] = '\0';
      }
      return false;
    }
    for (size_t j = i + 1; j < count_; ++j) {
      bool duplicate = entries_[j].id != NULL;
      if (duplicate) {
        size_t k = 0;
        while (entries_[i].id[k] != '\0' &&
               entries_[j].id[k] == entries_[i].id[k]) {
          ++k;
        }
        duplicate = entries_[i].id[k] == '\0' &&
                    entries_[j].id[k] == '\0';
      }
      if (duplicate) {
        if (error && errorSize > 0) {
          const char prefix[] = "Duplicate extension id: ";
          size_t pos = 0;
          while (prefix[pos] && pos + 1 < errorSize) {
            error[pos] = prefix[pos];
            ++pos;
          }
          size_t idPos = 0;
          while (entries_[j].id[idPos] && pos + 1 < errorSize) {
            error[pos++] = entries_[j].id[idPos++];
          }
          error[pos] = '\0';
        }
        return false;
      }
    }
  }
  return true;
}

}  // namespace wiphone
