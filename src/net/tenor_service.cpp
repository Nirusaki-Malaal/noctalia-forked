#include "net/tenor_service.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace {

  constexpr Logger kLog("tenor");

  std::string urlEncode(std::string_view value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (const char c : value) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
        escaped << c;
      } else {
        escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
      }
    }

    return escaped.str();
  }

  std::string buildUrl(const std::string& base, const std::vector<std::pair<std::string, std::string>>& params) {
    if (params.empty()) {
      return base;
    }
    std::string url = base;
    char separator = base.contains('?') ? '&' : '?';
    for (const auto& [k, v] : params) {
      url += separator;
      url += urlEncode(k);
      url += '=';
      url += urlEncode(v);
      separator = '&';
    }
    return url;
  }

  std::string hashString(std::string_view str) {
    std::size_t h = 14695981039346656037ULL;
    for (char c : str) {
      h ^= static_cast<std::size_t>(static_cast<unsigned char>(c));
      h *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << h;
    return ss.str();
  }

} // namespace

TenorService::TenorService(HttpClient* http, ConfigService* config) : m_http(http), m_config(config) {
  const char* xdgCache = std::getenv("XDG_CACHE_HOME");
  if (xdgCache != nullptr && xdgCache[0] != '\0') {
    m_cacheDir = std::filesystem::path(xdgCache) / "noctalia" / "tenor";
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      m_cacheDir = std::filesystem::path(home) / ".cache" / "noctalia" / "tenor";
    } else {
      m_cacheDir = "/tmp/noctalia_tenor";
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(m_cacheDir, ec);
}

std::filesystem::path TenorService::cacheDir() const { return m_cacheDir; }

std::filesystem::path TenorService::localPathForUrl(const std::string& url) const {
  if (url.empty()) {
    return {};
  }
  std::string ext = ".gif";
  if (url.ends_with(".png") || url.contains(".png?")) {
    ext = ".png";
  } else if (url.ends_with(".webp") || url.contains(".webp?")) {
    ext = ".webp";
  } else if (url.ends_with(".mp4") || url.contains(".mp4?")) {
    ext = ".mp4";
  }
  return m_cacheDir / (hashString(url) + ext);
}

void TenorService::searchGifs(const std::string& query, std::size_t limit, MediaListCallback callback) {
  std::vector<std::pair<std::string, std::string>> params;
  params.emplace_back("q", query);
  params.emplace_back("limit", std::to_string(limit));

  std::string tenorKey;
  if (m_config != nullptr) {
    tenorKey = m_config->config().shell.tenorApiKey;
  }
  if (tenorKey.empty()) {
    const char* envKey = std::getenv("TENOR_API_KEY");
    if (envKey != nullptr) {
      tenorKey = envKey;
    }
  }

  std::string giphyKey;
  if (m_config != nullptr) {
    giphyKey = m_config->config().shell.giphyApiKey;
  }
  if (giphyKey.empty()) {
    const char* envKey = std::getenv("GIPHY_API_KEY");
    if (envKey != nullptr) {
      giphyKey = envKey;
    }
  }

  if (tenorKey.empty() && !giphyKey.empty()) {
    queryGiphy("https://api.giphy.com/v1/gifs/search", params, false, std::move(callback));
  } else {
    queryTenor("https://tenor.googleapis.com/v2/search", params, false, std::move(callback));
  }
}

void TenorService::searchStickers(const std::string& query, std::size_t limit, MediaListCallback callback) {
  std::vector<std::pair<std::string, std::string>> params;
  params.emplace_back("q", query);
  params.emplace_back("limit", std::to_string(limit));
  params.emplace_back("searchfilter", "sticker");

  std::string tenorKey;
  if (m_config != nullptr) {
    tenorKey = m_config->config().shell.tenorApiKey;
  }
  if (tenorKey.empty()) {
    const char* envKey = std::getenv("TENOR_API_KEY");
    if (envKey != nullptr) {
      tenorKey = envKey;
    }
  }

  std::string giphyKey;
  if (m_config != nullptr) {
    giphyKey = m_config->config().shell.giphyApiKey;
  }
  if (giphyKey.empty()) {
    const char* envKey = std::getenv("GIPHY_API_KEY");
    if (envKey != nullptr) {
      giphyKey = envKey;
    }
  }

  if (tenorKey.empty() && !giphyKey.empty()) {
    queryGiphy("https://api.giphy.com/v1/stickers/search", params, true, std::move(callback));
  } else {
    queryTenor("https://tenor.googleapis.com/v2/search", params, true, std::move(callback));
  }
}

void TenorService::featuredGifs(std::size_t limit, MediaListCallback callback) {
  std::vector<std::pair<std::string, std::string>> params;
  params.emplace_back("limit", std::to_string(limit));

  std::string tenorKey;
  if (m_config != nullptr) {
    tenorKey = m_config->config().shell.tenorApiKey;
  }
  if (tenorKey.empty()) {
    const char* envKey = std::getenv("TENOR_API_KEY");
    if (envKey != nullptr) {
      tenorKey = envKey;
    }
  }

  std::string giphyKey;
  if (m_config != nullptr) {
    giphyKey = m_config->config().shell.giphyApiKey;
  }
  if (giphyKey.empty()) {
    const char* envKey = std::getenv("GIPHY_API_KEY");
    if (envKey != nullptr) {
      giphyKey = envKey;
    }
  }

  if (tenorKey.empty() && !giphyKey.empty()) {
    queryGiphy("https://api.giphy.com/v1/gifs/trending", params, false, std::move(callback));
  } else {
    queryTenor("https://tenor.googleapis.com/v2/featured", params, false, std::move(callback));
  }
}

void TenorService::featuredStickers(std::size_t limit, MediaListCallback callback) {
  std::vector<std::pair<std::string, std::string>> params;
  params.emplace_back("limit", std::to_string(limit));
  params.emplace_back("searchfilter", "sticker");

  std::string tenorKey;
  if (m_config != nullptr) {
    tenorKey = m_config->config().shell.tenorApiKey;
  }
  if (tenorKey.empty()) {
    const char* envKey = std::getenv("TENOR_API_KEY");
    if (envKey != nullptr) {
      tenorKey = envKey;
    }
  }

  std::string giphyKey;
  if (m_config != nullptr) {
    giphyKey = m_config->config().shell.giphyApiKey;
  }
  if (giphyKey.empty()) {
    const char* envKey = std::getenv("GIPHY_API_KEY");
    if (envKey != nullptr) {
      giphyKey = envKey;
    }
  }

  if (tenorKey.empty() && !giphyKey.empty()) {
    queryGiphy("https://api.giphy.com/v1/stickers/trending", params, true, std::move(callback));
  } else {
    queryTenor("https://tenor.googleapis.com/v2/featured", params, true, std::move(callback));
  }
}

void TenorService::queryTenor(
    const std::string& endpoint, const std::vector<std::pair<std::string, std::string>>& params, bool isSticker,
    MediaListCallback callback
) {
  if (m_http == nullptr) {
    callback({});
    return;
  }

  std::string key;
  if (m_config != nullptr) {
    key = m_config->config().shell.tenorApiKey;
  }
  if (key.empty()) {
    const char* envKey = std::getenv("TENOR_API_KEY");
    if (envKey != nullptr) {
      key = envKey;
    }
  }
  if (key.empty()) {
    key = "AIzaSyCZt6SSh5VgVPzD9fhyzG1DprdPRhtoaR4";
  }

  std::string filter = "medium";
  if (m_config != nullptr && !m_config->config().shell.tenorContentFilter.empty()) {
    filter = m_config->config().shell.tenorContentFilter;
  }

  auto fullParams = params;
  fullParams.emplace_back("key", key);
  fullParams.emplace_back("client_key", "tenor_web");
  fullParams.emplace_back("contentfilter", filter);
  fullParams.emplace_back("media_filter", "gif,tinygif,nanogif,png,webp");

  std::string url = buildUrl(endpoint, fullParams);

  HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.followRedirects = true;

  m_http->request(std::move(req), [callback = std::move(callback), isSticker](HttpResponse res) {
    if (!res.transportOk || res.status != 200) {
      kLog.warn("Tenor request failed: status={} body={}", res.status, res.body);
      callback({});
      return;
    }

    std::vector<TenorMediaItem> results;
    try {
      auto json = nlohmann::json::parse(res.body);
      if (json.contains("results") && json["results"].is_array()) {
        for (const auto& item : json["results"]) {
          TenorMediaItem media;
          media.id = item.value("id", "");
          media.title = item.value("content_description", item.value("title", ""));
          media.isSticker = isSticker;

          if (item.contains("media_formats") && item["media_formats"].is_object()) {
            const auto& formats = item["media_formats"];
            if (formats.contains("tinygif") && formats["tinygif"].contains("url")) {
              media.previewUrl = formats["tinygif"]["url"].get<std::string>();
            } else if (formats.contains("nanogif") && formats["nanogif"].contains("url")) {
              media.previewUrl = formats["nanogif"]["url"].get<std::string>();
            }

            if (formats.contains("gif") && formats["gif"].contains("url")) {
              media.fullUrl = formats["gif"]["url"].get<std::string>();
              if (formats["gif"].contains("dims") && formats["gif"]["dims"].is_array() && formats["gif"]["dims"].size() >= 2) {
                media.width = formats["gif"]["dims"][0].get<float>();
                media.height = formats["gif"]["dims"][1].get<float>();
              }
            } else if (formats.contains("mediumgif") && formats["mediumgif"].contains("url")) {
              media.fullUrl = formats["mediumgif"]["url"].get<std::string>();
            }

            if (isSticker && formats.contains("png_transparent") && formats["png_transparent"].contains("url")) {
              media.fullUrl = formats["png_transparent"]["url"].get<std::string>();
              if (media.previewUrl.empty()) {
                media.previewUrl = media.fullUrl;
              }
            }
          }

          if (media.previewUrl.empty()) {
            media.previewUrl = media.fullUrl;
          }
          if (media.fullUrl.empty()) {
            media.fullUrl = media.previewUrl;
          }

          if (!media.previewUrl.empty() && !media.fullUrl.empty()) {
            results.push_back(std::move(media));
          }
        }
      }
    } catch (const std::exception& e) {
      kLog.warn("Failed to parse Tenor JSON response: {}", e.what());
    }

    callback(std::move(results));
  });
}

void TenorService::queryGiphy(
    const std::string& endpoint, const std::vector<std::pair<std::string, std::string>>& params, bool isSticker,
    MediaListCallback callback
) {
  if (m_http == nullptr) {
    callback({});
    return;
  }

  std::string key;
  if (m_config != nullptr) {
    key = m_config->config().shell.giphyApiKey;
  }
  if (key.empty()) {
    const char* envKey = std::getenv("GIPHY_API_KEY");
    if (envKey != nullptr) {
      key = envKey;
    }
  }

  auto fullParams = params;
  if (!key.empty()) {
    fullParams.emplace_back("api_key", key);
  }

  std::string url = buildUrl(endpoint, fullParams);

  HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.followRedirects = true;

  m_http->request(std::move(req), [callback = std::move(callback), isSticker](HttpResponse res) {
    if (!res.transportOk || res.status != 200) {
      kLog.warn("GIPHY request failed: status={} body={}", res.status, res.body);
      callback({});
      return;
    }

    std::vector<TenorMediaItem> results;
    try {
      auto json = nlohmann::json::parse(res.body);
      if (json.contains("data") && json["data"].is_array()) {
        for (const auto& item : json["data"]) {
          TenorMediaItem media;
          media.id = item.value("id", "");
          media.title = item.value("title", "");
          media.isSticker = isSticker;

          if (item.contains("images") && item["images"].is_object()) {
            const auto& images = item["images"];
            if (images.contains("fixed_height_small") && images["fixed_height_small"].contains("url")) {
              media.previewUrl = images["fixed_height_small"]["url"].get<std::string>();
            } else if (images.contains("downsized_small") && images["downsized_small"].contains("url")) {
              media.previewUrl = images["downsized_small"]["url"].get<std::string>();
            }

            if (images.contains("original") && images["original"].contains("url")) {
              media.fullUrl = images["original"]["url"].get<std::string>();
              if (images["original"].contains("width") && images["original"].contains("height")) {
                media.width = std::stof(images["original"].value("width", "0"));
                media.height = std::stof(images["original"].value("height", "0"));
              }
            }
          }

          if (media.previewUrl.empty()) {
            media.previewUrl = media.fullUrl;
          }
          if (media.fullUrl.empty()) {
            media.fullUrl = media.previewUrl;
          }

          if (!media.previewUrl.empty() && !media.fullUrl.empty()) {
            results.push_back(std::move(media));
          }
        }
      }
    } catch (const std::exception& e) {
      kLog.warn("Failed to parse GIPHY JSON response: {}", e.what());
    }

    callback(std::move(results));
  });
}

void TenorService::ensureLocalPreview(TenorMediaItem& item, DownloadCallback callback) {
  if (item.previewUrl.empty()) {
    callback(std::nullopt);
    return;
  }

  const auto path = localPathForUrl(item.previewUrl);
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
    item.localPreviewPath = path.string();
    callback(path);
    return;
  }

  if (m_http == nullptr) {
    callback(std::nullopt);
    return;
  }

  m_http->download(item.previewUrl, path, [path, callback = std::move(callback)](bool success) {
    if (success) {
      callback(path);
    } else {
      callback(std::nullopt);
    }
  });
}

void TenorService::fetchMediaBytes(const std::string& url, BytesCallback callback) {
  if (url.empty()) {
    callback({});
    return;
  }

  const auto path = localPathForUrl(url);
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
      const auto size = file.tellg();
      file.seekg(0, std::ios::beg);
      std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
      if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        callback(std::move(buffer));
        return;
      }
    }
  }

  if (m_http == nullptr) {
    callback({});
    return;
  }

  m_http->download(url, path, [path, callback = std::move(callback)](bool success) {
    if (!success) {
      callback({});
      return;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      callback({});
      return;
    }
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
      callback(std::move(buffer));
    } else {
      callback({});
    }
  });
}
