#include "launcher/baloo_provider.h"

#include "core/deferred_call.h"
#include "core/process.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace {
  std::string_view iconForPath(const std::filesystem::path& path) {
    try {
      if (std::filesystem::is_directory(path)) {
        return "folder";
      }
    } catch (...) {
      // Ignore filesystem permission or other errors
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    if (ext == ".txt" || ext == ".md" || ext == ".rst" || ext == ".log") {
      return "text-x-generic";
    }
    if (ext == ".pdf") {
      return "document";
    }
    if (ext == ".doc" || ext == ".docx" || ext == ".odt" || ext == ".rtf") {
      return "x-office-document";
    }
    if (ext == ".xls" || ext == ".xlsx" || ext == ".ods") {
      return "x-office-spreadsheet";
    }
    if (ext == ".ppt" || ext == ".pptx" || ext == ".odp") {
      return "x-office-presentation";
    }
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".svg" || ext == ".bmp" || ext == ".webp") {
      return "image-x-generic";
    }
    if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".m4a" || ext == ".wma" || ext == ".opus") {
      return "audio-x-generic";
    }
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".webm" || ext == ".flv" || ext == ".mpeg") {
      return "video-x-generic";
    }
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".bz2" || ext == ".rar" || ext == ".7z" || ext == ".zst") {
      return "package-x-generic";
    }
    if (ext == ".sh" || ext == ".py" || ext == ".js" || ext == ".cpp" || ext == ".h" || ext == ".c" || ext == ".rs" || ext == ".go" || ext == ".rb" || ext == ".json" || ext == ".toml" || ext == ".yaml" || ext == ".yml" || ext == ".xml" || ext == ".css" || ext == ".html") {
      return "text-x-script";
    }
    if (ext == ".desktop") {
      return "application-x-desktop";
    }
    return "text-x-generic";
  }
} // namespace

BalooProvider::BalooProvider() {
  if (process::commandExists("baloosearch")) {
    m_binPath = "baloosearch";
  } else if (process::commandExists("baloosearch6")) {
    m_binPath = "baloosearch6";
  }
}

BalooProvider::~BalooProvider() {
  *m_alive = false;
  m_timer.stop();
  if (m_currentCancel) {
    *m_currentCancel = true;
  }
}

std::string BalooProvider::displayName() const {
  return i18n::tr("launcher.providers.baloo.title");
}

void BalooProvider::initialize() {}

void BalooProvider::reset() {
  m_timer.stop();
  if (m_currentCancel) {
    *m_currentCancel = true;
    m_currentCancel = nullptr;
  }
  m_cache.clear();
  m_lastQuery.clear();
  m_resultsQuery.clear();
}

std::vector<LauncherResult> BalooProvider::query(std::string_view text) const {
  if (m_binPath.empty()) {
    return {};
  }

  std::string trimmed = std::string(StringUtils::trim(text));
  if (trimmed.empty()) {
    m_timer.stop();
    if (m_currentCancel) {
      *m_currentCancel = true;
      m_currentCancel = nullptr;
    }
    m_cache.clear();
    m_lastQuery.clear();
    m_resultsQuery.clear();
    return {};
  }

  if (trimmed == m_lastQuery) {
    return m_cache;
  }

  m_lastQuery = trimmed;
  m_timer.stop();
  m_timer.start(std::chrono::milliseconds(150), [this, trimmed]() {
    startSearch(trimmed);
  });

  return m_cache;
}

bool BalooProvider::activate(const LauncherResult& result) {
  return net::openInBrowser(result.id);
}

void BalooProvider::startSearch(const std::string& queryText) const {
  if (m_currentCancel) {
    *m_currentCancel = true;
  }
  m_currentCancel = std::make_shared<std::atomic<bool>>(false);

  std::vector<std::string> args = {m_binPath, queryText};
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, queryText, cancel = m_currentCancel](process::RunResult result) {
    if (*cancel) {
      return;
    }

    std::vector<LauncherResult> newResults;
    if (result.exitCode == 0 && !result.out.empty()) {
      std::stringstream ss(result.out);
      std::string line;
      int index = 0;
      while (std::getline(ss, line) && index < 50) {
        if (line.empty()) {
          continue;
        }

        auto spaceIdx = line.find(' ');
        if (spaceIdx == std::string::npos) {
          continue;
        }

        std::string pathStr = line.substr(spaceIdx + 1);
        while (!pathStr.empty() && (pathStr.back() == '\r' || pathStr.back() == '\n' || pathStr.back() == ' ')) {
          pathStr.pop_back();
        }

        if (pathStr.empty()) {
          continue;
        }

        std::filesystem::path path(pathStr);
        LauncherResult r;
        r.id = pathStr;
        r.title = path.filename().string();
        r.subtitle = path.parent_path().string();
        r.category = "Files";
        r.iconName = std::string(iconForPath(path));
        r.score = 100.0 - (index * 0.5);

        newResults.push_back(std::move(r));
        index++;
      }
    }

    auto alive = std::weak_ptr<bool>(m_alive);
    DeferredCall::callLater([this, alive, queryText, cancel, res = std::move(newResults)]() mutable {
      auto token = alive.lock();
      if (token != nullptr && *token && !*cancel) {
        m_cache = std::move(res);
        m_resultsQuery = queryText;
        if (m_onResultsChanged) {
          m_onResultsChanged();
        }
      }
    });
  };

  process::RunOptions options;
  options.cancel = m_currentCancel;
  options.maxOutputBytes = 65536;

  if (!process::runAsync(args, callbacks, options)) {
    m_currentCancel = nullptr;
  }
}
