#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class BalooProvider : public LauncherProvider {
public:
  BalooProvider();
  ~BalooProvider() override;

  [[nodiscard]] std::string_view prefix() const override { return "/find"; }
  [[nodiscard]] std::string_view id() const override { return "Baloo"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "file-search"; }

  [[nodiscard]] bool includeInGlobalSearch() const override { return true; }

  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }

  void initialize() override;
  void reset() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  void startSearch(const std::string& query) const;

  std::string m_binPath;
  std::function<void()> m_onResultsChanged;

  mutable std::vector<LauncherResult> m_cache;
  mutable std::string m_lastQuery;
  mutable std::string m_resultsQuery;
  mutable Timer m_timer;
  mutable std::shared_ptr<std::atomic<bool>> m_currentCancel;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
