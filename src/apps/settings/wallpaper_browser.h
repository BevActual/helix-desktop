#pragma once
#include "apps/common/image_preview.h"
#include "apps/settings/wallpaper_catalog.h"
#include "desktop/client.h"
#include <adwaita.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>

namespace helix::settings {
class WallpaperBrowser : public std::enable_shared_from_this<WallpaperBrowser> {
  using Callback = std::function<void()>;
  DesktopClient client;
  AdwDialog* dialog = nullptr;
  GtkWindow* parent;
  GtkFlowBox* grid = nullptr;
  GtkSearchEntry* search = nullptr;
  GtkDropDown* sourceSelector = nullptr;
  GtkWidget *localControls = nullptr, *remoteControls = nullptr, *searchButton = nullptr, *apply = nullptr;
  GtkWidget *previous = nullptr, *next = nullptr, *cancelButton = nullptr;
  GtkLabel *status = nullptr, *selection = nullptr, *pager = nullptr, *folderLabel = nullptr, *keyLabel = nullptr;
  AdwComboRow *sort = nullptr, *ratio = nullptr, *order = nullptr, *resolution = nullptr;
  AdwSpinRow *width = nullptr, *height = nullptr;
  AdwSwitchRow *general = nullptr, *anime = nullptr, *people = nullptr, *sfw = nullptr, *sketchy = nullptr, *nsfw = nullptr;
  bool remote = false, recursive = false, closed = false, available = true, online = true, busy = false, polling = false, applyingLocal = false;
  unsigned generation = 0, page = 0, totalPages = 1;
  guint timer = 0;
  GCancellable* scan = nullptr;
  std::uint64_t searchJob = 0, downloadJob = 0;
  std::string folder, selected, current, remoteFingerprint;
  std::vector<std::filesystem::path> localFiles, filtered;
  std::map<std::string, GtkWidget*> tiles;
  std::map<std::string, GtkBox*> imageBoxes;
  std::map<std::string, std::string> previews;
  Callback changed;
  static void signal(gpointer widget, const char* name, Callback action, bool notify = false) {
    auto* callback = new Callback(std::move(action));
    auto destroy = +[](gpointer p, GClosure*) { delete static_cast<Callback*>(p); };
    if (notify) g_signal_connect_data(widget, name, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) { (*static_cast<Callback*>(p))(); }), callback, destroy, G_CONNECT_DEFAULT);
    else g_signal_connect_data(widget, name, G_CALLBACK(+[](gpointer, gpointer p) { (*static_cast<Callback*>(p))(); }), callback, destroy, G_CONNECT_DEFAULT);
  }
  static GtkWidget* button(const char* text, Callback action) {
    auto* value = gtk_button_new_with_label(text); signal(value, "clicked", std::move(action)); return value;
  }
  static GtkWidget* label(const char* text) {
    auto* value = gtk_label_new(text); gtk_label_set_wrap(GTK_LABEL(value), true); gtk_label_set_xalign(GTK_LABEL(value), 0); return value;
  }
  static void margins(GtkWidget* widget, int value) {
    gtk_widget_set_margin_top(widget, value); gtk_widget_set_margin_bottom(widget, value);
    gtk_widget_set_margin_start(widget, value); gtk_widget_set_margin_end(widget, value);
  }
  static void rowTitle(AdwPreferencesRow* row, const char* text) { adw_preferences_row_set_use_markup(row, false); adw_preferences_row_set_title(row, text); }
  void message(const std::string& text) { gtk_label_set_text(status, text.c_str()); }
  void cancelJob(std::uint64_t& id) {
    if (id) client.callAsync("appearance", "CancelWallhaven", g_variant_new("(t)", id), [](Json, std::string) {});
    id = 0;
  }
  void cancelWork() {
    ++generation; if (scan) g_cancellable_cancel(scan);
    cancelJob(searchJob); cancelJob(downloadJob); busy = false;
  }
  void controls() {
    gtk_widget_set_sensitive(apply, available && !busy && !selected.empty() && (!remote || (online && searchJob)));
    gtk_widget_set_sensitive(searchButton, available && online && !busy);
    gtk_widget_set_sensitive(previous, !busy && page > 0);
    gtk_widget_set_sensitive(next, !busy && page + 1 < totalPages);
    gtk_widget_set_visible(cancelButton, busy && !applyingLocal);
    gtk_widget_set_sensitive(GTK_WIDGET(sourceSelector), !applyingLocal);
    gtk_widget_set_sensitive(localControls, !applyingLocal);
    gtk_button_set_label(GTK_BUTTON(apply), remote ? "Download & Apply" : "Apply Wallpaper");
    auto* name = g_filename_display_basename(selected.c_str());
    gtk_label_set_text(selection, selected.empty() ? "Choose an image to apply." :
      (remote ? "Selected: " + selected : "Selected: " + std::string(name)).c_str()); g_free(name);
    gtk_label_set_text(pager, (std::to_string(page + 1) + " / " + std::to_string(totalPages)).c_str());
  }
  void clearGrid() {
    tiles.clear(); imageBoxes.clear(); previews.clear(); selected.clear();
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(grid))) gtk_flow_box_remove(grid, child);
    selected.clear();
  }
  void addTile(const std::string& id, const std::string& name, const std::string& path) {
    auto* tile = gtk_toggle_button_new(); auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* image = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_size_request(image, 140, 100);
    auto* placeholder = gtk_image_new_from_icon_name("image-x-generic-symbolic"); gtk_image_set_pixel_size(GTK_IMAGE(placeholder), 48);
    gtk_widget_set_vexpand(placeholder, true); gtk_box_append(GTK_BOX(image), placeholder);
    gtk_box_append(GTK_BOX(box), image);
    auto* caption = gtk_label_new(name.c_str()); gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(caption), 18); gtk_box_append(GTK_BOX(box), caption);
    gtk_button_set_child(GTK_BUTTON(tile), box); gtk_widget_set_tooltip_text(tile, name.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(tile), GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
    if (!tiles.empty()) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(tile), GTK_TOGGLE_BUTTON(tiles.begin()->second));
    tiles[id] = tile; imageBoxes[id] = GTK_BOX(image);
    auto self = shared_from_this(); signal(tile, "toggled", [self, id, tile] {
      if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tile))) { self->selected = id; self->controls(); }
    });
    gtk_flow_box_append(grid, tile);
    if (!path.empty()) setPreview(id, path);
    if (id == current) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tile), true);
  }
  void setPreview(const std::string& id, const std::string& path) {
    if (path.empty() || previews[id] == path || !imageBoxes.contains(id)) return;
    previews[id] = path; auto* box = imageBoxes.at(id);
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(box))) gtk_box_remove(box, child);
    auto* preview = apps::imagePreview(path, "Wallpaper thumbnail");
    gtk_widget_set_size_request(gtk_widget_get_first_child(preview), -1, 100);
    gtk_box_append(box, preview);
  }
  void localPage() {
    clearGrid(); filtered.clear();
    auto* needle = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(search)), -1);
    for (const auto& path : localFiles) {
      auto* display = g_filename_display_basename(path.c_str()); auto* name = g_utf8_casefold(display, -1);
      if (std::string(name).find(needle) != std::string::npos) filtered.push_back(path);
      g_free(name); g_free(display);
    }
    g_free(needle); totalPages = std::max(1u, unsigned((filtered.size() + 23) / 24)); page = std::min(page, totalPages - 1);
    for (std::size_t i = page * 24; i < std::min(filtered.size(), std::size_t(page + 1) * 24); ++i) {
      auto* name = g_filename_display_basename(filtered[i].c_str()); addTile(filtered[i].string(), name, filtered[i].string()); g_free(name);
    }
    if (filtered.empty() && !localFiles.empty()) message("No images match this search.");
    controls();
  }
  void loadFolder() {
    cancelWork(); if (scan) g_object_unref(scan); scan = g_cancellable_new();
    busy = true; localFiles.clear(); clearGrid(); page = 0; totalPages = 1;
    auto* display = g_filename_display_name(folder.c_str()); gtk_label_set_text(folderLabel, display); g_free(display);
    message("Looking for images…"); controls();
    struct Work { std::shared_ptr<WallpaperBrowser> self; unsigned generation; std::string folder; bool recursive; };
    auto* work = new Work{shared_from_this(), generation, folder, recursive};
    auto* task = g_task_new(nullptr, scan, +[](GObject*, GAsyncResult* result, gpointer) {
      auto* task = G_TASK(result); auto& work = *static_cast<Work*>(g_task_get_task_data(task)); auto& self = *work.self;
      std::unique_ptr<WallpaperCatalog> catalog(static_cast<WallpaperCatalog*>(g_task_propagate_pointer(task, nullptr)));
      if (!catalog || self.closed || self.generation != work.generation) return;
      self.busy = false; self.localFiles = std::move(catalog->files);
      self.message(catalog->unavailable ? "This folder could not be fully read. Choose another folder or retry." : catalog->truncated ?
        "This folder is large. Showing up to 1,000 images; choose a smaller folder to see more." : self.localFiles.empty() ?
        "No supported images in this folder. Choose another folder or include subfolders." : "Choose an image, then apply it to your desktop.");
      self.localPage();
    }, nullptr);
    g_task_set_task_data(task, work, +[](gpointer p) { delete static_cast<Work*>(p); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer p, GCancellable* cancel) {
      auto& work = *static_cast<Work*>(p);
      auto* result = new WallpaperCatalog(scanWallpapers(work.folder, work.recursive, [cancel] { return g_cancellable_is_cancelled(cancel); }));
      g_task_return_pointer(task, result, +[](gpointer p) { delete static_cast<WallpaperCatalog*>(p); });
    }); g_object_unref(task);
  }
  void chooseFolder() {
    auto* chooser = gtk_file_dialog_new(); gtk_file_dialog_set_title(chooser, "Wallpaper Folder");
    gtk_file_dialog_select_folder(chooser, parent, nullptr, +[](GObject* source, GAsyncResult* result, gpointer p) {
      std::unique_ptr<std::shared_ptr<WallpaperBrowser>> keep(static_cast<std::shared_ptr<WallpaperBrowser>*>(p)); auto& self = **keep;
      GError* error = nullptr; auto* file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
      if (file) { auto* path = g_file_get_path(file); if (!self.closed && path) { self.folder = path; self.loadFolder(); } g_free(path); g_object_unref(file); }
      if (error) g_error_free(error);
    }, new std::shared_ptr<WallpaperBrowser>(shared_from_this())); g_object_unref(chooser);
  }
  void searchRemote() {
    cancelWork(); clearGrid(); busy = true; remoteFingerprint.clear(); message("Searching Wallhaven…"); controls();
    Json query = {{"query", gtk_editable_get_text(GTK_EDITABLE(search))}, {"page", page + 1},
      {"sort", adw_combo_row_get_selected(sort)}, {"ratio", adw_combo_row_get_selected(ratio)}, {"ascending", adw_combo_row_get_selected(order) == 1},
      {"width", int(adw_spin_row_get_value(width))}, {"height", int(adw_spin_row_get_value(height))}, {"exact", adw_combo_row_get_selected(resolution) == 1}};
    for (const auto& [name, row] : std::vector<std::pair<const char*, AdwSwitchRow*>>{{"general", general}, {"anime", anime}, {"people", people}, {"sfw", sfw}, {"sketchy", sketchy}, {"nsfw", nsfw}})
      query[name] = bool(adw_switch_row_get_active(row));
    auto self = shared_from_this(); const auto version = generation;
    client.callAsync("appearance", "SearchWallhaven", g_variant_new("(@a{sv})", toVariant(query)), [self, version](Json value, std::string error) {
      if (self->closed || self->generation != version) { if (value.is_number()) { auto id = value.get<std::uint64_t>(); self->cancelJob(id); } return; }
      if (!error.empty() || !value.is_number()) { self->busy = false; self->message("Search could not start. Check your connection, filters and API key, then retry."); self->controls(); return; }
      self->searchJob = value.get<std::uint64_t>(); self->pollRemote();
    });
  }
  void pollRemote() {
    if (closed || polling || (!searchJob && !downloadJob)) return;
    const auto id = downloadJob ? downloadJob : searchJob; const bool downloading = downloadJob != 0;
    const auto version = generation; polling = true; auto self = shared_from_this();
    client.callAsync("appearance", "GetWallhaven", g_variant_new("(t)", id), [self, id, downloading, version](Json value, std::string error) {
      self->polling = false; if (self->closed || self->generation != version) return;
      if (!error.empty()) { self->cancelWork(); self->message("Wallhaven disconnected. Search again to reconnect."); self->controls(); return; }
      self->busy = value.value("busy", false);
      if (!value.value("error", "").empty()) { self->message(value.at("error")); self->cancelJob(downloading ? self->downloadJob : self->searchJob); self->controls(); return; }
      if (downloading) {
        if (!self->busy) { self->cancelJob(self->downloadJob); self->message("Wallpaper downloaded and applied."); self->changed(); }
      } else if (!self->busy) {
        const auto items = value.value("items", Json::array()); Json keys = Json::array(); for (const auto& item : items) keys.push_back(item.at("id"));
        if (keys.dump() != self->remoteFingerprint) {
          self->remoteFingerprint = keys.dump(); self->clearGrid(); self->totalPages = value.value("pages", 1u);
          for (const auto& item : items) self->addTile(item.at("id"), item.at("id").get<std::string>() + " · " + item.value("resolution", ""), item.value("thumbnail", ""));
          self->message(items.empty() ? "No images found. Adjust the search or filters and try again." : "Choose an image to download and apply. Images are provided by Wallhaven.");
        } else for (const auto& item : items) self->setPreview(item.at("id"), item.value("thumbnail", ""));
      }
      self->controls();
    });
  }
  void applySelected() {
    if (!available || busy || selected.empty()) return;
    busy = true; controls(); auto self = shared_from_this(); const auto version = generation;
    if (!remote) {
      applyingLocal = true; controls();
      client.callAsync("appearance", "SetWallpaper", g_variant_new("(s)", selected.c_str()), [self](Json, std::string error) {
        if (self->closed) return;
        self->busy = false;
        self->applyingLocal = false;
        self->message(error.empty() ? "Wallpaper applied." : "Wallpaper could not be applied. Check that the file is still available."); self->controls(); self->changed();
      });
    } else {
      message("Downloading wallpaper…");
      client.callAsync("appearance", "DownloadWallhaven", g_variant_new("(ts)", searchJob, selected.c_str()), [self, version](Json value, std::string error) {
        if (self->closed || self->generation != version) { if (value.is_number()) { auto id = value.get<std::uint64_t>(); self->cancelJob(id); } return; }
        if (!error.empty() || !value.is_number()) { self->busy = false; self->message("Download could not start. Search again and retry."); self->controls(); return; }
        self->downloadJob = value.get<std::uint64_t>(); self->pollRemote();
      });
    }
  }
  void editKey() {
    auto* prompt = adw_alert_dialog_new("Wallhaven API Key", "Enter a new key from your Wallhaven account. It replaces the key in your Helix configuration; existing keys are never displayed.");
    auto* entry = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), true);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Wallhaven API key", -1);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(prompt), entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(prompt), "cancel", "Cancel", "save", "Save", nullptr);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(prompt), "cancel"); adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(prompt), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(prompt), "save", false);
    signal(entry, "changed", [entry, prompt] { adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(prompt), "save", *gtk_editable_get_text(GTK_EDITABLE(entry))); });
    struct Key { std::shared_ptr<WallpaperBrowser> self; GtkWidget* entry; };
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(prompt), GTK_WIDGET(dialog), nullptr, +[](GObject* source, GAsyncResult* result, gpointer p) {
      std::unique_ptr<Key> key(static_cast<Key*>(p)); const auto* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
      if (key->self->closed || g_strcmp0(response, "save")) return;
      auto self = key->self;
      self->client.callAsync("appearance", "SetWallhavenApiKey", g_variant_new("(s)", gtk_editable_get_text(GTK_EDITABLE(key->entry))), [self](Json, std::string error) {
        if (self->closed) return;
        self->message(error.empty() ? "Wallhaven API key saved." : "The API key could not be saved. Check the key and connection, then retry."); self->changed();
      });
      gtk_editable_set_text(GTK_EDITABLE(key->entry), "");
    }, new Key{shared_from_this(), entry});
  }
  void source(bool wallhaven) {
    cancelWork(); remote = wallhaven; page = 0; totalPages = 1; clearGrid(); gtk_editable_set_text(GTK_EDITABLE(search), "");
    gtk_widget_set_visible(localControls, !remote); gtk_widget_set_visible(remoteControls, remote); gtk_widget_set_visible(searchButton, remote);
    if (remote) { message(online ? "Enter a search or browse the latest Wallhaven images." : "Wallhaven is unavailable while shell network requests are restricted."); controls(); }
    else loadFolder();
  }
  void build(bool wallhaven, const Json& appearance) {
    auto self = shared_from_this(); dialog = adw_dialog_new(); adw_dialog_set_title(dialog, "Wallpapers");
    adw_dialog_set_content_width(dialog, 820); adw_dialog_set_content_height(dialog, 720);
    g_object_set_data_full(G_OBJECT(dialog), "browser", new std::shared_ptr<WallpaperBrowser>(self), +[](gpointer p) { delete static_cast<std::shared_ptr<WallpaperBrowser>*>(p); });
    signal(dialog, "closed", [self] { self->closed = true; self->cancelWork(); if (self->timer) { g_source_remove(self->timer); self->timer = 0; } });
    auto* toolbar = adw_toolbar_view_new(); auto* header = adw_header_bar_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    const char* choices[] = {"My Images", "Wallhaven", nullptr}; auto* sources = gtk_drop_down_new_from_strings(choices);
    sourceSelector = GTK_DROP_DOWN(sources);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(sources), wallhaven ? 1 : 0); adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), sources);
    gtk_accessible_update_property(GTK_ACCESSIBLE(sources), GTK_ACCESSIBLE_PROPERTY_LABEL, "Wallpaper source", -1);
    signal(sources, "notify::selected", [self, sources] { self->source(gtk_drop_down_get_selected(GTK_DROP_DOWN(sources)) == 1); }, true);
    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); margins(content, 12);
    auto* scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content); adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    localControls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); folderLabel = GTK_LABEL(label(folder.c_str()));
    gtk_label_set_ellipsize(folderLabel, PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_wrap(folderLabel, false); gtk_widget_set_hexpand(GTK_WIDGET(folderLabel), true);
    gtk_box_append(GTK_BOX(localControls), GTK_WIDGET(folderLabel)); auto* localActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(localActions), button("Choose Folder", [self] { self->chooseFolder(); }));
    gtk_box_append(GTK_BOX(localActions), button("Refresh", [self] { self->loadFolder(); })); gtk_box_append(GTK_BOX(localControls), localActions);
    auto* subfolders = gtk_check_button_new_with_label("Include subfolders"); gtk_box_append(GTK_BOX(localControls), subfolders);
    signal(subfolders, "toggled", [self, subfolders] { self->recursive = gtk_check_button_get_active(GTK_CHECK_BUTTON(subfolders)); self->loadFolder(); });
    gtk_box_append(GTK_BOX(content), localControls);
    remoteControls = adw_preferences_group_new(); auto* filters = adw_expander_row_new(); rowTitle(ADW_PREFERENCES_ROW(filters), "Wallhaven Filters");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(remoteControls), filters);
    auto combo = [filters](const char* name, const char* const* options, unsigned selected = 0) {
      auto* row = adw_combo_row_new(); rowTitle(ADW_PREFERENCES_ROW(row), name); auto* model = gtk_string_list_new(options);
      adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model)); g_object_unref(model); adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
      adw_expander_row_add_row(ADW_EXPANDER_ROW(filters), row); return ADW_COMBO_ROW(row);
    };
    const char* sorts[] = {"Relevance", "Random", "Date Added", "Views", "Favorites", "Top List", "Hot", nullptr}; sort = combo("Sort", sorts, 2);
    const char* orders[] = {"Descending", "Ascending", nullptr}; order = combo("Order", orders);
    const char* ratios[] = {"Any", "Landscape", "Portrait", "16:9", "16:10", "21:9", "32:9", "48:9", "9:16", "10:16", "9:18", "1:1", "3:2", "4:3", "5:4", nullptr}; ratio = combo("Aspect Ratio", ratios);
    const char* resolutions[] = {"At Least", "Exactly", nullptr}; resolution = combo("Resolution Match", resolutions);
    auto spin = [filters](const char* name) { auto* row = adw_spin_row_new_with_range(0, 32768, 1); rowTitle(ADW_PREFERENCES_ROW(row), name); adw_expander_row_add_row(ADW_EXPANDER_ROW(filters), row); return ADW_SPIN_ROW(row); };
    width = spin("Width (0 for any)"); height = spin("Height (0 for any)");
    auto toggle = [filters](const char* name, bool active) { auto* row = adw_switch_row_new(); rowTitle(ADW_PREFERENCES_ROW(row), name); adw_switch_row_set_active(ADW_SWITCH_ROW(row), active); adw_expander_row_add_row(ADW_EXPANDER_ROW(filters), row); return ADW_SWITCH_ROW(row); };
    general = toggle("General", true); anime = toggle("Anime", true); people = toggle("People", true);
    sfw = toggle("Safe for Work", true); sketchy = toggle("Sketchy", false); nsfw = toggle("Adult Content (API key required)", false);
    gtk_box_append(GTK_BOX(content), remoteControls); keyLabel = GTK_LABEL(label("")); adw_preferences_group_add(ADW_PREFERENCES_GROUP(remoteControls), GTK_WIDGET(keyLabel));
    auto* keyButton = button("API Key", [self] { self->editKey(); }); gtk_widget_set_halign(keyButton, GTK_ALIGN_START); adw_preferences_group_add(ADW_PREFERENCES_GROUP(remoteControls), keyButton);
    auto* searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(search), true); gtk_widget_set_size_request(GTK_WIDGET(search), 0, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search wallpapers", -1);
    gtk_box_append(GTK_BOX(searchRow), GTK_WIDGET(search)); searchButton = button("Search", [self] { self->page = 0; self->searchRemote(); }); gtk_box_append(GTK_BOX(searchRow), searchButton);
    signal(search, "search-changed", [self] { if (!self->remote) { self->page = 0; self->localPage(); } });
    signal(search, "activate", [self] { if (self->remote && self->online && self->available && !self->busy) { self->page = 0; self->searchRemote(); } });
    gtk_box_append(GTK_BOX(content), searchRow); status = GTK_LABEL(label("")); gtk_box_append(GTK_BOX(content), GTK_WIDGET(status));
    grid = GTK_FLOW_BOX(gtk_flow_box_new()); gtk_flow_box_set_selection_mode(grid, GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(grid, 1); gtk_flow_box_set_max_children_per_line(grid, 4); gtk_flow_box_set_homogeneous(grid, true);
    gtk_flow_box_set_row_spacing(grid, 6); gtk_flow_box_set_column_spacing(grid, 6); gtk_box_append(GTK_BOX(content), GTK_WIDGET(grid));
    auto* pages = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_widget_set_halign(pages, GTK_ALIGN_CENTER);
    previous = button("Previous", [self] { if (self->page) --self->page; if (self->remote) self->searchRemote(); else self->localPage(); });
    next = button("Next", [self] { ++self->page; if (self->remote) self->searchRemote(); else self->localPage(); }); pager = GTK_LABEL(gtk_label_new("1 / 1"));
    gtk_box_append(GTK_BOX(pages), previous); gtk_box_append(GTK_BOX(pages), GTK_WIDGET(pager)); gtk_box_append(GTK_BOX(pages), next); gtk_box_append(GTK_BOX(content), pages);
    auto* bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); margins(bottom, 12);
    selection = GTK_LABEL(label("")); gtk_label_set_ellipsize(selection, PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_wrap(selection, false); gtk_box_append(GTK_BOX(bottom), GTK_WIDGET(selection));
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); cancelButton = button("Cancel", [self] { self->cancelWork(); self->message("Operation cancelled."); self->controls(); });
    apply = button("Apply Wallpaper", [self] { self->applySelected(); }); gtk_widget_add_css_class(apply, "suggested-action"); gtk_widget_set_hexpand(apply, true);
    gtk_box_append(GTK_BOX(actions), cancelButton); gtk_box_append(GTK_BOX(actions), apply); gtk_box_append(GTK_BOX(bottom), actions);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), bottom); adw_dialog_set_child(dialog, toolbar);
    update(appearance, true); source(wallhaven);
    timer = g_timeout_add(600, +[](gpointer p) -> gboolean { static_cast<WallpaperBrowser*>(p)->pollRemote(); return G_SOURCE_CONTINUE; }, this);
    adw_dialog_present(dialog, GTK_WIDGET(parent));
  }
