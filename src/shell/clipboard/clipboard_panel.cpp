#include "shell/clipboard/clipboard_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/emoji_store.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

  constexpr float kRowHeightEstimate = 46.0F;
  constexpr float kPreviewImageHeight = 280.0F;
  constexpr float kListGlyphSize = 24.0F;
  constexpr float kListThumbSize = 40.0F;
  constexpr float kListPinGlyphSize = 16.0F;
  constexpr std::size_t kListOverscanRows = 3;
  constexpr auto kPreviewPayloadDebounceInterval = std::chrono::milliseconds(75);
  constexpr auto kFilterDebounceInterval = std::chrono::milliseconds(120);
  constexpr auto kGifSearchDebounceInterval = std::chrono::milliseconds(250);
  constexpr Logger kLog("clipboard");

  [[nodiscard]] float listRowHeight(Renderer& renderer, float scale) {
    const TextMetrics title = renderer.measureFont(Style::fontSizeBody * scale, FontWeight::SemiBold);
    const TextMetrics meta = renderer.measureFont(Style::fontSizeCaption * scale, FontWeight::Normal);
    const float textHeight = std::round(title.bottom - title.top) + std::round(meta.bottom - meta.top);
    return std::ceil(std::max(kListThumbSize * scale, textHeight) + Style::spaceXs * scale * 2.0F);
  }

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  void replaceAll(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) {
      return;
    }

    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
      text.replace(pos, needle.size(), replacement);
      pos += replacement.size();
    }
  }

  std::string buildImageActionCommand(std::string command, std::string_view imagePath) {
    const bool hasPathPlaceholder = command.contains("{path}");
    const bool hasStdinPlaceholder = command.contains("{stdin}");
    const std::string quotedPath = StringUtils::shellQuote(imagePath);

    if (hasPathPlaceholder) {
      replaceAll(command, "{path}", quotedPath);
    }
    if (hasStdinPlaceholder) {
      replaceAll(command, "{stdin}", "-");
    }

    if (!hasPathPlaceholder || hasStdinPlaceholder) {
      return "cat -- " + quotedPath + " | " + command;
    }
    return command;
  }

  std::string collapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    bool lastWasSpace = true;
    for (char ch : text) {
      const bool isWhitespace = (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r');
      if (isWhitespace) {
        if (!lastWasSpace) {
          out.push_back(' ');
        }
        lastWasSpace = true;
        continue;
      }
      out.push_back(ch);
      lastWasSpace = false;
    }

    if (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    return out;
  }

  std::string formatBytes(std::size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < std::size(units)) {
      value /= 1024.0;
      ++unitIndex;
    }

    char buffer[32];
    if (unitIndex == 0) {
      std::snprintf(buffer, sizeof(buffer), "%zu %s", bytes, units[unitIndex]);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%.1F %s", value, units[unitIndex]);
    }
    return buffer;
  }

  std::string formatPreviewMeta(const ClipboardEntry& entry, int imageWidth = 0, int imageHeight = 0) {
    std::string meta = formatTimeAgo(entry.capturedAt) + "  •  " + formatBytes(entry.byteSize);
    if (imageWidth > 0 && imageHeight > 0) {
      meta += "  •  " + std::to_string(imageWidth) + "x" + std::to_string(imageHeight);
    }
    return meta;
  }

  std::string entryTitle(const ClipboardEntry& entry) {
    if (!entry.textPreview.empty()) {
      return entry.textPreview;
    }
    if (entry.isImage()) {
      return i18n::tr("clipboard.entry.image");
    }
    return entry.dataMimeType.empty() ? i18n::tr("clipboard.entry.title") : entry.dataMimeType;
  }

  std::string previewTitle(const ClipboardEntry& entry) {
    if (entry.isImage()) {
      return i18n::tr("clipboard.preview.image-title");
    }
    return i18n::tr("clipboard.preview.text-title");
  }

  std::unique_ptr<Button> makeCompactIconButton(
      Button** out, std::string glyph, ButtonVariant variant, float scale, std::function<void()> onClick,
      bool visible = true, bool participatesInLayout = true
  ) {
    return ui::button({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeBody * scale,
        .variant = variant,
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .visible = visible,
        .participatesInLayout = participatesInLayout,
        .onClick = std::move(onClick),
    });
  }

  std::unique_ptr<Flex> makeInlineConfirmPanel(Flex** out, float scale) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::Error, 0.10F),
        .radius = Style::scaledRadiusSm(scale),
        .border = colorSpecFromRole(ColorRole::Error, 0.5F),
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
    });
  }

  std::unique_ptr<Button> makeConfirmButton(
      Button** out, std::string text, ButtonVariant variant, float scale, std::function<void()> onClick,
      std::optional<std::string> glyph = std::nullopt
  ) {
    ui::ButtonProps props;
    props.out = out;
    props.text = std::move(text);
    props.fontSize = Style::fontSizeCaption * scale;
    props.variant = variant;
    props.minHeight = Style::controlHeightSm * scale;
    props.paddingV = Style::spaceXs * scale;
    props.paddingH = Style::spaceSm * scale;
    props.radius = Style::scaledRadiusSm(scale);
    props.onClick = std::move(onClick);
    if (glyph.has_value()) {
      props.glyph = std::move(*glyph);
      props.glyphSize = Style::fontSizeCaption * scale;
    }
    return ui::button(std::move(props));
  }

  class ClipboardListRow final : public InputArea {
  public:
    ClipboardListRow(float scale, AsyncTextureCache* asyncTextures, std::optional<ColorSpec> listItemBackground)
        : m_scale(scale), m_asyncTextures(asyncTextures), m_listItemBackground(listItemBackground) {
      setVisible(false);

      addChild(
          ui::box({
              .out = &m_background,
              .radius = Style::scaledRadiusMd(scale),
          })
      );

      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceMd * scale,
           .paddingV = Style::spaceXs * scale,
           .paddingH = Style::spaceSm * scale}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::row({
              .out = &m_lead,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
          })
      );

      m_lead->addChild(
          ui::image({
              .out = &m_image,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusSm(scale),
              .visible = false,
              .configure = [](Image& image) {
                image.setAsyncReadyCallback([]() { PanelManager::instance().refresh(); });
              },
          })
      );

      m_lead->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = kListGlyphSize * scale,
          })
      );

      m_lead->addChild(
          ui::box({
              .out = &m_colorSwatch,
              .radius = Style::scaledRadiusSm(scale),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textColumn,
                  .align = FlexAlign::Start,
                  .gap = 0.0F,
                  .flexGrow = 1.0F,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * scale,
                  .fontWeight = FontWeight::SemiBold,
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
                  .configure = [](Label& label) { label.setHitTestVisible(false); },
              }),
              ui::label({
                  .out = &m_meta,
                  .fontSize = Style::fontSizeCaption * scale,
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
                  .configure = [](Label& label) { label.setHitTestVisible(false); },
              })
          )
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_pinGlyph,
              .glyph = "pin",
              .glyphSize = kListPinGlyphSize * scale,
              .visible = false,
              .participatesInLayout = false,
              .configure = [](Glyph& glyph) { glyph.setHitTestVisible(false); },
          })
      );
    }

    void bind(
        Renderer& renderer, const ClipboardEntry& entry, std::size_t historyIndex, float width, float height,
        bool selected, bool hovered, std::string imageSource
    ) {
      m_historyIndex = historyIndex;
      m_selected = selected;
      m_hovered = hovered;
      m_isImage = entry.isImage();
      m_pinned = entry.pinned;
      setVisible(true);
      setEnabled(true);
      setSize(width, height);

      if (m_imageSource != imageSource) {
        if (m_image != nullptr) {
          m_image->clear(renderer);
          m_image->setVisible(false);
        }
        m_imageSource = std::move(imageSource);
      }

      const std::string rawTitle = entryTitle(entry);
      m_title->setText(m_isImage ? rawTitle : collapseWhitespace(rawTitle));
      m_meta->setText(formatTimeAgo(entry.capturedAt) + "  •  " + formatBytes(entry.byteSize));

      if (m_glyph != nullptr) {
        m_glyph->setGlyph(m_isImage ? "photo" : "file-text");
      }

      Color parsedColor;
      const bool isColor = !m_isImage && tryParseCssColor(entry.textPreview, parsedColor);

      if (m_colorSwatch != nullptr) {
        if (isColor) {
          m_colorSwatch->setFill(fixedColorSpec(parsedColor));
        }
        m_colorSwatch->setVisible(isColor);
        m_colorSwatch->setParticipatesInLayout(isColor);
      }
      if (m_glyph != nullptr) {
        m_glyph->setVisible(!isColor);
        m_glyph->setParticipatesInLayout(!isColor);
      }

      refreshThumbnail(renderer);
      applyVisualState();
      layout(renderer);
    }

    void refreshThumbnail(Renderer& renderer) {
      if (m_image == nullptr || m_glyph == nullptr || m_colorSwatch == nullptr) {
        return;
      }
      if (!m_isImage || m_imageSource.empty() || m_asyncTextures == nullptr) {
        m_image->clear(renderer);
        m_image->setVisible(false);
        const bool showGlyph = !m_colorSwatch->visible();
        m_glyph->setVisible(showGlyph);
        m_glyph->setParticipatesInLayout(showGlyph);
        return;
      }

      const int targetSize = static_cast<int>(std::ceil(kListThumbSize * m_scale));
      const bool ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_imageSource, targetSize);
      m_image->setVisible(ready);
      m_glyph->setVisible(!ready);
      m_glyph->setParticipatesInLayout(!ready);
    }

  private:
    void doLayout(Renderer& renderer) override {
      const float thumbPx = kListThumbSize * m_scale;
      const float rowW = width();
      const float rowH = height();
      if (m_background != nullptr) {
        m_background->setPosition(0.0F, 0.0F);
        m_background->setSize(rowW, rowH);
      }
      if (m_row != nullptr) {
        m_row->setPosition(0.0F, 0.0F);
        m_row->setSize(rowW, rowH);
      }
      if (m_lead != nullptr) {
        m_lead->setSize(thumbPx, thumbPx);
        m_lead->setMinWidth(thumbPx);
        m_lead->setMinHeight(thumbPx);
      }
      if (m_image != nullptr) {
        m_image->setSize(thumbPx, thumbPx);
      }
      if (m_colorSwatch != nullptr && m_colorSwatch->visible()) {
        const float swatchPx = std::round(thumbPx * 0.82F);
        m_colorSwatch->setSize(swatchPx, swatchPx);
      }
      if (m_title != nullptr && m_meta != nullptr) {
        const float pinW = m_pinned ? kListPinGlyphSize * m_scale + Style::spaceMd * m_scale : 0.0F;
        const float textWidth =
            std::max(0.0F, rowW - thumbPx - pinW - Style::spaceMd * m_scale - Style::spaceSm * m_scale * 2.0F);
        m_title->setMaxWidth(textWidth);
        m_meta->setMaxWidth(textWidth);
      }

      InputArea::doLayout(renderer);
    }

    void applyVisualState() {
      if (m_background == nullptr || m_glyph == nullptr || m_title == nullptr || m_meta == nullptr) {
        return;
      }

      if (m_selected) {
        m_background->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_background->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_background->setFill(m_listItemBackground.value_or(clearColorSpec()));
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      m_glyph->setColor(
          active ? colorSpecFromRole(activeRole)
                 : colorSpecFromRole(m_isImage ? ColorRole::Secondary : ColorRole::Primary)
      );
      m_title->setColor(colorSpecFromRole(active ? activeRole : ColorRole::OnSurface));
      m_meta->setColor(active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant));
      if (m_pinGlyph != nullptr) {
        m_pinGlyph->setVisible(m_pinned);
        m_pinGlyph->setParticipatesInLayout(m_pinned);
        m_pinGlyph->setColor(colorSpecFromRole(active ? activeRole : ColorRole::Primary));
      }
    }

    float m_scale = 1.0F;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::optional<ColorSpec> m_listItemBackground;
    Box* m_background = nullptr;
    Flex* m_row = nullptr;
    Flex* m_lead = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Box* m_colorSwatch = nullptr;
    Glyph* m_pinGlyph = nullptr;
    Flex* m_textColumn = nullptr;
    Label* m_title = nullptr;
    Label* m_meta = nullptr;
    std::size_t m_historyIndex = static_cast<std::size_t>(-1);
    bool m_selected = false;
    bool m_hovered = false;
    bool m_isImage = false;
    bool m_pinned = false;
    std::string m_imageSource;
  };

  class GifGridTile final : public InputArea {
  public:
    GifGridTile(
        float scale, AsyncTextureCache* asyncTextures, TenorService* tenor,
        std::optional<ColorSpec> listItemBackground
    )
        : m_scale(scale), m_asyncTextures(asyncTextures), m_tenor(tenor), m_listItemBackground(listItemBackground) {
      setVisible(false);

      addChild(
          ui::box({
              .out = &m_background,
              .radius = Style::scaledRadiusMd(scale),
          })
      );

      auto col = ui::column({
          .out = &m_column,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceXs * scale,
      });
      addChild(std::move(col));

      m_column->addChild(
          ui::image({
              .out = &m_image,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusSm(scale),
              .configure = [](Image& image) {
                image.setAsyncReadyCallback([]() { PanelManager::instance().refresh(); });
              },
          })
      );

      m_column->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * scale,
              .maxLines = 1,
              .baselineMode = LabelBaselineMode::TextFixedHeight,
              .configure = [](Label& label) { label.setHitTestVisible(false); },
          })
      );
    }

    void bind(Renderer& renderer, const TenorMediaItem& item, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      setVisible(true);
      setEnabled(true);
      setSize(width, height);

      m_title->setText(item.title.empty() ? "GIF" : item.title);

      if (m_tenor != nullptr && !item.previewUrl.empty()) {
        const auto path = m_tenor->localPathForUrl(item.previewUrl);
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
          m_image->setSourceFileAsync(renderer, *m_asyncTextures, path.string(), static_cast<int>(std::ceil(width)));
          m_image->setVisible(true);
        } else {
          m_tenor->ensureLocalPreview(
              const_cast<TenorMediaItem&>(item),
              [this, &renderer, w = width](std::optional<std::filesystem::path> p) {
                if (p.has_value() && m_image != nullptr) {
                  m_image->setSourceFileAsync(renderer, *m_asyncTextures, p->string(), static_cast<int>(std::ceil(w)));
                  m_image->setVisible(true);
                  PanelManager::instance().refresh();
                }
              }
          );
        }
      }

      applyVisualState();
      layout(renderer);
    }

    void applyVisualState() {
      if (m_background == nullptr || m_title == nullptr) {
        return;
      }
      if (m_selected) {
        m_background->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_background->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_background->setFill(m_listItemBackground.value_or(clearColorSpec()));
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      m_title->setColor(colorSpecFromRole(active ? activeRole : ColorRole::OnSurface));
    }

  private:
    void doLayout(Renderer& renderer) override {
      if (m_background != nullptr) {
        m_background->setPosition(0.0f, 0.0f);
        m_background->setSize(width(), height());
      }
      if (m_column != nullptr) {
        m_column->setPosition(0.0f, 0.0f);
        m_column->setSize(width(), height());
      }
      if (m_image != nullptr) {
        const float textH = Style::fontSizeCaption * m_scale + Style::spaceXs * m_scale;
        m_image->setSize(
            std::max(10.0f, width() - Style::spaceXs * m_scale * 2.0f),
            std::max(10.0f, height() - textH - Style::spaceXs * m_scale * 2.0f)
        );
      }
      InputArea::doLayout(renderer);
    }

    float m_scale = 1.0f;
    AsyncTextureCache* m_asyncTextures = nullptr;
    TenorService* m_tenor = nullptr;
    std::optional<ColorSpec> m_listItemBackground;
    Box* m_background = nullptr;
    Flex* m_column = nullptr;
    Image* m_image = nullptr;
    Label* m_title = nullptr;
    bool m_selected = false;
    bool m_hovered = false;
  };

  class EmojiGridTile final : public InputArea {
  public:
    EmojiGridTile(float scale, std::optional<ColorSpec> listItemBackground)
        : m_scale(scale), m_listItemBackground(listItemBackground) {
      setVisible(false);

      addChild(
          ui::box({
              .out = &m_background,
              .radius = Style::scaledRadiusMd(scale),
          })
      );

      addChild(
          ui::label({
              .out = &m_emojiLabel,
              .fontSize = 28.0f * scale,
              .baselineMode = LabelBaselineMode::TextFixedHeight,
              .configure = [](Label& label) { label.setHitTestVisible(false); },
          })
      );
    }

    void bind(Renderer& renderer, const EmojiItem& item, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      setVisible(true);
      setEnabled(true);
      setSize(width, height);

      m_emojiLabel->setText(item.emoji);
      applyVisualState();
      layout(renderer);
    }

    void applyVisualState() {
      if (m_background == nullptr) {
        return;
      }
      if (m_selected) {
        m_background->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_background->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_background->setFill(m_listItemBackground.value_or(clearColorSpec()));
      }
    }

  private:
    void doLayout(Renderer& renderer) override {
      if (m_background != nullptr) {
        m_background->setPosition(0.0f, 0.0f);
        m_background->setSize(width(), height());
      }
      if (m_emojiLabel != nullptr) {
        const float emojiSize = 28.0f * m_scale;
        m_emojiLabel->setPosition(
            std::max(0.0f, (width() - emojiSize) * 0.5f), std::max(0.0f, (height() - emojiSize) * 0.5f)
        );
        m_emojiLabel->setSize(emojiSize, emojiSize);
      }
      InputArea::doLayout(renderer);
    }

    float m_scale = 1.0f;
    std::optional<ColorSpec> m_listItemBackground;
    Box* m_background = nullptr;
    Label* m_emojiLabel = nullptr;
    bool m_selected = false;
    bool m_hovered = false;
  };

} // namespace

