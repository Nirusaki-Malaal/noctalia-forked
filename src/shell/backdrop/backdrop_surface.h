#pragma once

#include "render/core/cached_layer.h"
#include "render/wallpaper_renderer.h"
#include "wayland/layer_surface.h"
#include "shell/backdrop/video_player.h"

#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>

class GlSharedContext;

class BackdropSurface : public LayerSurface {
public:
  using LayerSurface::LayerSurface;
  ~BackdropSurface() override;

  void setSharedGl(GlSharedContext* shared) noexcept { m_shared = shared; }
  void setBlurIntensity(float v) noexcept;
  void setTintIntensity(float v) noexcept;
  void setTintColor(float r, float g, float b) noexcept;
  void setWallpaperState(TextureId tex, float imgW, float imgH, WallpaperFillMode fillMode);
  void playVideo(const std::string& path);
  void stopVideo();
  void onGpuResourcesInvalidated();
  void prepareForGraphicsReset() noexcept;
  void restoreAfterGraphicsReset();
  void finishGraphicsResetRecovery() noexcept;

  [[nodiscard]] WallpaperRenderer* wallpaperRenderer() noexcept { return &m_wallpaperRenderer; }

protected:
  bool createWlSurface() override;
  void onConfigure(std::uint32_t width, std::uint32_t height) override;
  void render() override;
  void onScaleChanged() override;

private:
  WallpaperRenderer m_wallpaperRenderer;
  std::unique_ptr<VideoPlayer> m_videoPlayer;
  std::vector<uint8_t> m_videoFrame;
  std::mutex m_videoMutex;
  bool m_newVideoFrame = false;
  int m_videoW = 0;
  int m_videoH = 0;
  TextureHandle m_videoTex;
  CachedLayer m_layer;

  std::uint32_t m_bufW = 0;
  std::uint32_t m_bufH = 0;

  GlSharedContext* m_shared = nullptr;
  float m_blurIntensity = 0.5F;
  float m_tintIntensity = 0.3F;
  float m_tintR = 0.0F;
  float m_tintG = 0.0F;
  float m_tintB = 0.0F;
};
