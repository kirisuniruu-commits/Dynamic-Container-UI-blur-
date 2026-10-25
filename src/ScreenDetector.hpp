#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

enum class ScreenGroup { None, Inventory, Chests, Crafting, Processing, Specialty, Storage };

struct HookRule {
  std::string id;
  std::string signature;
  std::vector<size_t> nameOffsets;
};

class ScreenDetector {
public:
  bool loadRules(const std::filesystem::path &file);
  bool loadScreens(const std::filesystem::path &file);

  const std::vector<HookRule> &rules() const { return mRules; }
  ScreenGroup classify(std::string_view screenName) const;
  bool readScreenName(void *self, const HookRule &rule, std::string &out) const;

private:
  struct Entry {
    ScreenGroup group;
    std::vector<std::string> tokens;
  };
  std::vector<HookRule> mRules;
  std::vector<Entry> mEntries;
};
