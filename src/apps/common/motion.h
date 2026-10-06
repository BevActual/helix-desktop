#pragma once

#include "ui/style.h"

#include <adwaita.h>
#include <algorithm>

namespace helix::apps {

namespace detail {

struct RevealState {
  GtkWidget* widget = nullptr;
  int marginFrom = 0;
  int marginTo = 0;
  guint durationMs = Style::animNormal;
};

inline void playReveal(RevealState* reveal) {
  if (!reveal) {
    return;
  }
  if (!GTK_IS_WIDGET(reveal->widget) || gtk_widget_in_destruction(reveal->widget)) {
    if (GTK_IS_WIDGET(reveal->widget)) {
      g_object_unref(reveal->widget);
    }
    delete reveal;
    return;
  }
  if (!gtk_widget_get_mapped(reveal->widget) || !adw_get_enable_animations(reveal->widget)) {
    gtk_widget_set_opacity(reveal->widget, 1.0);
    gtk_widget_set_margin_top(reveal->widget, reveal->marginTo);
    g_object_unref(reveal->widget);
    delete reveal;
    return;
  }

  auto* target = adw_callback_animation_target_new(
      +[](double value, gpointer user) {
        auto* reveal = static_cast<RevealState*>(user);
        if (!GTK_IS_WIDGET(reveal->widget) || gtk_widget_in_destruction(reveal->widget)) {
          return;
        }
        const double t = std::clamp(value, 0.0, 1.0);
        gtk_widget_set_opacity(reveal->widget, t);
        const auto margin = reveal->marginFrom + (reveal->marginTo - reveal->marginFrom) * t;
        gtk_widget_set_margin_top(reveal->widget, int(margin + (margin >= 0 ? 0.5 : -0.5)));
      },
      reveal, nullptr
  );
  auto* animation = adw_timed_animation_new(reveal->widget, 0.0, 1.0, reveal->durationMs, target);
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(animation), ADW_EASE_OUT_CUBIC);
  g_signal_connect(
      animation, "done",
      G_CALLBACK(+[](AdwAnimation*, gpointer user) {
        auto* reveal = static_cast<RevealState*>(user);
        if (GTK_IS_WIDGET(reveal->widget) && !gtk_widget_in_destruction(reveal->widget)) {
          gtk_widget_set_opacity(reveal->widget, 1.0);
          gtk_widget_set_margin_top(reveal->widget, reveal->marginTo);
        }
        if (GTK_IS_WIDGET(reveal->widget)) {
          g_object_unref(reveal->widget);
        }
        delete reveal;
      }),
      reveal
  );
  adw_animation_play(animation);
}

}  // namespace detail

// Soft fade + slight rise. Skips when Motion is off (adw_get_enable_animations).
inline void revealWidget(GtkWidget* widget, guint delayMs = 0, guint durationMs = Style::animNormal) {
  if (!widget || !GTK_IS_WIDGET(widget)) {
    return;
  }
  if (!adw_get_enable_animations(widget)) {
    return;
  }

  const int baseMargin = gtk_widget_get_margin_top(widget);
  constexpr int risePx = 10;
  gtk_widget_set_opacity(widget, 0.0);
  gtk_widget_set_margin_top(widget, baseMargin + risePx);

  auto* reveal = new detail::RevealState{widget, baseMargin + risePx, baseMargin, durationMs};
  g_object_ref(widget);

  if (delayMs == 0) {
    detail::playReveal(reveal);
    return;
  }

  g_timeout_add(
      delayMs,
      +[](gpointer data) -> gboolean {
        detail::playReveal(static_cast<detail::RevealState*>(data));
        return G_SOURCE_REMOVE;
      },
      reveal
  );
}

// Stagger direct children of a box — explains store-home hierarchy without delaying work.
inline void staggerRevealChildren(
    GtkWidget* parent, guint stepMs = 45, guint durationMs = Style::animNormal, guint startDelayMs = 30
) {
  if (!parent || !GTK_IS_WIDGET(parent) || !adw_get_enable_animations(parent)) {
    return;
  }
  guint index = 0;
  for (auto* child = gtk_widget_get_first_child(parent); child; child = gtk_widget_get_next_sibling(child)) {
    if (!gtk_widget_get_visible(child)) {
      continue;
    }
    const auto delay = startDelayMs + std::min(index, 8u) * stepMs;
    revealWidget(child, delay, durationMs);
    ++index;
  }
}