class ClipboardListAdapter final : public VirtualGridAdapter {
public:
  ClipboardListAdapter(
      float scale, ClipboardService* clipboard, AsyncTextureCache* asyncTextures,
      std::optional<ColorSpec> listItemBackground
  )
      : m_scale(scale), m_clipboard(clipboard), m_asyncTextures(asyncTextures),
        m_listItemBackground(listItemBackground) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setFilteredIndices(const std::vector<std::size_t>* indices) { m_filteredIndices = indices; }
  void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override {
    return m_filteredIndices == nullptr ? 0 : m_filteredIndices->size();
  }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<ClipboardListRow>(m_scale, m_asyncTextures, m_listItemBackground);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_clipboard == nullptr || m_filteredIndices == nullptr
        || index >= m_filteredIndices->size()) {
      return;
    }
    const std::size_t historyIndex = (*m_filteredIndices)[index];
    const auto& history = m_clipboard->history();
    if (historyIndex >= history.size()) {
      return;
    }
    auto* row = static_cast<ClipboardListRow*>(&tile);
    std::string imageSource;
    if (history[historyIndex].isImage()) {
      imageSource = m_clipboard->imageDataUri(historyIndex).value_or("");
    }
    row->bind(*m_renderer, history[historyIndex], historyIndex, row->width(), row->height(), selected, hovered, imageSource);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

