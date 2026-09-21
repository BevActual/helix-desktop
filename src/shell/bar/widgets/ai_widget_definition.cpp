#include "shell/bar/widgets/ai_widget_definition.h"

const noctalia::bar::WidgetDefinition<AiWidget::Options>& aiWidgetDefinition() {
  using noctalia::bar::field;
  using Options = AiWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "ai",
      .fields = {
          field<&Options::maxWidth>({
              .key = "max_length",
              .minValue = 40.0,
              .maxValue = 800.0,
              .step = 1.0,
          }),
      },
  };
  return definition;
}