inline void revealProgress(GtkWidget* progress, bool show) {
  if (!progress || !GTK_IS_WIDGET(progress)) {
    return;
  }
  if (!show) {
    gtk_widget_set_visible(progress, false);
    gtk_widget_set_opacity(progress, 1.0);
    return;
  }
  const bool wasHidden = !gtk_widget_get_visible(progress);
  gtk_widget_set_visible(progress, true);
  if (wasHidden) {
    revealWidget(progress, 0, Style::animFast);
  }
}

// Three pulsing dots for inline/status loading. Respects reduced motion.
inline GtkWidget* loadingDots(int pixelSize = 12) {
  auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, std::max(4, pixelSize / 2));
  gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL, "Loading", -1);

  GtkWidget* dots[3] = {};
  for (int i = 0; i < 3; ++i) {
    auto* dot = gtk_label_new("●");
    gtk_widget_set_size_request(dot, pixelSize, pixelSize);
    gtk_widget_set_opacity(dot, 0.25);
    gtk_widget_add_css_class(dot, "dim-label");
    gtk_box_append(GTK_BOX(row), dot);
    dots[i] = dot;
  }

  if (!adw_get_enable_animations(row)) {
    for (auto* dot : dots) {
      gtk_widget_set_opacity(dot, 0.7);
    }
    return row;
  }

  struct Pulse {
    GtkWidget* row = nullptr;
    GtkWidget* dots[3] = {};
    guint source = 0;
    guint phase = 0;
  };
  auto* pulse = new Pulse{row, {dots[0], dots[1], dots[2]}, 0, 0};
  pulse->source = g_timeout_add(
      Style::animNormal,
      +[](gpointer data) -> gboolean {
        auto* pulse = static_cast<Pulse*>(data);
        // Destroy handler owns Pulse lifetime; bail if the row is going away.
        if (!GTK_IS_WIDGET(pulse->row) || gtk_widget_in_destruction(pulse->row)) {
          return G_SOURCE_REMOVE;
        }
        if (!gtk_widget_get_mapped(pulse->row)) {
          return G_SOURCE_CONTINUE;
        }
        pulse->phase = (pulse->phase + 1) % 3;
        for (guint i = 0; i < 3; ++i) {
          if (!GTK_IS_WIDGET(pulse->dots[i])) {
            continue;
          }
          // Active dot bright; neighbors dim — reads as a traveling pulse.
          const double opacity = i == pulse->phase ? 1.0 : (i == (pulse->phase + 2) % 3 ? 0.45 : 0.2);
          gtk_widget_set_opacity(pulse->dots[i], opacity);
        }
        return G_SOURCE_CONTINUE;
      },
      pulse
  );
  g_signal_connect_data(
      row, "destroy",
      G_CALLBACK(+[](GtkWidget*, gpointer data) {
        auto* pulse = static_cast<Pulse*>(data);
        if (pulse->source) {
          g_source_remove(pulse->source);
          pulse->source = 0;
        }
        delete pulse;
      }),
      pulse, nullptr, G_CONNECT_DEFAULT
  );
  return row;
}

// Status page loading: animated Adw spinner + optional three-dot accent under the title area.
inline void applyLoadingStatusPage(AdwStatusPage* page, const char* title, const char* description) {
  if (!page) {
    return;
  }
  adw_status_page_set_title(page, title ? title : "Loading");
  adw_status_page_set_description(page, description ? description : "");
  auto* paintable = adw_spinner_paintable_new(GTK_WIDGET(page));
  adw_status_page_set_paintable(page, GDK_PAINTABLE(paintable));
  // Keep a compact dot pulse in the child slot so “loading dots” read clearly while switching.
  auto* dots = loadingDots(10);
  gtk_widget_set_margin_top(dots, 8);
  adw_status_page_set_child(page, dots);
}

}  // namespace helix::apps
