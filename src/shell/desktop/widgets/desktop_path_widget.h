#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/controls/button.h"

#include <string>

class DesktopPathWidget : public DesktopWidget {
public:
  struct Options {
    std::string path;
    std::string label;
    std::string glyph;
    bool preferFolder = true;
    bool showBackground = true;
  };

  explicit DesktopPathWidget(Options options);

  void create() override;
  void setEditorPreview(bool enabled) noexcept override;
  void layout(Renderer& renderer) override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;
  void syncButtonContent();
  void syncButtonStyle();
  void openPath() const;
  [[nodiscard]] std::string displayLabel() const;
  [[nodiscard]] std::string displayGlyph() const;

  std::string m_path;
  std::string m_label;
  std::string m_glyph;
  bool m_preferFolder = true;
  bool m_showBackground = true;

  Button* m_button = nullptr;
  bool m_editorPreview = false;
  bool m_fillToBox = false;
};