private:
  float m_scale = 1.0F;
  Renderer* m_renderer = nullptr;
  ClipboardService* m_clipboard = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::optional<ColorSpec> m_listItemBackground;
  const std::vector<std::size_t>* m_filteredIndices = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

class GifGridAdapter final : public VirtualGridAdapter {
public:
  GifGridAdapter(
      float scale, AsyncTextureCache* asyncTextures, TenorService* tenor,
      std::optional<ColorSpec> listItemBackground
  )
      : m_scale(scale), m_asyncTextures(asyncTextures), m_tenor(tenor),
        m_listItemBackground(listItemBackground) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setItems(const std::vector<TenorMediaItem>* items) { m_items = items; }
  void setOnActivate(std::function<void(std::size_t)> cb) { m_onActivate = std::move(cb); }

  [[nodiscard]] std::size_t itemCount() const override { return m_items != nullptr ? m_items->size() : 0; }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<GifGridTile>(m_scale, m_asyncTextures, m_tenor, m_listItemBackground);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_items == nullptr || index >= m_items->size()) {
      return;
    }
    auto* t = static_cast<GifGridTile*>(&tile);
    t->bind(*m_renderer, (*m_items)[index], t->width(), t->height(), selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_items != nullptr && index < m_items->size()) {
      return (*m_items)[index].title;
    }
    return {};
  }

private:
  float m_scale = 1.0f;
  Renderer* m_renderer = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  TenorService* m_tenor = nullptr;
  std::optional<ColorSpec> m_listItemBackground;
  const std::vector<TenorMediaItem>* m_items = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

class EmojiGridAdapter final : public VirtualGridAdapter {
public:
  EmojiGridAdapter(float scale, std::optional<ColorSpec> listItemBackground)
      : m_scale(scale), m_listItemBackground(listItemBackground) {}

  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setIndices(const std::vector<std::size_t>* indices) { m_indices = indices; }
  void setOnActivate(std::function<void(std::size_t)> cb) { m_onActivate = std::move(cb); }

  [[nodiscard]] std::size_t itemCount() const override { return m_indices != nullptr ? m_indices->size() : 0; }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<EmojiGridTile>(m_scale, m_listItemBackground);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_indices == nullptr || index >= m_indices->size()) {
      return;
    }
    const std::size_t emojiIndex = (*m_indices)[index];
    const auto* item = EmojiStore::instance().get(emojiIndex);
    if (item == nullptr) {
      return;
    }
    auto* t = static_cast<EmojiGridTile*>(&tile);
    t->bind(*m_renderer, *item, t->width(), t->height(), selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_indices != nullptr && index < m_indices->size()) {
      const auto* item = EmojiStore::instance().get((*m_indices)[index]);
      if (item != nullptr) {
        return item->name + " (" + item->category + ")";
      }
    }
    return {};
  }

private:
  float m_scale = 1.0f;
  Renderer* m_renderer = nullptr;
  std::optional<ColorSpec> m_listItemBackground;
  const std::vector<std::size_t>* m_indices = nullptr;
  std::function<void(std::size_t)> m_onActivate;
};

ClipboardPanel::ClipboardPanel(
    ClipboardService* clipboard, ConfigService* config, AsyncTextureCache* asyncTextures, TenorService* tenor
)
    : m_clipboard(clipboard), m_config(config), m_asyncTextures(asyncTextures), m_tenor(tenor) {}

ClipboardPanel::~ClipboardPanel() = default;

PanelPlacement ClipboardPanel::panelPlacement() const noexcept {
  if (m_config == nullptr) {
    return PanelPlacement::Floating;
  }
  return m_config->config().shell.panel.clipboardPlacement;
}

void ClipboardPanel::setActivateCallback(std::function<void(const ClipboardEntry&)> callback) {
  m_activateCallback = std::move(callback);
}

