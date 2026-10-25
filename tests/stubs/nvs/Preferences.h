#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nvs_mock {
extern std::map<std::string, std::vector<uint8_t> > records;
extern std::vector<std::string> writes;
extern bool failWrites;
}
class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  bool isKey(const char* key) { return nvs_mock::records.count(key) != 0; }
  size_t getBytesLength(const char* key) { return nvs_mock::records[key].size(); }
  size_t getBytes(const char* key, void* data, size_t size) {
    if (!isKey(key)) return 0;
    const auto& value = nvs_mock::records[key];
    const size_t length = std::min(size, value.size());
    if (length) std::memcpy(data, value.data(), length);
    return length;
  }
  size_t putBytes(const char* key, const void* data, size_t size) {
    if (nvs_mock::failWrites) return 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    nvs_mock::records[key] = std::vector<uint8_t>(bytes, bytes + size);
    nvs_mock::writes.push_back(key);
    return size;
  }
  uint8_t getUChar(const char* key, uint8_t fallback) {
    return isKey(key) && getBytesLength(key) == 1 ? nvs_mock::records[key][0] : fallback;
  }
  size_t putUChar(const char* key, uint8_t value) { return putBytes(key, &value, 1); }
  bool getBool(const char* key, bool fallback) { return getUChar(key, fallback) != 0; }
  size_t putBool(const char* key, bool value) { return putUChar(key, value); }
  bool remove(const char* key) { return nvs_mock::records.erase(key) != 0; }
};
