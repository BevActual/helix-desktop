#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/ai_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<AiWidget::Options>& aiWidgetDefinition();
