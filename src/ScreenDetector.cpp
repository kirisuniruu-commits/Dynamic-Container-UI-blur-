#include "ScreenDetector.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>
#include <sys/uio.h>
#include <unistd.h>

namespace {

bool safeRead(uintptr_t addr, void *out, size_t n) {
  static bool useSyscall = true;
  if (addr < 0x1000) return false;
  if (useSyscall) {
    iovec local{out, n};
    iovec remote{reinterpret_cast<void *>(addr), n};
    const ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (r == static_cast<ssize_t>(n)) return true;
    if (r < 0 && (errno == ENOSYS || errno == EPERM)) {
      useSyscall = false;
    } else {
      return false;
    }
  }
  std::memcpy(out, reinterpret_cast<const void *>(addr), n);
  return true;
}

bool readStdString(uintptr_t addr, std::string &out) {
  unsigned char raw[24];
  if (!safeRead(addr, raw, sizeof(raw))) return false;
  if ((raw[0] & 1) == 0) {
    const size_t n = raw[0] >> 1;
    if (n == 0 || n > 22) return false;
    out.assign(reinterpret_cast<char *>(raw) + 1, n);
  } else {
    size_t size = 0;
    uintptr_t ptr = 0;
    std::memcpy(&size, raw + 8, sizeof(size));
    std::memcpy(&ptr, raw + 16, sizeof(ptr));
    if (size == 0 || size > 128) return false;
    out.resize(size);
    if (!safeRead(ptr, out.data(), size)) return false;
  }
  return std::all_of(out.begin(), out.end(),
                     [](unsigned char c) { return c >= 0x20 && c < 0x7f; });
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool readJson(const std::filesystem::path &file, nlohmann::json &out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out = nlohmann::json::parse(in, nullptr, false);
  return !out.is_discarded();
}

} // namespace

bool ScreenDetector::loadRules(const std::filesystem::path &file) {
  nlohmann::json j;
  mRules.clear();
  if (!readJson(file, j) || !j.contains("rules") || !j["rules"].is_array()) return false;
  for (const auto &r : j["rules"]) {
    HookRule rule;
    rule.id = r.value("id", "");
    rule.signature = r.value("signature", "");
    if (r.contains("name_offsets") && r["name_offsets"].is_array())
      for (const auto &o : r["name_offsets"])
        if (o.is_number_unsigned()) rule.nameOffsets.push_back(o.get<size_t>());
    if (!rule.signature.empty() && !rule.nameOffsets.empty()) mRules.push_back(std::move(rule));
  }
  return !mRules.empty();
}

bool ScreenDetector::loadScreens(const std::filesystem::path &file) {
  nlohmann::json j;
  mEntries.clear();
  if (!readJson(file, j) || !j.contains("groups") || !j["groups"].is_object()) return false;

  static const std::pair<const char *, ScreenGroup> order[] = {
      {"storage", ScreenGroup::Storage},       {"specialty", ScreenGroup::Specialty},
      {"processing", ScreenGroup::Processing}, {"crafting", ScreenGroup::Crafting},
      {"chests", ScreenGroup::Chests},         {"inventory", ScreenGroup::Inventory}};
  for (const auto &[key, group] : order) {
    if (!j["groups"].contains(key) || !j["groups"][key].is_array()) continue;
    Entry e{group, {}};
    for (const auto &t : j["groups"][key])
      if (t.is_string()) e.tokens.push_back(lower(t.get<std::string>()));
    mEntries.push_back(std::move(e));
  }
  return !mEntries.empty();
}

ScreenGroup ScreenDetector::classify(std::string_view screenName) const {
  const std::string name = lower(std::string(screenName));
  for (const auto &e : mEntries)
    for (const auto &t : e.tokens)
      if (name.find(t) != std::string::npos) return e.group;
  return ScreenGroup::None;
}

bool ScreenDetector::readScreenName(void *self, const HookRule &rule, std::string &out) const {
  if (!self || rule.nameOffsets.empty()) return false;
  uintptr_t p = reinterpret_cast<uintptr_t>(self);
  for (size_t i = 0; i + 1 < rule.nameOffsets.size(); ++i) {
    uintptr_t next = 0;
    if (!safeRead(p + rule.nameOffsets[i], &next, sizeof(next)) || next == 0) return false;
    p = next;
  }
  return readStdString(p + rule.nameOffsets.back(), out);
}
