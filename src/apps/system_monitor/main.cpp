#include "apps/system_monitor/proc_reader.h"
#include "apps/system_monitor/report_reader.h"
#include "apps/common/appearance.h"
#include "apps/common/preview_export.h"
#include "desktop/protocol.h"

#include <adwaita.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
using namespace helix::monitor;
using Json = nlohmann::json;
std::string bytes(double value) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"}; unsigned unit = 0;
  while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
  char result[64]; std::snprintf(result, sizeof(result), "%.1f %s", value, units[unit]); return result;
}
std::string percentage(double value) {
  char result[32]; std::snprintf(result, sizeof(result), "%.1f%%", value); return result;
}
std::string duration(std::uint64_t seconds) {
  return std::to_string(seconds / 3600) + "h " + std::to_string((seconds / 60) % 60) + "m " + std::to_string(seconds % 60) + "s";
}
std::string validText(const std::string& text) {
  auto* valid = g_utf8_make_valid(text.data(), text.size()); std::string result(valid); g_free(valid); return result;
}
std::string stateName(char state) {
  switch (state) {
    case 'R': return "Running"; case 'S': return "Sleeping"; case 'D': return "Waiting for I/O";
    case 'T': case 't': return "Stopped"; case 'Z': return "Zombie"; case 'I': return "Idle";
    default: return "Unknown";
  }
}
struct Monitor {
  AdwApplication* app;
  AdwApplicationWindow* window = nullptr;
  AdwNavigationSplitView* split = nullptr;
  AdwNavigationPage* content = nullptr;
  GtkStack* stack = nullptr;
  GtkListBox* sidebar = nullptr;
  GtkListBoxRow *overviewRow = nullptr, *processesRow = nullptr, *reportsRow = nullptr;
  GtkListBox* processes = nullptr;
  AdwPreferencesGroup* processGroup = nullptr;
  AdwActionRow *cpuRow = nullptr, *memoryRow = nullptr, *swapRow = nullptr, *networkRow = nullptr,
               *uptimeRow = nullptr, *diskRow = nullptr;
  GtkLevelBar *cpuBar = nullptr, *memoryBar = nullptr;
  GtkDrawingArea *cpuChart = nullptr, *memoryChart = nullptr, *hourChart = nullptr;
  struct HistoryPoint { std::chrono::steady_clock::time_point time; std::optional<double> cpu, memory; };
  std::deque<HistoryPoint> history;
  std::array<std::uint64_t, 24> hourValues{};
  GtkLabel* status = nullptr;
  GtkWidget* processEmpty = nullptr;
  GtkSwitch* live = nullptr;
  GtkSearchEntry* searchEntry = nullptr;
  AdwPreferencesGroup *reportSummary = nullptr, *reportApps = nullptr, *reportHours = nullptr;
  AdwExpanderRow* hourDetails = nullptr;
  AdwComboRow* reportDate = nullptr;
  AdwActionRow* reportTotal = nullptr;
  std::vector<GtkWidget*> reportAppRows, reportHourRows;
  Reports reports;
  std::vector<std::string> reportDates;
  std::string selectedDate;
  bool selectingDate = false;
  std::chrono::steady_clock::time_point reportsRead;
  GtkCssProvider* css = nullptr;
  GDBusConnection* bus = nullptr;
  GCancellable* appearanceCancel = nullptr;
  unsigned appearanceChanged = 0, ownerChanged = 0;
  bool appearancePending = false;
  Json appearance = Json::object();
  unsigned timer = 0, sort = 0;
  bool sampling = false;
  std::string page = "overview", query;
  Snapshot latest;
  std::map<int, Process> previous;
  std::map<int, double> usage;
  std::map<int, AdwActionRow*> rows;
  explicit Monitor(AdwApplication* application) : app(application) {}
  ~Monitor() {
    if (timer) g_source_remove(timer);
    if (appearanceCancel) { g_cancellable_cancel(appearanceCancel); g_object_unref(appearanceCancel); }
    if (bus) {
      if (appearanceChanged) g_dbus_connection_signal_unsubscribe(bus, appearanceChanged);
      if (ownerChanged) g_dbus_connection_signal_unsubscribe(bus, ownerChanged);
      g_object_unref(bus);
    }
    if (css) {
      g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), this);
      if (auto* display = gdk_display_get_default()) gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(css));
      g_object_unref(css);
    }
  }
  void refreshAppearance() {
    if (!bus || appearancePending) return;
    appearancePending = true;
    // Monitoring never starts or depends on the desktop service.
    g_dbus_connection_call(bus, helix::busName, helix::objectPath, helix::appearanceInterface, "GetSnapshot",
      nullptr, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, appearanceCancel,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        auto& self = *static_cast<Monitor*>(data); self.appearancePending = false;
        auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
        self.appearance = Json::object();
        if (reply) {
          try { self.appearance = helix::fromVariant(reply).at(0); } catch (...) {}
          g_variant_unref(reply);
        }
        helix::apps::applyAppearance(self.css, self.appearance);
      }, this);
  }
  static AdwActionRow* row(AdwPreferencesGroup* group, const char* title, const char* subtitle = nullptr) {
    auto* widget = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(widget), false);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), title);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(widget), 2);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(widget), 2);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), subtitle);
    if (group) adw_preferences_group_add(group, widget);
    return ADW_ACTION_ROW(widget);
  }
  static AdwPreferencesGroup* group(GtkBox* box, const char* name, const char* description = nullptr) {
    auto* widget = adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(widget), name);
    if (description) adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(widget), description);
    gtk_box_append(box, widget); return ADW_PREFERENCES_GROUP(widget);
  }
  static GtkWidget* scrolled(GtkWidget* child) {
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child); return scroll;
  }
  static GtkBox* box() {
    auto* widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    for (const auto* side : {"margin-top", "margin-bottom", "margin-start", "margin-end"}) g_object_set(widget, side, 18, nullptr);
    return GTK_BOX(widget);
  }
  static GtkLevelBar* level(AdwActionRow* row) {
    auto* bar = gtk_level_bar_new_for_interval(0, 100);
    gtk_widget_set_size_request(bar, 64, -1); gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(row, bar); return GTK_LEVEL_BAR(bar);
  }
  GtkWidget* historyChart(const char* heading, bool memory) {
    auto* chart = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* label = gtk_label_new(heading); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "heading"); gtk_box_append(GTK_BOX(chart), label);
    auto* area = GTK_DRAWING_AREA(gtk_drawing_area_new()); gtk_drawing_area_set_content_height(area, 130);
    gtk_widget_set_hexpand(GTK_WIDGET(area), true); gtk_widget_add_css_class(GTK_WIDGET(area), "accent");
    if (memory) memoryChart = area; else cpuChart = area;
    gtk_drawing_area_set_draw_func(area, +[](GtkDrawingArea* area, cairo_t* context, int width, int height, gpointer data) {
      const auto& self = *static_cast<Monitor*>(data); GdkRGBA color; gtk_widget_get_color(GTK_WIDGET(area), &color);
      const double left = 1, right = std::max(1, width - 1), top = 4, bottom = std::max(4, height - 4);
      cairo_set_source_rgba(context, color.red, color.green, color.blue, .16); cairo_set_line_width(context, 1);
      for (unsigned line = 0; line <= 4; ++line) {
        const auto y = top + (bottom - top) * line / 4;
        cairo_move_to(context, left, y); cairo_line_to(context, right, y);
      }
      cairo_stroke(context); cairo_set_source_rgba(context, color.red, color.green, color.blue, 1); cairo_set_line_width(context, 2);
      const auto now = self.latest.sampled; bool drawing = false;
      const double period = self.history.empty() ? 120 : std::clamp(std::chrono::duration<double>(now - self.history.front().time).count(), 10.0, 120.0);
      std::chrono::steady_clock::time_point previous;
      for (const auto& point : self.history) {
        const auto value = area == self.memoryChart ? point.memory : point.cpu;
        const auto age = std::chrono::duration<double>(now - point.time).count();
        if (!value || age < 0 || age > period) { drawing = false; continue; }
        const double x = left + (right - left) * (1 - age / period), y = bottom - (bottom - top) * std::clamp(*value, 0.0, 100.0) / 100;
        if (!drawing || point.time - previous > std::chrono::seconds(5)) cairo_move_to(context, x, y);
        else cairo_line_to(context, x, y);
        drawing = true; previous = point.time;
      }
      cairo_stroke(context);
    }, this, nullptr);
    gtk_box_append(GTK_BOX(chart), GTK_WIDGET(area));
    auto* caption = gtk_label_new("0–100% · Recent activity, up to 2 minutes"); gtk_label_set_xalign(GTK_LABEL(caption), 0);
    gtk_label_set_wrap(GTK_LABEL(caption), true);
    gtk_widget_add_css_class(caption, "dim-label"); gtk_box_append(GTK_BOX(chart), caption); return chart;
  }
  void select(std::string id) {
    if (id != "overview" && id != "processes" && id != "screen-time") return;
    page = std::move(id); gtk_stack_set_visible_child_name(stack, page.c_str());
    gtk_list_box_select_row(sidebar, page == "overview" ? overviewRow : page == "processes" ? processesRow : reportsRow);
    adw_navigation_page_set_title(content, page == "overview" ? "Overview" : page == "processes" ? "Processes" : "Screen Time");
    adw_navigation_split_view_set_show_content(split, true);
  }
  void showReportDay() {
    for (auto* row : reportAppRows) adw_preferences_group_remove(reportApps, row);
    for (auto* row : reportHourRows) adw_expander_row_remove(hourDetails, row);
    reportAppRows.clear(); reportHourRows.clear();
    hourValues.fill(0); gtk_widget_queue_draw(GTK_WIDGET(hourChart));
    auto day = std::ranges::find_if(reports.days, [this](const ReportDay& day) { return day.date == selectedDate; });
    if (day == reports.days.end()) {
      adw_action_row_set_subtitle(reportTotal, "No saved usage for this day"); return;
    }
    hourValues = day->hourly; gtk_widget_queue_draw(GTK_WIDGET(hourChart));
    gtk_accessible_update_property(GTK_ACCESSIBLE(hourChart), GTK_ACCESSIBLE_PROPERTY_LABEL,
      ("Hourly screen time for " + selectedDate + ". Exact durations are listed below.").c_str(), -1);
    std::uint64_t total = 0, hourlyTotal = 0;
    for (const auto& [app, seconds] : day->apps) total += seconds;
    for (const auto seconds : day->hourly) hourlyTotal += seconds;
    adw_action_row_set_subtitle(reportTotal, duration(std::max(total, hourlyTotal)).c_str());
    std::map<std::string, std::string> names;
    auto* apps = g_app_info_get_all();
    for (auto* node = apps; node; node = node->next) {
      auto* app = G_APP_INFO(node->data); const auto* id = g_app_info_get_id(app); if (!id) continue;
      std::string key(id); names[key] = g_app_info_get_display_name(app);
      if (key.ends_with(".desktop")) { key.resize(key.size() - 8); names[key] = g_app_info_get_display_name(app); }
    }
    g_list_free_full(apps, g_object_unref);
    std::vector<std::pair<std::string, std::uint64_t>> ordered(day->apps.begin(), day->apps.end());
    std::ranges::sort(ordered, [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    for (const auto& [app, seconds] : ordered) {
      if (!seconds) continue;
      auto* appRow = row(reportApps, validText(names.contains(app) ? names.at(app) : app).c_str(), duration(seconds).c_str());
      auto* bar = gtk_progress_bar_new(); gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), std::min(1.0, double(seconds) / double(std::max<std::uint64_t>(1, std::max(total, hourlyTotal)))));
      gtk_widget_set_size_request(bar, 64, -1); gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
      gtk_accessible_update_property(GTK_ACCESSIBLE(bar), GTK_ACCESSIBLE_PROPERTY_LABEL, "Share of recorded daily usage", -1);
      adw_action_row_add_suffix(appRow, bar); reportAppRows.push_back(GTK_WIDGET(appRow));
    }
    if (reportAppRows.empty()) reportAppRows.push_back(GTK_WIDGET(row(reportApps, "No application usage recorded")));
    if (!hourlyTotal) reportHourRows.push_back(GTK_WIDGET(row(nullptr, "No hourly usage recorded")));
    else for (unsigned hour = 0; hour < 24; ++hour) {
      char label[32]; std::snprintf(label, sizeof(label), "%02u:00–%02u:00", hour, (hour + 1) % 24);
      reportHourRows.push_back(GTK_WIDGET(row(nullptr, label, duration(day->hourly[hour]).c_str())));
    }
    for (auto* row : reportHourRows) adw_expander_row_add_row(hourDetails, row);
  }
  void acceptReports(Reports incoming) {
    if (incoming.state == Reports::State::Invalid) {
      adw_preferences_group_set_description(reportSummary, "Saved reports could not be read. Previously loaded data remains visible; the next refresh will retry.");
      return;
    }
    reports = std::move(incoming);
    adw_preferences_group_set_description(reportSummary, reports.state == Reports::State::Missing ?
      "No saved reports. Recording is opt-in in Settings → Privacy &amp; Security on Helix. Other desktops can view existing saved reports." :
      "Saved by Helix Shell while recording is enabled. This view refreshes every 10 seconds; unsaved activity is not included. Recording preferences are in Settings → Privacy &amp; Security; clear history in the current desktop Screen Time controls.");
    std::vector<std::string> dates; for (const auto& day : reports.days) dates.push_back(day.date);
    if (dates != reportDates) {
      selectingDate = true;
      reportDates = std::move(dates); auto* model = gtk_string_list_new(nullptr);
      for (const auto& date : reportDates) gtk_string_list_append(model, date.c_str());
      adw_combo_row_set_model(reportDate, G_LIST_MODEL(model)); g_object_unref(model);
      if (std::ranges::find(reportDates, selectedDate) == reportDates.end()) selectedDate = reportDates.empty() ? "" : reportDates.front();
      const auto index = std::ranges::find(reportDates, selectedDate);
      adw_combo_row_set_selected(reportDate, index == reportDates.end() ? GTK_INVALID_LIST_POSITION : static_cast<unsigned>(index - reportDates.begin()));
      selectingDate = false;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(reportDate), !reportDates.empty()); showReportDay();
  }
  void details(int pid) {
    auto it = previous.find(pid); if (it == previous.end()) return;
    const auto& process = it->second;
    const auto text = "PID " + std::to_string(pid) + " · " + stateName(process.state) + "\n" +
      "Memory: " + bytes(process.resident) + "\nThreads: " + std::to_string(process.threads) +
      "\nCPU: " + (usage.contains(pid) ? percentage(usage.at(pid)) : "Waiting for next sample") +
      "\n\nCPU uses one core as 100%. Multithreaded processes can exceed 100%.";
    auto* dialog = adw_alert_dialog_new(validText(process.name).c_str(), text.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  }
  void showProcesses() {
    for (auto it = rows.begin(); it != rows.end();) {
      if (!previous.contains(it->first)) { gtk_list_box_remove(processes, GTK_WIDGET(it->second)); it = rows.erase(it); }
      else ++it;
    }
    for (const auto& [pid, process] : previous) {
      AdwActionRow* widget;
      if (rows.contains(pid)) widget = rows.at(pid);
      else {
        widget = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(widget), false);
        adw_action_row_set_title_lines(widget, 2); adw_action_row_set_subtitle_lines(widget, 2);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(widget), true);
        adw_action_row_add_suffix(widget, gtk_image_new_from_icon_name("go-next-symbolic"));
        g_object_set_data(G_OBJECT(widget), "pid", GINT_TO_POINTER(pid));
        gtk_list_box_append(processes, GTK_WIDGET(widget)); rows[pid] = widget;
      }
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), validText(process.name).c_str());
      const auto description = "PID " + std::to_string(pid) + " · " + bytes(process.resident) + " · " +
        (usage.contains(pid) ? percentage(usage.at(pid)) + " CPU" : "Sampling CPU…");
      adw_action_row_set_subtitle(widget, description.c_str());
    }
    gtk_list_box_invalidate_sort(processes); gtk_list_box_invalidate_filter(processes);
    const auto description = latest.processesAvailable ? "Select a process for details. Restricted or exited processes may be omitted." :
      "Process information is unavailable. Check access to /proc.";
    adw_preferences_group_set_description(processGroup, description);
    unsigned visible = 0;
    for (const auto& [pid, widget] : rows) {
      (void)widget;
      const auto& process = previous.at(pid);
      auto* folded = g_utf8_casefold(validText(process.name).c_str(), -1);
      const bool match = query.empty() || std::string(folded).find(query) != std::string::npos || std::to_string(pid).find(query) != std::string::npos;
      g_free(folded); visible += match;
    }
    gtk_widget_set_visible(processEmpty, visible == 0);
    adw_preferences_group_set_title(processGroup, ("Processes · " + std::to_string(previous.size())).c_str());
  }
  void accept(Snapshot snapshot) {
    const double seconds = std::chrono::duration<double>(snapshot.sampled - latest.sampled).count();
    const auto cpu = snapshot.cpu && latest.cpu ? cpuUsage(*latest.cpu, *snapshot.cpu) : std::nullopt;
    adw_action_row_set_subtitle(cpuRow, cpu ? percentage(*cpu).c_str() : snapshot.cpu ? "Waiting for next sample" : "Unavailable");
    gtk_level_bar_set_value(cpuBar, cpu.value_or(0));
    if (snapshot.memory) {
      const auto& memory = *snapshot.memory; const auto used = memory.total - memory.available;
      adw_action_row_set_subtitle(memoryRow, (bytes(used) + " of " + bytes(memory.total)).c_str());
      gtk_level_bar_set_value(memoryBar, 100.0 * double(used) / double(memory.total));
      adw_action_row_set_subtitle(swapRow, memory.swapTotal ? (bytes(memory.swapTotal - memory.swapFree) + " of " + bytes(memory.swapTotal)).c_str() : "No swap configured");
    } else {
      adw_action_row_set_subtitle(memoryRow, "Unavailable"); adw_action_row_set_subtitle(swapRow, "Unavailable"); gtk_level_bar_set_value(memoryBar, 0);
    }
    std::string network = "Waiting for next sample";
    if (!snapshot.received || !snapshot.transmitted) network = "Unavailable";
    else if (latest.received && latest.transmitted && seconds > 0 && *snapshot.received >= *latest.received && *snapshot.transmitted >= *latest.transmitted)
      network = "Receive " + bytes(double(*snapshot.received - *latest.received) / seconds) + "/s · Send " + bytes(double(*snapshot.transmitted - *latest.transmitted) / seconds) + "/s";
    adw_action_row_set_subtitle(networkRow, network.c_str());
    if (snapshot.uptime) {
      const auto minutes = static_cast<std::uint64_t>(*snapshot.uptime / 60);
      adw_action_row_set_subtitle(uptimeRow, (std::to_string(minutes / 60) + " hours, " + std::to_string(minutes % 60) + " minutes").c_str());
    } else adw_action_row_set_subtitle(uptimeRow, "Unavailable");
    struct statvfs disk{};
    if (statvfs("/", &disk) == 0 && disk.f_blocks) {
      const double total = double(disk.f_blocks) * double(disk.f_frsize), free = double(disk.f_bfree) * double(disk.f_frsize);
      adw_action_row_set_subtitle(diskRow, (bytes(std::max(0.0, total - free)) + " of " + bytes(total)).c_str());
    } else adw_action_row_set_subtitle(diskRow, "Unavailable");
    usage.clear(); std::map<int, Process> next;
    for (auto& process : snapshot.processes) {
      if (previous.contains(process.pid)) if (auto value = processUsage(previous.at(process.pid), process, seconds, sysconf(_SC_CLK_TCK))) usage[process.pid] = *value;
      next[process.pid] = process;
    }
    previous = std::move(next); latest = std::move(snapshot); showProcesses();
    const auto memory = latest.memory ? std::optional<double>(100.0 * double(latest.memory->total - latest.memory->available) / double(latest.memory->total)) : std::nullopt;
    history.push_back({latest.sampled, cpu, memory});
    while (!history.empty() && latest.sampled - history.front().time > std::chrono::seconds(120)) history.pop_front();
    gtk_widget_queue_draw(GTK_WIDGET(cpuChart)); gtk_widget_queue_draw(GTK_WIDGET(memoryChart));
    gtk_accessible_update_property(GTK_ACCESSIBLE(cpuChart), GTK_ACCESSIBLE_PROPERTY_LABEL,
      ("CPU usage over the last two minutes. Current " + (cpu ? percentage(*cpu) : "unavailable")).c_str(), -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(memoryChart), GTK_ACCESSIBLE_PROPERTY_LABEL,
      ("Memory usage over the last two minutes. Current " + (memory ? percentage(*memory) : "unavailable")).c_str(), -1);
    gtk_label_set_text(status, "Live information from this device. Updates every 2 seconds.");
  }
  void refresh() {
    if (sampling || !gtk_switch_get_active(live)) return;
    sampling = true;
    auto* task = g_task_new(app, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); self.sampling = false;
      auto sample = std::unique_ptr<Sample>(static_cast<Sample*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
      if (sample && gtk_switch_get_active(self.live)) {
        self.accept(std::move(sample->resources));
        if (sample->reports) self.acceptReports(std::move(*sample->reports));
      }
    }, this);
    if (page == "screen-time" && std::chrono::steady_clock::now() - reportsRead > std::chrono::seconds(10)) {
      reportsRead = std::chrono::steady_clock::now();
      g_task_set_task_data(task, new std::filesystem::path(reportPath()), +[](gpointer data) { delete static_cast<std::filesystem::path*>(data); });
    }
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer, GCancellable*) {
      auto* sample = new Sample{readSnapshot(), {}};
      if (auto* path = static_cast<std::filesystem::path*>(g_task_get_task_data(task))) sample->reports = readReports(*path);
      g_task_return_pointer(task, sample, +[](gpointer data) { delete static_cast<Sample*>(data); });
    });
    g_object_unref(task);
  }
  struct Sample { Snapshot resources; std::optional<Reports> reports; };
  void create() {
    window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_title(GTK_WINDOW(window), "Helix System Monitor"); gtk_window_set_default_size(GTK_WINDOW(window), 960, 720);
    gtk_window_set_icon_name(GTK_WINDOW(window), "org.helix.SystemMonitor");
    split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    auto* breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
    GValue collapsed = G_VALUE_INIT; g_value_init(&collapsed, G_TYPE_BOOLEAN); g_value_set_boolean(&collapsed, true);
    adw_breakpoint_add_setter(breakpoint, G_OBJECT(split), "collapsed", &collapsed); g_value_unset(&collapsed);
    adw_application_window_add_breakpoint(window, breakpoint);
    auto* navigation = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(navigation), adw_header_bar_new());
    sidebar = GTK_LIST_BOX(gtk_list_box_new()); gtk_widget_add_css_class(GTK_WIDGET(sidebar), "navigation-sidebar");
    for (const auto& id : {"overview", "processes", "screen-time"}) {
      auto* widget = gtk_list_box_row_new(); auto* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name(std::string(id) == "overview" ? "view-grid-symbolic" : std::string(id) == "screen-time" ? "document-open-recent-symbolic" : "view-list-symbolic"));
      gtk_box_append(GTK_BOX(content), gtk_label_new(std::string(id) == "overview" ? "Overview" : std::string(id) == "processes" ? "Processes" : "Screen Time"));
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(widget), content); gtk_list_box_append(sidebar, widget);
      if (std::string(id) == "overview") overviewRow = GTK_LIST_BOX_ROW(widget);
      else if (std::string(id) == "processes") processesRow = GTK_LIST_BOX_ROW(widget); else reportsRow = GTK_LIST_BOX_ROW(widget);
    }
    g_signal_connect(sidebar, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); self.select(row == self.overviewRow ? "overview" : row == self.processesRow ? "processes" : "screen-time");
    }), this);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(navigation), scrolled(GTK_WIDGET(sidebar)));
    adw_navigation_split_view_set_sidebar(split, adw_navigation_page_new(navigation, "System Monitor"));
    auto* view = adw_toolbar_view_new(); auto* header = adw_header_bar_new();
    live = GTK_SWITCH(gtk_switch_new()); gtk_switch_set_active(live, true);
    gtk_widget_set_valign(GTK_WIDGET(live), GTK_ALIGN_CENTER); gtk_widget_set_tooltip_text(GTK_WIDGET(live), "Live updates");
    gtk_accessible_update_property(GTK_ACCESSIBLE(live), GTK_ACCESSIBLE_PROPERTY_LABEL, "Live updates", -1);
    auto* liveControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(liveControl), gtk_label_new("Live")); gtk_box_append(GTK_BOX(liveControl), GTK_WIDGET(live));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), liveControl);
    g_signal_connect(live, "notify::active", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data);
      if (gtk_switch_get_active(self.live)) { self.reportsRead = {}; self.refresh(); }
      else if (self.status) gtk_label_set_text(self.status, "Live updates are paused.");
    }), this);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    stack = GTK_STACK(gtk_stack_new());
    auto* overview = box(); auto* resources = group(overview, "Resources");
    cpuRow = row(resources, "CPU", "Waiting for first sample"); cpuBar = level(cpuRow);
    memoryRow = row(resources, "Memory"); memoryBar = level(memoryRow);
    swapRow = row(resources, "Swap"); networkRow = row(resources, "Network", "Excludes loopback traffic");
    auto* charts = group(overview, "Recent Activity");
    adw_preferences_group_add(charts, historyChart("CPU", false));
    adw_preferences_group_add(charts, historyChart("Memory", true));
    auto* system = group(overview, "System"); uptimeRow = row(system, "Uptime"); diskRow = row(system, "System disk", "Root filesystem");
    status = GTK_LABEL(gtk_label_new("Loading local resource information…")); gtk_label_set_wrap(status, true);
    gtk_widget_add_css_class(GTK_WIDGET(status), "dim-label"); gtk_box_append(overview, GTK_WIDGET(status));
    auto* clamp = adw_clamp_new(); adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760); adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(overview));
    gtk_stack_add_named(stack, scrolled(clamp), "overview");
    auto* processBox = box(); auto* search = gtk_search_entry_new(); gtk_widget_set_tooltip_text(search, "Search by process name or PID");
    searchEntry = GTK_SEARCH_ENTRY(search);
    gtk_accessible_update_property(GTK_ACCESSIBLE(search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search by process name or PID", -1);
    gtk_box_append(processBox, search);
    g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry* search, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); auto* folded = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(search)), -1);
      self.query = folded; g_free(folded); self.showProcesses();
    }), this);
    const char* labels[] = {"Memory usage", "CPU usage", "Name", nullptr}; auto* sort = gtk_drop_down_new_from_strings(labels);
    gtk_accessible_update_property(GTK_ACCESSIBLE(sort), GTK_ACCESSIBLE_PROPERTY_LABEL, "Sort processes", -1);
    gtk_box_append(processBox, sort);
    g_signal_connect(sort, "notify::selected", G_CALLBACK(+[](GObject* widget, GParamSpec*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); self.sort = gtk_drop_down_get_selected(GTK_DROP_DOWN(widget)); gtk_list_box_invalidate_sort(self.processes);
    }), this);
    processGroup = group(processBox, "Processes");
    processes = GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(processes, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(processes), "boxed-list"); adw_preferences_group_add(processGroup, GTK_WIDGET(processes));
    gtk_list_box_set_sort_func(processes, +[](GtkListBoxRow* first, GtkListBoxRow* second, gpointer data) -> int {
      auto& self = *static_cast<Monitor*>(data); const int a = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(first), "pid")), b = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(second), "pid"));
      if (!self.previous.contains(a) || !self.previous.contains(b)) return a < b ? -1 : a > b ? 1 : 0;
      const auto& left = self.previous.at(a); const auto& right = self.previous.at(b);
      if (self.sort == 0 && left.resident != right.resident) return left.resident > right.resident ? -1 : 1;
      if (self.sort == 1) {
        const double l = self.usage.contains(a) ? self.usage.at(a) : 0, r = self.usage.contains(b) ? self.usage.at(b) : 0;
        if (l != r) return l > r ? -1 : 1;
      }
      const auto compare = left.name.compare(right.name); return compare ? compare : a < b ? -1 : a > b ? 1 : 0;
    }, this, nullptr);
    gtk_list_box_set_filter_func(processes, +[](GtkListBoxRow* row, gpointer data) -> gboolean {
      auto& self = *static_cast<Monitor*>(data); const int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pid"));
      if (!self.previous.contains(pid)) return false;
      auto* name = g_utf8_casefold(validText(self.previous.at(pid).name).c_str(), -1);
      const bool match = self.query.empty() || std::string(name).find(self.query) != std::string::npos || std::to_string(pid).find(self.query) != std::string::npos;
      g_free(name); return match;
    }, this, nullptr);
    g_signal_connect(processes, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
      static_cast<Monitor*>(data)->details(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pid")));
    }), this);
    processEmpty = gtk_label_new("No matching processes"); gtk_label_set_wrap(GTK_LABEL(processEmpty), true); gtk_box_append(processBox, processEmpty);
    auto* processClamp = adw_clamp_new(); adw_clamp_set_maximum_size(ADW_CLAMP(processClamp), 760); adw_clamp_set_child(ADW_CLAMP(processClamp), GTK_WIDGET(processBox));
    gtk_stack_add_named(stack, scrolled(processClamp), "processes");
    auto* reportsBox = box(); reportSummary = group(reportsBox, "Saved Screen Time");
    reportDate = ADW_COMBO_ROW(adw_combo_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(reportDate), "Day");
    adw_preferences_group_add(reportSummary, GTK_WIDGET(reportDate));
    reportTotal = row(reportSummary, "Total recorded time", "Loading saved reports…");
    hourChart = GTK_DRAWING_AREA(gtk_drawing_area_new()); gtk_drawing_area_set_content_height(hourChart, 150);
    gtk_widget_set_hexpand(GTK_WIDGET(hourChart), true); gtk_widget_add_css_class(GTK_WIDGET(hourChart), "accent");
    gtk_accessible_update_property(GTK_ACCESSIBLE(hourChart), GTK_ACCESSIBLE_PROPERTY_LABEL, "Hourly screen-time chart", -1);
    gtk_drawing_area_set_draw_func(hourChart, +[](GtkDrawingArea* area, cairo_t* context, int width, int height, gpointer data) {
      const auto& self = *static_cast<Monitor*>(data); GdkRGBA color; gtk_widget_get_color(GTK_WIDGET(area), &color);
      const auto maximum = std::max<std::uint64_t>(1, *std::ranges::max_element(self.hourValues));
      const double step = double(width) / 24;
      cairo_set_source_rgba(context, color.red, color.green, color.blue, .2);
      cairo_rectangle(context, 0, height - 1, width, 1); cairo_fill(context);
      cairo_set_source_rgba(context, color.red, color.green, color.blue, 1);
      for (unsigned hour = 0; hour < 24; ++hour) {
        const double barHeight = double(height - 8) * double(self.hourValues[hour]) / double(maximum);
        cairo_rectangle(context, hour * step + 1, height - barHeight - 2, std::max(1.0, step - 2), barHeight); cairo_fill(context);
      }
    }, this, nullptr);
    adw_preferences_group_add(reportSummary, GTK_WIDGET(hourChart));
    auto* hourCaption = gtk_label_new("00:00 → 23:00 · Hourly activity"); gtk_label_set_xalign(GTK_LABEL(hourCaption), 0);
    gtk_label_set_wrap(GTK_LABEL(hourCaption), true);
    gtk_widget_add_css_class(hourCaption, "dim-label"); adw_preferences_group_add(reportSummary, hourCaption);
    reportApps = group(reportsBox, "Application Usage"); reportHours = group(reportsBox, "Hourly Usage", "Hours use the recorder’s local time.");
    hourDetails = ADW_EXPANDER_ROW(adw_expander_row_new()); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(hourDetails), false);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hourDetails), "Hourly details");
    adw_expander_row_set_subtitle(hourDetails, "Show exact durations for each hour");
    adw_preferences_group_add(reportHours, GTK_WIDGET(hourDetails));
    g_signal_connect(reportDate, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); if (self.selectingDate) return;
      const auto selected = adw_combo_row_get_selected(self.reportDate);
      if (selected < self.reportDates.size()) { self.selectedDate = self.reportDates[selected]; self.showReportDay(); }
    }), this);
    auto* reportsClamp = adw_clamp_new(); adw_clamp_set_maximum_size(ADW_CLAMP(reportsClamp), 760); adw_clamp_set_child(ADW_CLAMP(reportsClamp), GTK_WIDGET(reportsBox));
    gtk_stack_add_named(stack, scrolled(reportsClamp), "screen-time");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(stack));
    content = adw_navigation_page_new(view, "Overview"); adw_navigation_split_view_set_content(split, content);
    adw_application_window_set_content(window, GTK_WIDGET(split)); select(page);
    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(window)), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    appearanceCancel = g_cancellable_new(); bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) {
      appearanceChanged = g_dbus_connection_signal_subscribe(bus, helix::busName, helix::appearanceInterface, "Changed", helix::objectPath,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
          static_cast<Monitor*>(data)->refreshAppearance();
        }, this, nullptr);
      ownerChanged = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus",
        helix::busName, G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
          static_cast<Monitor*>(data)->refreshAppearance();
        }, this, nullptr);
    }
    g_signal_connect(adw_style_manager_get_default(), "notify::high-contrast", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data); helix::apps::applyAppearance(self.css, self.appearance);
    }), this);
    refreshAppearance();
    timer = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean { static_cast<Monitor*>(data)->refresh(); return G_SOURCE_CONTINUE; }, this);
    refresh();
  }
};
}
int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--snapshot") {
    const auto snapshot = readSnapshot();
    const Json output = {{"cpu-available", snapshot.cpu.has_value()}, {"memory-available", snapshot.memory.has_value()},
      {"processes-available", snapshot.processesAvailable}, {"process-count", snapshot.processes.size()},
      {"memory-total", snapshot.memory ? snapshot.memory->total : 0}, {"uptime", snapshot.uptime.value_or(0)}};
    std::puts(output.dump().c_str()); return 0;
  }
  auto* app = adw_application_new("org.helix.SystemMonitor", G_APPLICATION_HANDLES_COMMAND_LINE);
  Monitor monitor(app);
  g_application_add_main_option(G_APPLICATION(app), "page", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Open overview, processes or screen-time", "ID");
  g_application_add_main_option(G_APPLICATION(app), "search", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Search process names or PIDs", "QUERY");
  g_application_add_main_option(G_APPLICATION(app), "inspect-layout", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Print the current layout and sampling state", nullptr);
  g_application_add_main_option(G_APPLICATION(app), "save-preview", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Save a PNG of the running window", "FILE");
  g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) {
    auto& self = *static_cast<Monitor*>(data); if (!self.window) self.create(); gtk_window_present(GTK_WINDOW(self.window));
  }), &monitor);
  g_signal_connect(app, "command-line", G_CALLBACK((+[](GApplication* app, GApplicationCommandLine* command, gpointer data) -> int {
    auto& self = *static_cast<Monitor*>(data); const gchar *page = nullptr, *search = nullptr, *preview = nullptr; gboolean inspect = false;
    auto* options = g_application_command_line_get_options_dict(command);
    g_variant_dict_lookup(options, "save-preview", "&s", &preview);
    if (preview) {
      if (self.window && helix::apps::savePreview(GTK_WIDGET(self.window), preview)) return 0;
      g_application_command_line_printerr(command, "Open System Monitor before saving a preview\n"); return 1;
    }
    g_variant_dict_lookup(options, "page", "&s", &page); g_variant_dict_lookup(options, "inspect-layout", "b", &inspect);
    g_variant_dict_lookup(options, "search", "&s", &search);
    if (inspect && self.window) {
      const Json state = {{"page", self.page}, {"width", gtk_widget_get_width(GTK_WIDGET(self.window))},
        {"height", gtk_widget_get_height(GTK_WIDGET(self.window))}, {"collapsed", bool(adw_navigation_split_view_get_collapsed(self.split))},
        {"cpu-available", self.latest.cpu.has_value()}, {"memory-available", self.latest.memory.has_value()},
        {"process-count", self.previous.size()}, {"live", bool(gtk_switch_get_active(self.live))},
        {"appearance-available", self.appearance.value("available", false)}, {"query", self.query},
        {"report-days", self.reports.days.size()}, {"report-date", self.selectedDate}};
      g_application_command_line_print(command, "%s\n", state.dump().c_str()); return 0;
    }
    if (page && std::string(page) != "overview" && std::string(page) != "processes" && std::string(page) != "screen-time") {
      g_application_command_line_printerr(command, "Unknown System Monitor page\n"); return 2;
    }
    if (page) self.page = page;
    if (search) self.page = "processes";
    g_application_activate(app); self.select(self.page);
    if (search) gtk_editable_set_text(GTK_EDITABLE(self.searchEntry), search);
    return 0;
  })), &monitor);
  const int result = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app); return result;
}
