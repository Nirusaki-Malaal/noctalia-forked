#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct EmojiItem {
  std::string emoji;
  std::string name;
  std::string nameLower;
  std::string category;
  std::vector<std::string> keywords;
};

class EmojiStore {
public:
  static EmojiStore& instance();

  void loadIfNeeded();
  [[nodiscard]] const std::vector<EmojiItem>& all() const noexcept { return m_items; }
  [[nodiscard]] std::vector<std::size_t> search(std::string_view query) const;
  [[nodiscard]] const EmojiItem* get(std::size_t index) const;

private:
  EmojiStore() = default;
  bool m_loaded = false;
  std::vector<EmojiItem> m_items;
};