void ClipboardPanel::create() {
  const float scale = contentScale();
  auto rootLayout = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });
  auto contentRow = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });

  auto focusArea = ui::inputArea({});
  focusArea->setFocusable(true);
  focusArea->setTabStop(false);
  focusArea->setVisible(false);
  focusArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (key.pressed) {
      handleKeyEvent(key.sym, key.modifiers);
    }
  });
  m_focusArea = static_cast<InputArea*>(rootLayout->addChild(std::move(focusArea)));

  auto sidebar = ui::column({
      .out = &m_sidebar,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
      .flexGrow = 2.0F,
  });

  auto sidebarHeader = ui::row(
      {
          .out = &m_sidebarHeaderRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
          .minHeight = Style::controlHeightSm * scale,
      },
      ui::label({
          .out = &m_sidebarTitle,
          .text = i18n::tr("clipboard.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeCompactIconButton(&m_clearHistoryButton, "trash", ButtonVariant::Destructive, scale, [this]() {
        requestClearUnpinnedHistory();
      })
  );
  sidebar->addChild(std::move(sidebarHeader));

  // Tab mode switcher
  std::vector<ui::SegmentedOption> tabOptions;
  tabOptions.push_back({.label = i18n::tr("clipboard.title"), .glyph = "clipboard"});
  tabOptions.push_back({.label = "GIFs", .glyph = "movie"});
  tabOptions.push_back({.label = "Emojis", .glyph = "mood-smile"});

  sidebar->addChild(
      ui::segmented({
          .out = &m_tabSegmented,
          .options = std::move(tabOptions),
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Surface,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) { setTabMode(static_cast<ClipboardTabMode>(index)); },
      })
  );

  auto clearConfirmPanel = makeInlineConfirmPanel(&m_clearConfirmPanel, scale);
  clearConfirmPanel->addChild(
      ui::label({
          .text = i18n::tr("clipboard.confirm.clear-title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Error),
      })
  );
  clearConfirmPanel->addChild(
      ui::label({
          .out = &m_clearConfirmDesc,
          .text = i18n::tr("clipboard.confirm.clear-desc"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  clearConfirmPanel->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, ui::spacer(),
          makeConfirmButton(
              nullptr, i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, scale,
              [this]() {
                resetClearConfirmation();
                updateListState();
                PanelManager::instance().refresh();
              }
          ),
          makeConfirmButton(
              &m_clearKeepPinnedButton, i18n::tr("clipboard.actions.keep-pinned"), ButtonVariant::Default, scale,
              [this]() { clearUnpinnedHistory(); }
          ),
          makeConfirmButton(
              nullptr, i18n::tr("clipboard.actions.clear-all"), ButtonVariant::Destructive, scale,
              [this]() { clearAllHistory(); }, "trash"
          )
      )
  );
  rootLayout->addChild(std::move(clearConfirmPanel));

  sidebar->addChild(
      ui::input({
          .out = &m_filterInput,
          .placeholder = i18n::tr("clipboard.filter-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceMd * scale,
          .clearButtonEnabled = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](const std::string& text) { onFilterChanged(text); },
          .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      })
  );

  const bool listItemBackground = m_config != nullptr && m_config->config().shell.panel.listItemBackground;
  m_listAdapter = std::make_unique<ClipboardListAdapter>(
      scale, m_clipboard, m_asyncTextures,
      listItemBackground ? std::optional(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()))
                         : std::nullopt
  );
  m_listAdapter->setFilteredIndices(&m_filteredIndices);
  m_listAdapter->setOnActivate([this](std::size_t index) {
    if (m_selectedIndex == index) {
      activateSelected();
      return;
    }
    selectIndex(index);
  });

  m_gifAdapter = std::make_unique<GifGridAdapter>(
      scale, m_asyncTextures, m_tenor,
      listItemBackground ? std::optional(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()))
                         : std::nullopt
  );
  m_gifAdapter->setItems(&m_gifResults);
  m_gifAdapter->setOnActivate([this](std::size_t index) {
    if (m_selectedIndex == index) {
      activateSelected();
      return;
    }
    selectIndex(index);
  });

  m_emojiAdapter = std::make_unique<EmojiGridAdapter>(
      scale,
      listItemBackground ? std::optional(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()))
                         : std::nullopt
  );
  m_emojiAdapter->setIndices(&m_emojiIndices);
  m_emojiAdapter->setOnActivate([this](std::size_t index) {
    if (m_selectedIndex == index) {
      activateSelected();
      return;
    }
    selectIndex(index);
  });

  sidebar->addChild(
      ui::virtualGridView({
          .out = &m_listGrid,
          .contentScale = scale,
          .columns = 1,
          .cellHeight = kRowHeightEstimate * scale,
          .squareCells = false,
          .columnGap = 0.0F,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kListOverscanRows,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .scrollbarVisible = true,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0F,
      })
  );

  sidebar->addChild(
      ui::label({
          .out = &m_listEmptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  contentRow->addChild(std::move(sidebar));

  auto preview = ui::column({
      .out = &m_previewCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceSm * scale,
      .flexGrow = 3.0F,
  });

  auto previewActions = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      makeCompactIconButton(&m_copyButton, "copy", ButtonVariant::Default, scale, [this]() { activateSelected(); }),
      makeCompactIconButton(
          &m_imageActionButton, "photo-edit", ButtonVariant::Default, scale, [this]() { runImageAction(); }, false,
          false
      ),
      makeCompactIconButton(&m_pinButton, "pin", ButtonVariant::Default, scale, [this]() { togglePinSelected(); }),
      makeCompactIconButton(
          &m_deleteEntryButton, "trash", ButtonVariant::Destructive, scale, [this]() { requestDeleteSelectedEntry(); }
      ),
      makeCompactIconButton(&m_closeButton, "close", ButtonVariant::Default, scale, []() {
        PanelManager::instance().close();
      })
  );

  auto previewHeader = ui::row(
      {
          .out = &m_previewHeaderRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
      },
      ui::label({
          .out = &m_previewTitle,
          .text = i18n::tr("clipboard.entry.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .flexGrow = 1.0F,
      }),
      std::move(previewActions)
  );
  preview->addChild(std::move(previewHeader));

  preview->addChild(
      ui::label({
          .out = &m_previewMeta,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  auto deleteConfirmPanel = makeInlineConfirmPanel(&m_deleteConfirmPanel, scale);
  deleteConfirmPanel->addChild(
      ui::label({
          .text = i18n::tr("clipboard.confirm.delete-title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Error),
      })
  );
  deleteConfirmPanel->addChild(
      ui::label({
          .out = &m_clearConfirmDesc,
          .text = i18n::tr("clipboard.confirm.delete-desc"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  deleteConfirmPanel->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, ui::spacer(),
          makeConfirmButton(
              nullptr, i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, scale,
              [this]() {
                resetDeleteConfirmation();
                updatePreviewActions();
                PanelManager::instance().refresh();
              }
          ),
          makeConfirmButton(
              nullptr, i18n::tr("clipboard.actions.delete-item"), ButtonVariant::Destructive, scale,
              [this]() { deleteSelectedEntry(); }, "trash"
          )
      )
  );
  rootLayout->addChild(std::move(deleteConfirmPanel));

  auto previewScroll = ui::scrollView({
      .out = &m_previewScrollView,
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
      .configure = [scale, opacity = panelCardOpacity()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity);
      },
  });
  m_previewContent = previewScroll->content();
  m_previewContent->setDirection(FlexDirection::Vertical);
  m_previewContent->setAlign(FlexAlign::Start);
  m_previewContent->setGap(Style::spaceSm * scale);
  preview->addChild(std::move(previewScroll));

  contentRow->addChild(std::move(preview));
  rootLayout->addChild(std::move(contentRow));

  setRoot(std::move(rootLayout));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  schedulePreviewPayloadRefresh(false);
}

void ClipboardPanel::setTabMode(ClipboardTabMode mode) {
  if (m_tabMode == mode) {
    return;
  }
  m_tabMode = mode;
  m_selectedIndex = 0;
  m_pendingFilterQuery.clear();
  m_filterQuery.clear();

  if (m_filterInput != nullptr) {
    m_filterInput->setValue("");
    switch (m_tabMode) {
    case ClipboardTabMode::History:
      m_filterInput->setPlaceholder(i18n::tr("clipboard.filter-placeholder"));
      break;
    case ClipboardTabMode::Gifs:
      m_filterInput->setPlaceholder("Search Tenor GIFs...");
      break;
    case ClipboardTabMode::Emojis:
      m_filterInput->setPlaceholder("Search emojis...");
      break;
    }
  }

  if (m_clearHistoryButton != nullptr) {
    m_clearHistoryButton->setVisible(m_tabMode == ClipboardTabMode::History);
    m_clearHistoryButton->setParticipatesInLayout(m_tabMode == ClipboardTabMode::History);
  }

  const float scale = contentScale();
  if (m_listGrid != nullptr) {
    switch (m_tabMode) {
    case ClipboardTabMode::History:
      m_listGrid->setColumns(1);
      m_listGrid->setCellHeight(m_listRowHeight > 0.0f ? m_listRowHeight : kRowHeightEstimate * scale);
      m_listGrid->setSquareCells(false);
      m_listGrid->setAdapter(m_listAdapter.get());
      applyFilter();
      break;
    case ClipboardTabMode::Gifs:
      m_listGrid->setColumns(2);
      m_listGrid->setCellHeight(130.0f * scale);
      m_listGrid->setSquareCells(false);
      m_listGrid->setAdapter(m_gifAdapter.get());
      if (m_gifResults.empty()) {
        fetchTrendingGifs();
      }
      break;
    case ClipboardTabMode::Emojis:
      m_listGrid->setColumns(6);
      m_listGrid->setCellHeight(48.0f * scale);
      m_listGrid->setSquareCells(true);
      m_listGrid->setAdapter(m_emojiAdapter.get());
      searchEmojis("");
      break;
    }
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(0);
    m_listGrid->scrollView().setScrollOffset(0.0f);
  }

  updateListState();
  PanelManager::instance().refresh();
}

void ClipboardPanel::fetchTrendingGifs() {
  if (m_tenor == nullptr) {
    return;
  }
  m_isLoadingGifs = true;
  m_tenor->featuredGifs(50, [this](std::vector<TenorMediaItem> results) {
    m_isLoadingGifs = false;
    if (m_tabMode == ClipboardTabMode::Gifs && m_filterQuery.empty()) {
      m_gifResults = std::move(results);
      if (m_listGrid != nullptr) {
        m_listGrid->notifyDataChanged();
        m_listGrid->setSelectedIndex(m_gifResults.empty() ? std::nullopt : std::optional<std::size_t>(0));
      }
      updateListState();
      PanelManager::instance().refresh();
    }
  });
}

void ClipboardPanel::searchGifs(const std::string& query) {
  if (m_tenor == nullptr) {
    return;
  }
  if (query.empty()) {
    fetchTrendingGifs();
    return;
  }
  m_isLoadingGifs = true;
  m_tenor->searchGifs(query, 50, [this, query](std::vector<TenorMediaItem> results) {
    m_isLoadingGifs = false;
    if (m_tabMode == ClipboardTabMode::Gifs && m_filterQuery == query) {
      m_gifResults = std::move(results);
      if (m_listGrid != nullptr) {
        m_listGrid->notifyDataChanged();
        m_listGrid->setSelectedIndex(m_gifResults.empty() ? std::nullopt : std::optional<std::size_t>(0));
      }
      updateListState();
      PanelManager::instance().refresh();
    }
  });
}

void ClipboardPanel::searchEmojis(const std::string& query) {
  EmojiStore::instance().loadIfNeeded();
  m_emojiIndices = EmojiStore::instance().search(query);
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(m_emojiIndices.empty() ? std::nullopt : std::optional<std::size_t>(0));
  }
  updateListState();
  PanelManager::instance().refresh();
}

void ClipboardPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_sidebar == nullptr || m_previewCard == nullptr || m_listGrid == nullptr
      || m_previewScrollView == nullptr) {
    return;
  }

  m_lastWidth = width;
  m_lastHeight = height;

  m_focusArea->setPosition(0.0F, 0.0F);
  m_focusArea->setSize(1.0F, 1.0F);

  if (m_listAdapter != nullptr) {
    m_listAdapter->setRenderer(&renderer);
  }
  if (m_gifAdapter != nullptr) {
    m_gifAdapter->setRenderer(&renderer);
  }
  if (m_emojiAdapter != nullptr) {
    m_emojiAdapter->setRenderer(&renderer);
  }

  if (m_tabMode == ClipboardTabMode::History) {
    const float rowHeight = listRowHeight(renderer, contentScale());
    if (std::abs(rowHeight - m_listRowHeight) >= 0.5F) {
      m_listRowHeight = rowHeight;
      m_listGrid->setCellHeight(rowHeight);
    }
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  bool relayoutNeeded = false;
  const float previewScrollH = m_previewScrollView->height();
  if (m_lastPreviewWidth != m_previewScrollView->contentViewportWidth() || m_lastPreviewHeight != previewScrollH) {
    rebuildPreview(renderer, m_previewScrollView->contentViewportWidth(), previewScrollH);
    relayoutNeeded = true;
  }

  if (relayoutNeeded) {
    m_rootLayout->layout(renderer);
  }

  if (m_pendingScrollToSelected) {
    scrollToSelected();
    m_pendingScrollToSelected = false;
  }
}

void ClipboardPanel::doUpdate(Renderer& renderer) {
  updatePreviewActions();

  if (m_clipboard == nullptr || m_lastWidth <= 0.0F) {
    return;
  }

  if (m_tabMode == ClipboardTabMode::History && m_clipboard != nullptr) {
    if (m_lastChangeSerial != m_clipboard->changeSerial()) {
      resetClearConfirmation();
      resetDeleteConfirmation();
      applyFilter();
      if (m_filteredIndices.empty()) {
        m_selectedIndex = 0;
      } else if (m_selectedIndex >= m_filteredIndices.size()) {
        m_selectedIndex = m_filteredIndices.size() - 1;
      }

      m_lastChangeSerial = m_clipboard->changeSerial();
      updateListState();
      if (m_listGrid != nullptr) {
        m_listGrid->notifyDataChanged();
        m_listGrid->setSelectedIndex(
            m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
        );
      }

      schedulePreviewPayloadRefresh(false);
      const float previewWidth =
          m_previewScrollView != nullptr ? m_previewScrollView->contentViewportWidth() : m_lastWidth;
      const float previewHeight = m_previewScrollView != nullptr ? m_previewScrollView->height() : m_lastHeight;
      rebuildPreview(renderer, previewWidth, previewHeight);
    }
  }
}

void ClipboardPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex = 0;
  m_previewPayloadIndex = static_cast<std::size_t>(-1);
  m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
  m_previewPayloadDebounceTimer.stop();
  m_lastPreviewWidth = -1.0F;
  m_lastPreviewHeight = -1.0F;
  m_pendingScrollToSelected = false;
  m_filterQuery.clear();
  m_pendingFilterQuery.clear();
  m_filterDebounceTimer.stop();
  if (m_filterInput != nullptr) {
    m_filterInput->setValue("");
  }
  if (m_tabMode == ClipboardTabMode::History) {
    applyFilter();
    updateListState();
    if (m_listGrid != nullptr) {
      m_listGrid->notifyDataChanged();
      m_listGrid->setSelectedIndex(
          m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
      );
      m_listGrid->scrollView().setScrollOffset(0.0F);
    }
    m_lastChangeSerial = m_clipboard != nullptr ? m_clipboard->changeSerial() : 0;
    schedulePreviewPayloadRefresh(false);
  } else if (m_tabMode == ClipboardTabMode::Gifs) {
    fetchTrendingGifs();
  } else if (m_tabMode == ClipboardTabMode::Emojis) {
    searchEmojis("");
  }
}

void ClipboardPanel::onClose() {
  resetDeleteConfirmation();
  resetClearConfirmation();
  if (m_listGrid != nullptr) {
    m_listGrid->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gifAdapter.reset();
  m_emojiAdapter.reset();
  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_sidebar = nullptr;
  m_sidebarHeaderRow = nullptr;
  m_sidebarTitle = nullptr;
  m_tabSegmented = nullptr;
  m_clearHistoryButton = nullptr;
  m_clearKeepPinnedButton = nullptr;
  m_clearConfirmPanel = nullptr;
  m_clearConfirmDesc = nullptr;
  m_closeButton = nullptr;
  m_filterInput = nullptr;
  m_listGrid = nullptr;
  m_listEmptyLabel = nullptr;
  m_filteredIndices.clear();
  m_previewCard = nullptr;
  m_previewHeaderRow = nullptr;
  m_previewTitle = nullptr;
  m_previewMeta = nullptr;
  m_imageActionButton = nullptr;
  m_pinButton = nullptr;
  m_copyButton = nullptr;
  m_deleteEntryButton = nullptr;
  m_deleteConfirmPanel = nullptr;
  m_previewScrollView = nullptr;
  m_previewContent = nullptr;
  m_previewImage = nullptr;
  m_previewEmojiLabel = nullptr;
  m_previewPayloadDebounceTimer.stop();
  m_filterDebounceTimer.stop();
  m_pendingFilterQuery.clear();
  m_filterQuery.clear();
  clearReleasedRoot();
  m_lastWidth = 0.0F;
  m_lastHeight = 0.0F;
  m_pendingScrollToSelected = false;

  if (m_clipboard != nullptr) {
    m_clipboard->evictAllPayloads();
  }
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }
}

InputArea* ClipboardPanel::initialFocusArea() const {
  return m_filterInput != nullptr ? m_filterInput->inputArea() : m_focusArea;
}

bool ClipboardPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* focused = dispatcher.focusedArea();
  if (focused != nullptr && !isDescendantOf(focused, m_listGrid)) {
    return false;
  }

  return handleKeyEvent(sym, modifiers);
}

void ClipboardPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_previewScrollView != nullptr) {
    m_previewScrollView->setCardStyle(contentScale(), opacity);
  }
  if (m_filterInput != nullptr) {
    m_filterInput->setSurfaceOpacity(opacity);
  }
}

void ClipboardPanel::schedulePreviewPayloadRefresh(bool debounced) {
  if (m_tabMode != ClipboardTabMode::History) {
    m_previewPayloadDebounceTimer.stop();
    m_previewPayloadIndex = static_cast<std::size_t>(-1);
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0f;
    m_lastPreviewHeight = -1.0f;
    return;
  }

  const std::size_t historyIndex = selectedHistoryIndex();
  if (m_clipboard == nullptr || historyIndex == static_cast<std::size_t>(-1)) {
    m_previewPayloadDebounceTimer.stop();
    m_previewPayloadIndex = static_cast<std::size_t>(-1);
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0F;
    m_lastPreviewHeight = -1.0F;
    return;
  }

  if (!debounced || historyIndex == m_previewPayloadIndex) {
    m_previewPayloadDebounceTimer.stop();
    m_previewPayloadIndex = historyIndex;
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0F;
    m_lastPreviewHeight = -1.0F;
    return;
  }

  m_pendingPreviewPayloadIndex = historyIndex;
  m_lastPreviewWidth = -1.0F;
  m_lastPreviewHeight = -1.0F;
  m_previewPayloadDebounceTimer.start(kPreviewPayloadDebounceInterval, [this]() {
    if (m_pendingPreviewPayloadIndex == static_cast<std::size_t>(-1)) {
      return;
    }
    m_previewPayloadIndex = m_pendingPreviewPayloadIndex;
    m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
    m_lastPreviewWidth = -1.0F;
    m_lastPreviewHeight = -1.0F;
    PanelManager::instance().refresh();
  });
}

void ClipboardPanel::updateListState() {
  if (m_tabMode == ClipboardTabMode::History) {
    const auto& history = m_clipboard != nullptr ? m_clipboard->history() : std::deque<ClipboardEntry>{};
    const bool empty = history.empty() || m_filteredIndices.empty();
    const bool hasHistory = !history.empty();
    const bool hasPinned = std::ranges::any_of(history, [](const ClipboardEntry& entry) { return entry.pinned; });
    const bool hasUnpinned = std::ranges::any_of(history, [](const ClipboardEntry& entry) { return !entry.pinned; });
    const bool showKeepPinnedChoice = hasPinned && hasUnpinned;
    if (!hasHistory) {
      resetClearConfirmation();
    }

    if (m_clearHistoryButton != nullptr) {
      m_clearHistoryButton->setVisible(hasHistory);
      m_clearHistoryButton->setParticipatesInLayout(hasHistory);
      m_clearHistoryButton->setGlyph(m_clearConfirm ? "warning" : "trash");
    }
    if (m_clearConfirmPanel != nullptr) {
      m_clearConfirmPanel->setVisible(hasHistory && m_clearConfirm);
      m_clearConfirmPanel->setParticipatesInLayout(hasHistory && m_clearConfirm);
    }
    if (m_clearKeepPinnedButton != nullptr) {
      const bool showKeepPinned = showKeepPinnedChoice && m_clearConfirm;
      m_clearKeepPinnedButton->setVisible(showKeepPinned);
      m_clearKeepPinnedButton->setParticipatesInLayout(showKeepPinned);
      m_clearKeepPinnedButton->setEnabled(hasUnpinned);
    }
    if (m_clearConfirmDesc != nullptr) {
      if (!hasPinned) {
        m_clearConfirmDesc->setText(i18n::tr("clipboard.confirm.clear-desc-no-pinned"));
      } else if (!hasUnpinned) {
        m_clearConfirmDesc->setText(i18n::tr("clipboard.confirm.clear-desc-all-pinned"));
      } else {
        m_clearConfirmDesc->setText(i18n::tr("clipboard.confirm.clear-desc"));
      }
    }

    if (m_listEmptyLabel != nullptr) {
      m_listEmptyLabel->setText(
          history.empty()             ? i18n::tr("clipboard.empty.history-title")
              : m_filterQuery.empty() ? i18n::tr("clipboard.empty.history-title")
                                      : i18n::tr("clipboard.empty.no-matches-title")
      );
      m_listEmptyLabel->setVisible(empty);
      m_listEmptyLabel->setParticipatesInLayout(empty);
    }
    if (m_listGrid != nullptr) {
      m_listGrid->setVisible(!empty);
      m_listGrid->setParticipatesInLayout(!empty);
    }
  } else if (m_tabMode == ClipboardTabMode::Gifs) {
    const bool empty = m_gifResults.empty();
    if (m_clearHistoryButton != nullptr) {
      m_clearHistoryButton->setVisible(false);
      m_clearHistoryButton->setParticipatesInLayout(false);
    }
    if (m_clearConfirmPanel != nullptr) {
      m_clearConfirmPanel->setVisible(false);
      m_clearConfirmPanel->setParticipatesInLayout(false);
    }
    if (m_listEmptyLabel != nullptr) {
      m_listEmptyLabel->setText(m_isLoadingGifs ? "Loading Tenor GIFs..." : "No GIFs found");
      m_listEmptyLabel->setVisible(empty);
      m_listEmptyLabel->setParticipatesInLayout(empty);
    }
    if (m_listGrid != nullptr) {
      m_listGrid->setVisible(!empty);
      m_listGrid->setParticipatesInLayout(!empty);
    }
  } else if (m_tabMode == ClipboardTabMode::Emojis) {
    const bool empty = m_emojiIndices.empty();
    if (m_clearHistoryButton != nullptr) {
      m_clearHistoryButton->setVisible(false);
      m_clearHistoryButton->setParticipatesInLayout(false);
    }
    if (m_clearConfirmPanel != nullptr) {
      m_clearConfirmPanel->setVisible(false);
      m_clearConfirmPanel->setParticipatesInLayout(false);
    }
    if (m_listEmptyLabel != nullptr) {
      m_listEmptyLabel->setText("No emojis found");
      m_listEmptyLabel->setVisible(empty);
      m_listEmptyLabel->setParticipatesInLayout(empty);
    }
    if (m_listGrid != nullptr) {
      m_listGrid->setVisible(!empty);
      m_listGrid->setParticipatesInLayout(!empty);
    }
  }
}

void ClipboardPanel::updatePreviewActions() {
  bool hasSelection = false;
  bool showImageAction = false;
  bool pinned = false;
  bool deleteConfirmActive = false;

  if (m_tabMode == ClipboardTabMode::History) {
    if (m_clipboard != nullptr) {
      const std::size_t historyIndex = selectedHistoryIndex();
      const auto& history = m_clipboard->history();
      if (historyIndex != static_cast<std::size_t>(-1) && historyIndex < history.size()) {
        hasSelection = true;
        pinned = history[historyIndex].pinned;
        deleteConfirmActive =
            !m_deleteConfirmStorageId.empty() && m_deleteConfirmStorageId == history[historyIndex].storageId;
        showImageAction = m_config != nullptr
            && !StringUtils::trim(m_config->config().shell.clipboardImageActionCommand).empty()
            && history[historyIndex].isImage();
      }
    }
  } else if (m_tabMode == ClipboardTabMode::Gifs) {
    hasSelection = !m_gifResults.empty() && m_selectedIndex < m_gifResults.size();
  } else if (m_tabMode == ClipboardTabMode::Emojis) {
    hasSelection = !m_emojiIndices.empty() && m_selectedIndex < m_emojiIndices.size();
  }

  if (m_copyButton != nullptr) {
    m_copyButton->setVisible(hasSelection);
    m_copyButton->setParticipatesInLayout(hasSelection);
  }

  if (m_deleteEntryButton != nullptr) {
    const bool showDelete = (m_tabMode == ClipboardTabMode::History) && hasSelection;
    m_deleteEntryButton->setVisible(showDelete);
    m_deleteEntryButton->setParticipatesInLayout(showDelete);
    m_deleteEntryButton->setGlyph(deleteConfirmActive ? "warning" : "trash");
  }
  if (m_deleteConfirmPanel != nullptr) {
    const bool showDeleteConfirm = (m_tabMode == ClipboardTabMode::History) && hasSelection && deleteConfirmActive;
    m_deleteConfirmPanel->setVisible(showDeleteConfirm);
    m_deleteConfirmPanel->setParticipatesInLayout(showDeleteConfirm);
  }

  if (m_imageActionButton != nullptr) {
    m_imageActionButton->setVisible(showImageAction);
    m_imageActionButton->setParticipatesInLayout(showImageAction);
  }

  if (m_pinButton != nullptr) {
    const bool showPin = (m_tabMode == ClipboardTabMode::History) && hasSelection;
    m_pinButton->setVisible(showPin);
    m_pinButton->setParticipatesInLayout(showPin);
    m_pinButton->setGlyph(pinned ? "unpin" : "pin");
    m_pinButton->setVariant(pinned ? ButtonVariant::Primary : ButtonVariant::Default);
  }
}

void ClipboardPanel::rebuildPreview(Renderer& renderer, float width, float height) {
  uiAssertNotRendering("ClipboardPanel::rebuildPreview");
  if (m_previewContent == nullptr || m_previewTitle == nullptr || m_previewMeta == nullptr) {
    return;
  }

  updatePreviewActions();

  while (!m_previewContent->children().empty()) {
    m_previewContent->removeChild(m_previewContent->children().front().get());
  }
  m_previewImage = nullptr;
  m_previewEmojiLabel = nullptr;

  if (m_tabMode == ClipboardTabMode::Gifs) {
    if (m_gifResults.empty() || m_selectedIndex >= m_gifResults.size()) {
      m_previewTitle->setText("Tenor GIFs");
      m_previewMeta->setText(m_isLoadingGifs ? "Loading..." : "No selection");
      return;
    }

    const auto& item = m_gifResults[m_selectedIndex];
    m_previewTitle->setText(item.title.empty() ? "GIF" : item.title);
    m_previewTitle->setMaxWidth(width);
    m_previewMeta->setText(
        std::to_string(static_cast<int>(item.width)) + "x" + std::to_string(static_cast<int>(item.height))
        + " • Tenor GIF"
    );
    m_previewMeta->setMaxWidth(width);

    const float scale = contentScale();
    const float imageHeight =
        std::min(kPreviewImageHeight * scale, std::max(180.0f * scale, height - Style::spaceMd * scale));
    auto image = ui::image({
        .fit = ImageFit::Contain,
        .width = width,
        .height = imageHeight,
    });
    const int previewTargetSize = static_cast<int>(std::ceil(std::max(width, imageHeight)));
    image->setAsyncReadyCallback([]() { PanelManager::instance().refresh(); });

    if (m_tenor != nullptr && !item.previewUrl.empty()) {
      const auto path = m_tenor->localPathForUrl(item.previewUrl);
      std::error_code ec;
      if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
        (void)image->setSourceFileAsync(renderer, *m_asyncTextures, path.string(), previewTargetSize);
      } else {
        m_tenor->ensureLocalPreview(
            const_cast<TenorMediaItem&>(item),
            [this, &renderer, previewTargetSize](std::optional<std::filesystem::path> p) {
              if (p.has_value() && m_previewImage != nullptr) {
                (void)m_previewImage->setSourceFileAsync(renderer, *m_asyncTextures, p->string(), previewTargetSize);
                PanelManager::instance().refresh();
              }
            }
        );
      }
    }
    m_previewImage = image.get();
    m_previewContent->addChild(std::move(image));

    m_previewContent->layout(renderer);
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  if (m_tabMode == ClipboardTabMode::Emojis) {
    if (m_emojiIndices.empty() || m_selectedIndex >= m_emojiIndices.size()) {
      m_previewTitle->setText("Emojis");
      m_previewMeta->setText("No selection");
      return;
    }

    const auto* item = EmojiStore::instance().get(m_emojiIndices[m_selectedIndex]);
    if (item == nullptr) {
      return;
    }

    m_previewTitle->setText(item->name);
    m_previewTitle->setMaxWidth(width);
    m_previewMeta->setText("Category: " + item->category);
    m_previewMeta->setMaxWidth(width);

    const float scale = contentScale();
    auto emojiBox = ui::box({
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()),
        .radius = Style::scaledRadiusMd(scale),
        .width = width,
        .height = 140.0f * scale,
    });

    auto emojiLabel = ui::label({
        .text = item->emoji,
        .fontSize = 64.0f * scale,
        .baselineMode = LabelBaselineMode::TextFixedHeight,
    });
    emojiLabel->setPosition(std::max(0.0f, (width - 64.0f * scale) * 0.5f), std::max(0.0f, (140.0f * scale - 64.0f * scale) * 0.5f));
    emojiBox->addChild(std::move(emojiLabel));
    m_previewContent->addChild(std::move(emojiBox));

    if (!item->keywords.empty()) {
      std::string kwString = "Keywords: ";
      for (std::size_t i = 0; i < item->keywords.size(); ++i) {
        if (i > 0) kwString += ", ";
        kwString += item->keywords[i];
      }
      m_previewContent->addChild(
          ui::label({
              .text = kwString,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = width,
          })
      );
    }

    m_previewContent->layout(renderer);
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  const auto& history = m_clipboard != nullptr ? m_clipboard->history() : std::deque<ClipboardEntry>{};
  const std::size_t historyIndex = selectedHistoryIndex();
  if (history.empty() || historyIndex == static_cast<std::size_t>(-1)) {
    m_previewTitle->setText(i18n::tr("clipboard.entry.title"));
    m_previewMeta->setText("");

    m_previewContent->addChild(
        ui::label({
            .text = history.empty() ? i18n::tr("clipboard.empty.history-message")
                                    : i18n::tr("clipboard.empty.no-matches-message"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = width,
        })
    );
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  const auto& entry = history[historyIndex];
  m_previewTitle->setText(previewTitle(entry));
  m_previewTitle->setMaxWidth(width);
  m_previewMeta->setText(formatPreviewMeta(entry));
  m_previewMeta->setMaxWidth(width);

  if (m_previewPayloadIndex != historyIndex) {
    m_previewContent->addChild(
        ui::label({
            .text = i18n::tr("clipboard.preview.loading"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = width,
        })
    );
    m_lastPreviewWidth = width;
    m_lastPreviewHeight = height;
    return;
  }

  if (m_clipboard != nullptr && m_previewPayloadIndex != static_cast<std::size_t>(-1)
      && m_previewPayloadIndex != historyIndex) {
    m_clipboard->evictEntryPayload(m_previewPayloadIndex);
  }

  if (entry.isImage()) {
    const float scale = contentScale();
    const float imageHeight =
        std::min(kPreviewImageHeight * scale, std::max(180.0F * scale, height - Style::spaceMd * scale));
    auto image = ui::image({
        .fit = ImageFit::Contain,
        .width = width,
        .height = imageHeight,
    });
    m_previewImage = image.get();
    const int previewTargetSize = static_cast<int>(std::ceil(std::max(width, imageHeight)));
    image->setAsyncReadyCallback([this, historyIndex]() {
      if (m_clipboard == nullptr
          || m_previewMeta == nullptr
          || m_previewImage == nullptr
          || selectedHistoryIndex() != historyIndex) {
        return;
      }
      const auto& currentHistory = m_clipboard->history();
      if (historyIndex >= currentHistory.size() || !m_previewImage->hasImage()) {
        return;
      }
      m_previewMeta->setText(
          formatPreviewMeta(currentHistory[historyIndex], m_previewImage->sourceWidth(), m_previewImage->sourceHeight())
      );
      PanelManager::instance().refresh();
    });
    if (m_asyncTextures != nullptr && m_clipboard != nullptr) {
      const auto imageSource = m_clipboard->imageDataUri(historyIndex);
      if (imageSource.has_value()) {
        (void)image->setSourceFileAsync(renderer, *m_asyncTextures, *imageSource, previewTargetSize);
        if (image->hasImage()) {
          m_previewMeta->setText(formatPreviewMeta(entry, image->sourceWidth(), image->sourceHeight()));
        }
      }
    }
    m_previewContent->addChild(std::move(image));
  } else {
    Color previewColor;
    if (tryParseCssColor(entry.textPreview, previewColor)) {
      const float scale = contentScale();
      const float swatchH = std::round(80.0F * scale);
      m_previewContent->addChild(
          ui::box({
              .fill = fixedColorSpec(previewColor),
              .radius = Style::scaledRadiusMd(scale),
              .width = width,
              .height = swatchH,
          })
      );
    }

    if (m_clipboard != nullptr) {
      (void)m_clipboard->ensureEntryLoaded(historyIndex);
    }
    const auto& loadedEntry = m_clipboard != nullptr ? m_clipboard->history()[historyIndex] : entry;
    constexpr std::size_t kMaxPreviewChars = 8000;
    constexpr int kMaxPreviewLines = 200;

    std::string text(loadedEntry.data.begin(), loadedEntry.data.end());
    const bool truncated = text.size() > kMaxPreviewChars;
    if (truncated) {
      text.resize(kMaxPreviewChars);
    }

    std::string expanded;
    expanded.reserve(text.size());
    for (char ch : text) {
      if (ch == '\t') {
        expanded.append("    ");
      } else {
        expanded.push_back(ch);
      }
    }

    if (expanded.empty()) {
      m_previewContent->addChild(
          ui::label({
              .text = i18n::tr("clipboard.preview.empty-text-payload"),
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      m_previewContent->addChild(
          ui::label({
              .text = expanded,
              .fontSize = Style::fontSizeBody,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxWidth = width,
              .maxLines = kMaxPreviewLines,
          })
      );
      if (truncated) {
        m_previewContent->addChild(
            ui::label({
                .text = i18n::tr("clipboard.preview.truncated"),
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        );
      }
    }
  }

  m_previewContent->layout(renderer);
  m_lastPreviewWidth = width;
  m_lastPreviewHeight = height;
}

std::size_t ClipboardPanel::selectedHistoryIndex() const {
  if (m_selectedIndex >= m_filteredIndices.size()) {
    return static_cast<std::size_t>(-1);
  }
  return m_filteredIndices[m_selectedIndex];
}

void ClipboardPanel::applyFilter() {
  m_filteredIndices.clear();
  if (m_clipboard == nullptr) {
    return;
  }
  const auto& history = m_clipboard->history();

  std::string needle;
  needle.reserve(m_filterQuery.size());
  for (char ch : m_filterQuery) {
    needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  m_filteredIndices.reserve(history.size());
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (needle.empty()) {
      m_filteredIndices.push_back(i);
      continue;
    }
    std::string haystack = entryTitle(history[i]);
    for (char& ch : haystack) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (haystack.contains(needle)) {
      m_filteredIndices.push_back(i);
    }
  }
}

void ClipboardPanel::onFilterChanged(const std::string& text) {
  if (text == m_pendingFilterQuery && text == m_filterQuery) {
    return;
  }
  m_pendingFilterQuery = text;

  if (m_tabMode == ClipboardTabMode::Emojis) {
    m_filterQuery = text;
    searchEmojis(text);
    return;
  }

  const auto interval = (m_tabMode == ClipboardTabMode::Gifs) ? kGifSearchDebounceInterval : kFilterDebounceInterval;

  m_filterDebounceTimer.start(interval, [this]() {
    if (m_pendingFilterQuery == m_filterQuery) {
      return;
    }
    m_filterQuery = m_pendingFilterQuery;
    m_selectedIndex = 0;

    if (m_tabMode == ClipboardTabMode::History) {
      applyFilter();
      updateListState();
      if (m_listGrid != nullptr) {
        m_listGrid->notifyDataChanged();
        m_listGrid->setSelectedIndex(
            m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
        );
      }
      schedulePreviewPayloadRefresh(true);
    } else if (m_tabMode == ClipboardTabMode::Gifs) {
      searchGifs(m_filterQuery);
    }

    m_pendingScrollToSelected = true;
    PanelManager::instance().refresh();
  });
}

void ClipboardPanel::selectIndex(std::size_t index) {
  if (m_selectedIndex == index) {
    return;
  }
  resetDeleteConfirmation();
  m_selectedIndex = index;
  if (m_listGrid != nullptr) {
    m_listGrid->setSelectedIndex(index);
  }

  if (m_tabMode == ClipboardTabMode::History) {
    schedulePreviewPayloadRefresh(true);
  } else {
    m_lastPreviewWidth = -1.0f;
    m_lastPreviewHeight = -1.0f;
  }

  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::requestDeleteSelectedEntry() {
  if (m_tabMode != ClipboardTabMode::History || m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size()) {
    return;
  }
  const auto& entry = history[historyIndex];
  const bool confirmClear = m_config == nullptr || m_config->config().shell.clipboardConfirmClearHistory;
  if (!confirmClear && !entry.pinned) {
    resetClearConfirmation();
    performDeleteSelectedEntry();
    return;
  }

  const std::string storageId = entry.storageId;
  if (m_deleteConfirmStorageId == storageId) {
    resetDeleteConfirmation();
  } else {
    resetClearConfirmation();
    m_deleteConfirmStorageId = storageId;
  }
  updateListState();
  updatePreviewActions();
  PanelManager::instance().refresh();
}

void ClipboardPanel::performDeleteSelectedEntry() {
  if (m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size()) {
    return;
  }
  resetDeleteConfirmation();
  resetClearConfirmation();
  const std::size_t filterPos = m_selectedIndex;
  if (!m_clipboard->removeHistoryEntry(historyIndex)) {
    return;
  }
  applyFilter();
  if (m_filteredIndices.empty()) {
    m_selectedIndex = 0;
  } else {
    m_selectedIndex = std::min(filterPos, m_filteredIndices.size() - 1);
  }
  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
  }
  schedulePreviewPayloadRefresh(false);
  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::deleteSelectedEntry() {
  if (m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size()) {
    return;
  }
  if (m_deleteConfirmStorageId != history[historyIndex].storageId) {
    return;
  }
  performDeleteSelectedEntry();
}

void ClipboardPanel::selectByStorageId(std::string storageId) {
  applyFilter();

  std::size_t newSelected = 0;
  const auto& history = m_clipboard->history();
  for (std::size_t pos = 0; pos < m_filteredIndices.size(); ++pos) {
    const std::size_t idx = m_filteredIndices[pos];
    if (idx < history.size() && history[idx].storageId == storageId) {
      newSelected = pos;
      break;
    }
  }
  m_selectedIndex = m_filteredIndices.empty() ? 0 : newSelected;

  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
  }

  schedulePreviewPayloadRefresh(false);
}

void ClipboardPanel::togglePinSelected() {
  if (m_tabMode != ClipboardTabMode::History || m_clipboard == nullptr) {
    return;
  }
  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }
  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size()) {
    return;
  }

  const std::string storageId = history[historyIndex].storageId;
  const bool nextPinned = !history[historyIndex].pinned;
  if (!m_clipboard->setEntryPinned(historyIndex, nextPinned)) {
    return;
  }

  selectByStorageId(storageId);
  m_pendingScrollToSelected = true;
  PanelManager::instance().refresh();
}

void ClipboardPanel::requestClearUnpinnedHistory() {
  if (m_tabMode != ClipboardTabMode::History || m_clipboard == nullptr) {
    return;
  }

  const auto& history = m_clipboard->history();
  if (history.empty()) {
    resetClearConfirmation();
    updateListState();
    return;
  }

  const bool confirmClear = m_config == nullptr || m_config->config().shell.clipboardConfirmClearHistory;
  if (!confirmClear) {
    resetClearConfirmation();
    resetDeleteConfirmation();
    performClearUnpinnedHistory();
    return;
  }

  if (m_clearConfirm) {
    resetClearConfirmation();
  } else {
    resetDeleteConfirmation();
    m_clearConfirm = true;
  }
  updateListState();
  updatePreviewActions();
  PanelManager::instance().refresh();
}

void ClipboardPanel::performClearUnpinnedHistory() {
  if (m_clipboard == nullptr) {
    return;
  }
  const auto& history = m_clipboard->history();
  const bool hasUnpinned = std::ranges::any_of(history, [](const ClipboardEntry& entry) { return !entry.pinned; });
  if (!hasUnpinned) {
    return;
  }
  resetClearConfirmation();
  resetDeleteConfirmation();
  m_clipboard->clearUnpinnedHistory();
  applyFilter();
  if (m_filteredIndices.empty()) {
    m_selectedIndex = 0;
  } else {
    m_selectedIndex = std::min(m_selectedIndex, m_filteredIndices.size() - 1);
  }
  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(
        m_filteredIndices.empty() ? std::nullopt : std::optional<std::size_t>(m_selectedIndex)
    );
  }
  schedulePreviewPayloadRefresh(false);
  m_pendingScrollToSelected = true;
  if (m_clipboard->history().empty()) {
    PanelManager::instance().close();
  } else {
    PanelManager::instance().refresh();
  }
}

void ClipboardPanel::performClearAllHistory() {
  if (m_clipboard == nullptr || m_clipboard->history().empty()) {
    return;
  }
  resetClearConfirmation();
  resetDeleteConfirmation();
  m_clipboard->clearHistory();
  applyFilter();
  m_selectedIndex = 0;
  updateListState();
  if (m_listGrid != nullptr) {
    m_listGrid->notifyDataChanged();
    m_listGrid->setSelectedIndex(std::nullopt);
  }
  schedulePreviewPayloadRefresh(false);
  m_pendingScrollToSelected = false;
  if (m_clipboard->history().empty()) {
    PanelManager::instance().close();
  } else {
    PanelManager::instance().refresh();
  }
}

void ClipboardPanel::clearUnpinnedHistory() {
  if (!m_clearConfirm) {
    return;
  }
  performClearUnpinnedHistory();
}

void ClipboardPanel::clearAllHistory() {
  if (!m_clearConfirm) {
    return;
  }
  performClearAllHistory();
}

void ClipboardPanel::resetDeleteConfirmation() { m_deleteConfirmStorageId.clear(); }

void ClipboardPanel::resetClearConfirmation() { m_clearConfirm = false; }

void ClipboardPanel::runImageAction() {
  if (m_tabMode != ClipboardTabMode::History || m_clipboard == nullptr || m_config == nullptr) {
    return;
  }

  const std::string configuredCommand = StringUtils::trim(m_config->config().shell.clipboardImageActionCommand);
  if (configuredCommand.empty()) {
    return;
  }

  const std::size_t historyIndex = selectedHistoryIndex();
  if (historyIndex == static_cast<std::size_t>(-1)) {
    return;
  }

  const auto& history = m_clipboard->history();
  if (historyIndex >= history.size() || !history[historyIndex].isImage()) {
    return;
  }

  const std::optional<std::string> exportedPath = m_clipboard->exportEntryForExternalTool(historyIndex);
  if (!exportedPath.has_value()) {
    kLog.warn("clipboard image action failed: selected image could not be exported");
    return;
  }

  const std::string command = buildImageActionCommand(configuredCommand, *exportedPath);
  if (!process::runAsync(command)) {
    kLog.warn("clipboard image action failed to launch: {}", configuredCommand);
    return;
  }
  PanelManager::instance().close();
}

void ClipboardPanel::activateSelected() {
  if (m_tabMode == ClipboardTabMode::History) {
    if (m_clipboard == nullptr) {
      return;
    }
    const std::size_t historyIndex = selectedHistoryIndex();
    if (historyIndex == static_cast<std::size_t>(-1)) {
      return;
    }
    if (!m_clipboard->ensureEntryLoaded(historyIndex)) {
      return;
    }
    const ClipboardEntry entry = m_clipboard->history()[historyIndex];
    const bool wasPinned = entry.pinned;
    const bool promoted = wasPinned ? false : m_clipboard->promoteEntry(historyIndex);
    const bool copied = m_clipboard->copyEntry(entry);
    if (copied || promoted) {
      if (!wasPinned) {
        selectByStorageId(entry.storageId);
      }
      PanelManager::instance().refresh();
      if (m_activateCallback) {
        m_activateCallback(entry);
      }
    }
    return;
  }

  if (m_tabMode == ClipboardTabMode::Gifs) {
    if (m_gifResults.empty() || m_selectedIndex >= m_gifResults.size() || m_clipboard == nullptr) {
      return;
    }
    const auto item = m_gifResults[m_selectedIndex];
    if (m_tenor != nullptr) {
      m_tenor->fetchMediaBytes(item.fullUrl, [this, item](std::vector<std::uint8_t> bytes) {
        if (!bytes.empty() && m_clipboard != nullptr) {
          m_clipboard->copyImageGif(std::move(bytes));
          ClipboardEntry entry;
          entry.dataMimeType = "image/gif";
          entry.textPreview = item.title;
          PanelManager::instance().refresh();
          if (m_activateCallback) {
            m_activateCallback(entry);
          } else {
            PanelManager::instance().closePanel(false);
          }
        }
      });
    }
    return;
  }

  if (m_tabMode == ClipboardTabMode::Emojis) {
    if (m_emojiIndices.empty() || m_selectedIndex >= m_emojiIndices.size() || m_clipboard == nullptr) {
      return;
    }
    const auto* item = EmojiStore::instance().get(m_emojiIndices[m_selectedIndex]);
    if (item != nullptr && m_clipboard != nullptr) {
      m_clipboard->copyText(item->emoji);
      ClipboardEntry entry;
      entry.dataMimeType = "text/plain";
      entry.textPreview = item->emoji;
      PanelManager::instance().refresh();
      if (m_activateCallback) {
        m_activateCallback(entry);
      } else {
        PanelManager::instance().closePanel(false);
      }
    }
  }
}

bool ClipboardPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  // Tab / Shift+Tab cycles between modes
  if (KeySymbol::isTab(sym)) {
    const auto current = static_cast<int>(m_tabMode);
    const bool isShift = (modifiers & KeyMod::Shift) != 0 || (sym == XKB_KEY_ISO_Left_Tab);
    const auto next = isShift ? (current + 2) % 3 : (current + 1) % 3;
    if (m_tabSegmented != nullptr) {
      m_tabSegmented->setSelectedIndex(static_cast<std::size_t>(next));
    }
    setTabMode(static_cast<ClipboardTabMode>(next));
    return true;
  }

  const std::size_t totalItems = (m_tabMode == ClipboardTabMode::History)  ? m_filteredIndices.size()
      : (m_tabMode == ClipboardTabMode::Gifs)                              ? m_gifResults.size()
                                                                           : m_emojiIndices.size();

  if (totalItems == 0) {
    return false;
  }

  const auto moveSelection = [this, totalItems](int delta) {
    const int last = static_cast<int>(totalItems - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    selectIndex(static_cast<std::size_t>(next));
  };

  if (KeySymbol::isPageUp(sym)) {
    const int stride = m_listGrid != nullptr ? static_cast<int>(m_listGrid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    const int stride = m_listGrid != nullptr ? static_cast<int>(m_listGrid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  const int cols = m_listGrid != nullptr ? std::max(1, static_cast<int>(m_listGrid->layoutColumnCount())) : 1;

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(-cols);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(cols);
    return true;
  }

  if (cols > 1) {
    if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
      moveSelection(-1);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
      moveSelection(1);
      return true;
    }
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  if (m_tabMode == ClipboardTabMode::History && KeybindMatcher::matches(KeybindAction::Delete, sym, modifiers)) {
    const std::size_t historyIndex = selectedHistoryIndex();
    const auto& history = m_clipboard->history();
    if (historyIndex < history.size() && m_deleteConfirmStorageId == history[historyIndex].storageId) {
      deleteSelectedEntry();
    } else {
      requestDeleteSelectedEntry();
    }
    return true;
  }

  return false;
}

void ClipboardPanel::scrollToSelected() {
  const std::size_t totalItems = (m_tabMode == ClipboardTabMode::History)  ? m_filteredIndices.size()
      : (m_tabMode == ClipboardTabMode::Gifs)                              ? m_gifResults.size()
                                                                           : m_emojiIndices.size();

  if (m_listGrid == nullptr || m_selectedIndex >= totalItems) {
    return;
  }
  m_listGrid->scrollToIndex(m_selectedIndex);
}
