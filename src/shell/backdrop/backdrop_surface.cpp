#include "shell/backdrop/backdrop_surface.h"

#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "wayland/wayland_connection.h"
#include "core/deferred_call.h"

#include <stdexcept>
#include <cstring>
#include <wayland-client-protocol.h>

BackdropSurface::~BackdropSurface() {
  m_wallpaperRenderer.makeCurrent();
  m_layer.destroy();
}

bool BackdropSurface::createWlSurface() {
  m_surface = wl_compositor_create_surface(m_connection.compositor());
  if (m_surface == nullptr) {
    return false;
  }

  initializeSurfaceScaleProtocol();

  if (m_shared == nullptr) {
    throw std::runtime_error("BackdropSurface requires a GlSharedContext");
  }
  m_wallpaperRenderer.bind(*m_shared, m_surface);
  return true;
}

void BackdropSurface::onConfigure(std::uint32_t width, std::uint32_t height) {
  const auto bw = bufferWidthFor(width);
  const auto bh = bufferHeightFor(height);

  m_bufW = bw;
  m_bufH = bh;

  m_wallpaperRenderer.resize(bw, bh, width, height);
  m_layer.invalidate();

  Surface::onConfigure(width, height);
}

void BackdropSurface::onScaleChanged() {
  if (width() == 0 || height() == 0) {
    return;
  }
  onConfigure(width(), height());
}

void BackdropSurface::render() {
  auto* backend = m_wallpaperRenderer.backend();
  if (m_surface == nullptr || backend == nullptr) {
    return;
  }

  m_wallpaperRenderer.makeCurrent();
  {
    std::scoped_lock lock(m_videoMutex);
    if (m_newVideoFrame && !m_videoFrame.empty() && m_videoW > 0 && m_videoH > 0) {
      if (!m_videoTex.valid() || m_videoW != m_videoTex.width || m_videoH != m_videoTex.height) {
        if (m_videoTex.valid()) {
          m_wallpaperRenderer.backend()->textureManager().unload(m_videoTex);
        }
        m_videoTex = m_wallpaperRenderer.backend()->textureManager().loadFromRgba(m_videoFrame.data(), m_videoW, m_videoH);
      } else {
        m_wallpaperRenderer.backend()->textureManager().updateSubImage(m_videoTex, m_videoFrame.data(), 0, 0, m_videoW, m_videoH, TextureDataFormat::Rgba);
      }
      m_newVideoFrame = false;
      m_layer.invalidate();
      
      // Keep it as the active wallpaper state
      m_wallpaperRenderer.setTransitionState(
          m_videoTex.id, {}, static_cast<float>(m_videoW), static_cast<float>(m_videoH), 0.0f, 0.0f, 0.0f, WallpaperTransition::Fade, WallpaperFillMode::Crop, TransitionParams{}
      );
    }
  }

  m_layer.resize(*backend, m_bufW, m_bufH);

  if (!m_layer.valid()) {
    return;
  }

  static constexpr int kBlurRounds = 3;
  const auto options = BackdropPostProcessOptions{
      .blurRadius = m_blurIntensity * 40.0f,
      .blurRounds = kBlurRounds,
      .tintColor = rgba(m_tintR, m_tintG, m_tintB, 1.0f),
      .tintIntensity = m_tintIntensity,
  };

  if (!m_layer.dirty()) {
    return;
  }

  m_layer.ensure([&](RenderFramebuffer& target) {
    auto* scratch = m_layer.scratch();
    if (scratch == nullptr) {
      return;
    }
    m_wallpaperRenderer.renderBackdropContent(target, *scratch, options);
  });

  requestFrame();
  m_wallpaperRenderer.presentTexture(m_layer.texture());
}

void BackdropSurface::setBlurIntensity(float v) noexcept {
  if (m_blurIntensity == v) {
    return;
  }
  m_blurIntensity = v;
  m_layer.invalidate();
}

void BackdropSurface::setTintIntensity(float v) noexcept {
  if (m_tintIntensity == v) {
    return;
  }
  m_tintIntensity = v;
  m_layer.invalidate();
}

void BackdropSurface::setTintColor(float r, float g, float b) noexcept {
  if (m_tintR == r && m_tintG == g && m_tintB == b) {
    return;
  }
  m_tintR = r;
  m_tintG = g;
  m_tintB = b;
  m_layer.invalidate();
}

void BackdropSurface::setWallpaperState(TextureId tex, float imgW, float imgH, WallpaperFillMode fillMode) {
  m_wallpaperRenderer.setTransitionState(
      tex, {}, imgW, imgH, 0.0f, 0.0f, 0.0f, WallpaperTransition::Fade, fillMode, TransitionParams{}
  );
  m_layer.invalidate();
}

void BackdropSurface::playVideo(const std::string& path) {
  if (!m_videoPlayer) {
    m_videoPlayer = std::make_unique<VideoPlayer>();
  }
  m_videoPlayer->setFrameCallback([this](const uint8_t* rgba, int w, int h) {
    {
      std::scoped_lock lock(m_videoMutex);
      const size_t bytes = static_cast<size_t>(w * h * 4);
      if (m_videoFrame.size() != bytes) {
        m_videoFrame.resize(bytes);
      }
      std::memcpy(m_videoFrame.data(), rgba, bytes);
      m_videoW = w;
      m_videoH = h;
      m_newVideoFrame = true;
    }
    DeferredCall::callLater([this]() {
      requestRedraw();
    });
  });
  m_videoPlayer->load(path);
  m_videoPlayer->play();
}

void BackdropSurface::stopVideo() {
  if (m_videoPlayer) {
    m_videoPlayer->stop();
    m_videoPlayer.reset();
  }
  if (m_videoTex.valid()) {
    m_wallpaperRenderer.makeCurrent();
    m_wallpaperRenderer.backend()->textureManager().unload(m_videoTex);
    m_videoTex = TextureHandle{};
  }
}

void BackdropSurface::onGpuResourcesInvalidated() {
  m_wallpaperRenderer.invalidateGpuResources();
  m_layer.destroy();
  requestRedraw();
}

void BackdropSurface::prepareForGraphicsReset() noexcept {
  m_layer.abandon();
  m_wallpaperRenderer.prepareForGraphicsReset();
}

void BackdropSurface::restoreAfterGraphicsReset() {
  if (m_shared == nullptr) {
    throw std::runtime_error("BackdropSurface requires a GlSharedContext");
  }
  m_wallpaperRenderer.restoreAfterGraphicsReset(*m_shared);
  m_layer.invalidate();
}

void BackdropSurface::finishGraphicsResetRecovery() noexcept { m_wallpaperRenderer.finishGraphicsResetRecovery(); }
