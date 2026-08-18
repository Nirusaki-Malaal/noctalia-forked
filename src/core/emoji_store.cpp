#include "core/emoji_store.h"

#include "core/files/resource_paths.h"
#include "util/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

EmojiStore& EmojiStore::instance() {
  static EmojiStore s_instance;
  return s_instance;
}

void EmojiStore::loadIfNeeded() {
  if (m_loaded) {
    return;
  }
  m_loaded = true;

  const std::filesystem::path path = paths::assetPath("emoji.json");
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }

  try {
    auto json = nlohmann::json::parse(file);
    if (!json.is_array()) {
      return;
    }

    m_items.reserve(json.size());
    for (const auto& item : json) {
      EmojiItem entry;
      entry.emoji = item.value("emoji", "");
      entry.name = item.value("name", "");
      entry.nameLower = StringUtils::toLower(entry.name);
      entry.category = item.value("category", "");

      if (item.contains("keywords") && item["keywords"].is_array()) {
        for (const auto& kw : item["keywords"]) {
          if (kw.is_string()) {
            entry.keywords.push_back(StringUtils::toLower(kw.get<std::string>()));
          }
        }
      }

      if (!entry.emoji.empty() && !entry.name.empty()) {
        m_items.push_back(std::move(entry));
      }
    }
  } catch (...) {
    // Parse error ignored
  }
}

const EmojiItem* EmojiStore::get(std::size_t index) const {
  if (index < m_items.size()) {
    return &m_items[index];
  }
  return nullptr;
}

std::vector<std::size_t> EmojiStore::search(std::string_view query) const {
  if (query.empty()) {
    std::vector<std::size_t> indices(m_items.size());
    for (std::size_t i = 0; i < m_items.size(); ++i) {
      indices[i] = i;
    }
    return indices;
  }

  std::string lowerQuery = StringUtils::toLower(query);
  struct ScoredIndex {
    int score = 0;
    std::size_t index = 0;
    bool operator<(const ScoredIndex& other) const { return score > other.score; }
  };

  std::vector<ScoredIndex> matches;

  for (std::size_t i = 0; i < m_items.size(); ++i) {
    const auto& item = m_items[i];
    int score = 0;

    if (item.nameLower == lowerQuery) {
      score += 100;
    } else if (item.nameLower.starts_with(lowerQuery)) {
      score += 50;
    } else if (item.nameLower.contains(lowerQuery)) {
      score += 30;
    }

    for (const auto& kw : item.keywords) {
      if (kw == lowerQuery) {
        score += 40;
      } else if (kw.starts_with(lowerQuery)) {
        score += 20;
      } else if (kw.contains(lowerQuery)) {
        score += 10;
      }
    }

    if (score > 0) {
      matches.push_back({score, i});
    }
  }

  std::sort(matches.begin(), matches.end());
  std::vector<std::size_t> results;
  results.reserve(matches.size());
  for (const auto& m : matches) {
    results.push_back(m.index);
  }
  return results;
}
