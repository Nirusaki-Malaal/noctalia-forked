#pragma once

#include "net/http_client.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ConfigService;

struct TenorMediaItem {
  std::string id;
  std::string title;
  std::string previewUrl;      // Small gif/webp thumbnail
  std::string fullUrl;         // Full animated gif or png
  float width = 0.0f;
  float height = 0.0f;
  bool isSticker = false;
  std::string localPreviewPath;
  std::string localFullPath;
};

class TenorService {
public:
  using MediaListCallback = std::function<void(std::vector<TenorMediaItem>)>;
  using DownloadCallback = std::function<void(std::optional<std::filesystem::path>)>;
  using BytesCallback = std::function<void(std::vector<std::uint8_t>)>;

  explicit TenorService(HttpClient* http, ConfigService* config);
  ~TenorService() = default;

  TenorService(const TenorService&) = delete;
  TenorService& operator=(const TenorService&) = delete;

  void searchGifs(const std::string& query, std::size_t limit, MediaListCallback callback);
  void searchStickers(const std::string& query, std::size_t limit, MediaListCallback callback);
  void featuredGifs(std::size_t limit, MediaListCallback callback);
  void featuredStickers(std::size_t limit, MediaListCallback callback);

  void ensureLocalPreview(TenorMediaItem& item, DownloadCallback callback);
  void fetchMediaBytes(const std::string& url, BytesCallback callback);

  [[nodiscard]] std::filesystem::path cacheDir() const;
  [[nodiscard]] std::filesystem::path localPathForUrl(const std::string& url) const;

private:
  void queryTenor(
      const std::string& endpoint, const std::vector<std::pair<std::string, std::string>>& params, bool isSticker,
      MediaListCallback callback
  );
  void queryGiphy(
      const std::string& endpoint, const std::vector<std::pair<std::string, std::string>>& params, bool isSticker,
      MediaListCallback callback
  );

  HttpClient* m_http = nullptr;
  ConfigService* m_config = nullptr;
  std::filesystem::path m_cacheDir;
};