public:
  WallpaperBrowser(GtkWindow* parent, std::string folder, std::string current, Callback changed)
    : parent(parent), folder(std::move(folder)), current(std::move(current)), changed(std::move(changed)) {}
  ~WallpaperBrowser() { if (timer) g_source_remove(timer); if (scan) { g_cancellable_cancel(scan); g_object_unref(scan); } }
  static std::shared_ptr<WallpaperBrowser> open(GtkWindow* parent, std::string folder, const Json& appearance, bool remote, Callback changed) {
    auto self = std::make_shared<WallpaperBrowser>(parent, std::move(folder), appearance.value("wallpaper", ""), std::move(changed)); self->build(remote, appearance); return self;
  }
  bool isClosed() const { return closed; }
  void present(bool wallhaven) { if (!closed) { gtk_drop_down_set_selected(sourceSelector, wallhaven ? 1 : 0); adw_dialog_present(dialog, GTK_WIDGET(parent)); } }
  void update(const Json& appearance, bool connected) {
    if (closed) return;
    available = connected && appearance.value("helix-desktop", false); online = appearance.value("wallhaven-online", false);
    const bool key = appearance.value("wallhaven-key-available", false);
    gtk_widget_set_sensitive(GTK_WIDGET(nsfw), key); if (!key) adw_switch_row_set_active(nsfw, false);
    gtk_label_set_text(keyLabel, key ? "Using your saved Wallhaven API key." : "Public search is available without an API key. Existing Wallhaven credentials are reused.");
    if (!available) { cancelWork(); message("Helix desktop services are unavailable. Reconnect to apply a wallpaper."); }
    else if (!online && remote) { cancelWork(); message("Wallhaven is unavailable while shell network requests are restricted."); }
    controls();
  }
  Json inspect() const { return {{"open", !closed}, {"source", remote ? "wallhaven" : "library"}, {"count", tiles.size()}, {"busy", busy},
    {"width", gtk_widget_get_width(GTK_WIDGET(dialog))}, {"height", gtk_widget_get_height(GTK_WIDGET(dialog))}, {"apply-enabled", bool(gtk_widget_get_sensitive(apply))}}; }
};
} // namespace helix::settings
