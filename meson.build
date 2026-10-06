project('noctalia', ['c', 'cpp'],
  version: '5.1.0',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',   # applied selectively to our target; avoids noise in vendored code
    'b_ndebug=if-release',
  ],
)

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')


# Clang requires an opt-in flag to properly use experimental C++ libraries.
# GCC/libstdc++ doesn't need this since these libraries are unconditionally
# exposed.
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>' ) != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Sanitizers are enabled via meson's built-in -Db_sanitize=address,undefined.
# lsan_suppressions.cpp is only needed when address sanitizer is active.
sanitize = get_option('b_sanitize') != 'none'

# Hardened standard-library assertions in debug builds only. A std::clamp(v, lo, hi)
# with hi < lo aborts here (distro packagers enable this in release, so users see
# the same abort), surfacing inverted-bound bugs loudly during development so they
# can be routed through util::clampOrdered. Release stays unhardened on purpose: a
# not-yet-converted raw std::clamp there should degrade to a wrong-but-non-fatal
# value rather than abort a user's session (converted sites log and return lo via
# clampOrdered). The cost is a few precondition branches (not the heavy _GLIBCXX_DEBUG).
if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif

# Auto mode defines tests for unsanitized debug builds. `just build` targets
# only the shell executable, while `just test` builds and runs the test targets.
tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') == 'debug' and not sanitize)

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-ffunction-sections', '-fdata-sections',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c', 'cpp'],
  )
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: ['c', 'cpp'])
endif

# ── Dependencies ──────────────────────────────────────────────────────────────
sdbus_cpp_dep       = dependency('sdbus-c++')
wayland_client_dep  = dependency('wayland-client')
wayland_protos_dep  = dependency('wayland-protocols')
#egl_dep             = dependency('egl')
#glesv2_dep          = dependency('glesv2')
wayland_egl_dep     = dependency('wayland-egl')
freetype2_dep       = dependency('freetype2')
fontconfig_dep      = dependency('fontconfig')
cairo_dep           = dependency('cairo')
cairo_ft_dep        = dependency('cairo-ft')
pango_dep           = dependency('pango')
pangocairo_dep      = dependency('pangocairo')
pangoft2_dep        = dependency('pangoft2')
harfbuzz_dep        = dependency('harfbuzz')
librsvg_dep         = dependency('librsvg-2.0')
xkbcommon_dep       = dependency('xkbcommon')
glib_dep            = dependency('glib-2.0')
gobject_dep         = dependency('gobject-2.0')
gio_dep             = dependency('gio-2.0')
libsecret_dep       = dependency('libsecret-1', version: '>=0.20')
libsodium_dep       = dependency('libsodium', version: '>=1.0.18')
polkit_agent_dep    = dependency('polkit-agent-1')
polkit_gobject_dep  = dependency('polkit-gobject-1')
# PipeWire/SPA headers use GCC extensions that trigger -Wpedantic/-Wconversion;
# mark as system so the compiler suppresses those warnings.
pipewire_dep        = dependency('libpipewire-0.3', include_type: 'system')
wireplumber_dep     = dependency('wireplumber-0.5', include_type: 'system')
curl_dep            = dependency('libcurl')
libqalculate_dep    = dependency('libqalculate')
libxml2_dep         = dependency('libxml-2.0')
nlohmann_dep        = dependency('nlohmann_json')
tomlplusplus_dep    = dependency('tomlplusplus')
libical_dep         = dependency('libical')
cups_dep            = dependency('cups')
pam_dep             = cc.find_library('pam')
dl_dep              = cpp.find_library('dl', required: false)

jemalloc_option = get_option('jemalloc')
is_glibc = cpp.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
jemalloc_deps = []
jemalloc_cpp_args = []

if jemalloc_option.enabled() and not is_glibc
  error('jemalloc support requires glibc')
endif

if is_glibc
  jemalloc_dep = dependency('jemalloc', required: jemalloc_option)
  if jemalloc_dep.found()
    jemalloc_deps += [jemalloc_dep]
    jemalloc_cpp_args += ['-DNOCTALIA_USE_JEMALLOC=1']
  endif
endif

egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)


if not egl_dep.found()
  egl_dep = dependency('wayland-egl', required: false)
endif

if not egl_dep.found()
  egl_dep = dependency('epoxy')
endif

if not gles2_dep.found()
  gles2_dep = dependency('epoxy')
endif


# ── Vendored: fzy matcher (C fuzzy scoring) ──────────────────────────────────
_fzy_lib = static_library('fzy',
  'third_party/fzy/src/match.c',
  c_args: ['-Wno-pedantic', '-Wno-conversion'],
  override_options: ['warning_level=0'],
)
fzy_dep = declare_dependency(
  link_whole: _fzy_lib,
  include_directories: include_directories('third_party/fzy/src', is_system: true),
)

# ── Vendored: Material Color Utilities (Google) ───────────────────────────────
# Upstream uses #include "cpp/..." prefixes, so the include root is the
# material_color_utilities/ dir itself. Built as a quiet static library.
_mcu_sources = files(
  'third_party/material_color_utilities/cpp/blend/blend.cc',
  'third_party/material_color_utilities/cpp/cam/cam.cc',
  'third_party/material_color_utilities/cpp/cam/hct.cc',
  'third_party/material_color_utilities/cpp/cam/hct_solver.cc',
  'third_party/material_color_utilities/cpp/cam/viewing_conditions.cc',
  'third_party/material_color_utilities/cpp/contrast/contrast.cc',
  'third_party/material_color_utilities/cpp/dislike/dislike.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_color.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_scheme.cc',
  'third_party/material_color_utilities/cpp/dynamiccolor/material_dynamic_colors.cc',
  'third_party/material_color_utilities/cpp/palettes/tones.cc',
  'third_party/material_color_utilities/cpp/quantize/celebi.cc',
  'third_party/material_color_utilities/cpp/quantize/lab.cc',
  'third_party/material_color_utilities/cpp/quantize/wsmeans.cc',
  'third_party/material_color_utilities/cpp/quantize/wu.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_content.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_expressive.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_fidelity.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_fruit_salad.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_monochrome.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_neutral.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_rainbow.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_tonal_spot.cc',
  'third_party/material_color_utilities/cpp/scheme/scheme_vibrant.cc',
  'third_party/material_color_utilities/cpp/score/score.cc',
  'third_party/material_color_utilities/cpp/temperature/temperature_cache.cc',
  'third_party/material_color_utilities/cpp/utils/utils.cc',
)
_mcu_lib = static_library('material_color_utilities',
  _mcu_sources,
  include_directories: include_directories('third_party/material_color_utilities', is_system: true),
  cpp_args: ['-Wno-pedantic', '-Wno-conversion', '-Wno-shadow', '-Wno-unused-parameter'],
  override_options: ['warning_level=0'],
)
mcu_dep = declare_dependency(
  link_whole: _mcu_lib,
  include_directories: include_directories('third_party/material_color_utilities', is_system: true),
)

_qrcodegen_lib = static_library('qrcodegen',
  files('third_party/qrcodegen/qrcodegen.cpp'),
  include_directories: include_directories('third_party/qrcodegen', is_system: true),
  cpp_args: ['-w'],
  override_options: ['warning_level=0'],
)
qrcodegen_dep = declare_dependency(
  link_whole: _qrcodegen_lib,
  include_directories: include_directories('third_party/qrcodegen', is_system: true),
)

# ── System: stb_image_resize2 and stb_image_write (header-only) ─────────────────────────────────
if not cc.has_header('stb/stb_image_resize2.h')
  error('stb/stb_image_resize2.h not found')
endif
if not cc.has_header('stb/stb_image_write.h')
  error('stb/stb_image_write.h not found')
endif

# ── System: libwebp (WebP decode and thumbnail encode; demux for animations) ─
libwebp_dep       = dependency('libwebp', required: true)
libwebpdemux_dep  = dependency('libwebpdemux', required: true)

# ── System: libjxl (JPEG XL decode) ───────────────────────────────────────────
libjxl_dep          = dependency('libjxl', required: true)
libjxl_threads_dep  = dependency('libjxl_threads', required: true)

# ── System: libsndfile (sound effect decoding) ───────────────────────────────
sndfile_dep = dependency('sndfile', required: true)

# Tests link vendored archives selectively. The application keeps link_whole so
# release LTO sees every member, but propagating that policy to every small test
# would embed all vendored code in every test executable.
_fzy_selective_dep = declare_dependency(
  link_with: _fzy_lib,
  include_directories: include_directories('third_party/fzy/src', is_system: true),
)
_mcu_selective_dep = declare_dependency(
  link_with: _mcu_lib,
  include_directories: include_directories('third_party/material_color_utilities', is_system: true),
)

# ── Wayland protocol generation ───────────────────────────────────────────────
wayland_scanner    = find_program('wayland-scanner')
wayland_protos_dir = wayland_protos_dep.get_variable('pkgdatadir')

# Generates a (header, source) pair from a protocol XML path/target.
# Returns [h_target, c_target]: add c_target to sources.
# The c_target has an order-only dep on h_target so the header exists when
# the generated .c is compiled (it does #include "foo-client-protocol.h").
_protocol_sources = []
_protocol_headers = []

# Helper macro via foreach; each entry: [name, xml_input]
# xml_input can be a string (path) or a custom_target.

foreach _p : [
  # ── wlr-layer-shell needs namespace→name_space substitution first ──────────
  # Handled separately below.

  # ── system wayland-protocols ──────────────────────────────────────────────
  ['xdg-output-unstable-v1',
    wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell',
    wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['ext-workspace-v1',
    wayland_protos_dir / 'staging/ext-workspace/ext-workspace-v1.xml'],
  ['ext-foreign-toplevel-list-v1',
    wayland_protos_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'],
  ['ext-data-control-v1',
    wayland_protos_dir / 'staging/ext-data-control/ext-data-control-v1.xml'],
  ['xdg-activation-v1',
    wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['ext-session-lock-v1',
    wayland_protos_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'],
  ['ext-idle-notify-v1',
    wayland_protos_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'],
  ['ext-background-effect-v1',
    wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['ext-image-capture-source-v1',
    wayland_protos_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml'],
  ['ext-image-copy-capture-v1',
    wayland_protos_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'],
  ['fractional-scale-v1',
    wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter',
    wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['idle-inhibit-unstable-v1',
    wayland_protos_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],

  # ── vendored / project-local protocols (loaded from protocols/) ───────────
  ['text-input-unstable-v3',
    'protocols/text-input-unstable-v3.xml'],
  ['virtual-keyboard-unstable-v1',
    'protocols/virtual-keyboard-unstable-v1.xml'],
  ['cursor-shape-v1',
    'protocols/cursor-shape-v1.xml'],
  ['wlr-foreign-toplevel-management-unstable-v1',
    'protocols/wlr-foreign-toplevel-management-unstable-v1.xml'],
  ['wlr-data-control-unstable-v1',
    'protocols/wlr-data-control-unstable-v1.xml'],
  ['wlr-gamma-control-unstable-v1',
    'protocols/wlr-gamma-control-unstable-v1.xml'],
  ['wlr-screencopy-unstable-v1',
    'protocols/wlr-screencopy-unstable-v1.xml'],
  ['wlr-output-management-unstable-v1',
    'protocols/wlr-output-management-unstable-v1.xml'],
]
  _name = _p[0]
  _xml  = _p[1]

  _h = custom_target(_name + '-client-header',
    input:   _xml,
    output:  _name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  _c = custom_target(_name + '-client-source',
    input:   _xml,
    output:  _name + '-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: _h,  # ensure .h exists before this .c is compiled (it #includes it)
  )
  _protocol_sources += _c
  _protocol_headers += _h
endforeach

# wlr-layer-shell: 'namespace' is a C++ keyword; rename it in the generated
# header only (private-code is compiled as C so it's fine as-is).
_wlr_ls_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
_wlr_ls_h = custom_target('wlr-layer-shell-unstable-v1-client-header',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [
    'sh', '-c',
    '"$1" client-header "$2" "$3" && sed -i s/namespace/name_space/g "$3"',
    '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@',
  ],
)
_wlr_ls_c = custom_target('wlr-layer-shell-unstable-v1-client-source',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  depends: _wlr_ls_h,
)
_protocol_sources += _wlr_ls_c
_protocol_headers += _wlr_ls_h

alias_target('wayland-protocol-headers', _protocol_headers)

# ── Main sources ──────────────────────────────────────────────────────────────
_noctalia_sources = files(
  'src/app/application.cpp',
  'src/app/application_events.cpp',
  'src/app/application_ipc.cpp',
  'src/app/application_poll_sources.cpp',
  'src/app/application_services.cpp',
  'src/app/application_ui.cpp',
  'src/app/main_loop.cpp',
  'src/app/single_instance_lock.cpp',
  'src/auth/fingerprint_authenticator.cpp',
  'src/auth/pam_authenticator.cpp',
  'src/config/atomic_file.cpp',
  'src/config/config_export.cpp',
  'src/config/config_merge.cpp',
  'src/config/config_migrations.cpp',
  'src/config/config_origins.cpp',
  'src/config/config_overrides.cpp',
  'src/config/config_service.cpp',
  'src/config/config_types.cpp',
  'src/config/config_validate.cpp',
  'src/config/widget_config.cpp',
  'src/config/schema/config_schema.cpp',
  'src/config/schema/config_sections.cpp',
  'src/config/state_store.cpp',
  'src/config/cli.cpp',
  'src/core/build_info.cpp',
  'src/core/deferred_call.cpp',
  'src/core/process/process_fds.cpp',
  'src/core/files/file_watcher.cpp',
  'src/core/input/key_chord.cpp',
  'src/core/log.cpp',
  'src/core/process/process.cpp',
  'src/core/files/resource_paths.cpp',
  'src/core/inotify/inotify.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/dbus/accounts/accounts_service.cpp',
  'src/dbus/bluetooth/bluetooth_agent.cpp',
  'src/dbus/bluetooth/bluetooth_service.cpp',
  'src/dbus/idle/screensaver_service.cpp',
  'src/dbus/logind/logind_service.cpp',
  'src/dbus/mpris/mpris_art.cpp',
  'src/dbus/mpris/mpris_service.cpp',
  'src/dbus/network/inetwork_service.cpp',
  'src/dbus/network/iwd_secret_agent.cpp',
  'src/dbus/network/iwd_service.cpp',
  'src/dbus/network/network_display.cpp',
  'src/dbus/network/network_manager_service.cpp',
  'src/dbus/network/network_secret_agent.cpp',
  'src/dbus/network/wpa_supplicant_service.cpp',
  'src/dbus/polkit/polkit_agent.cpp',
  'src/dbus/polkit/polkit_session_support.cpp',
  'src/dbus/notification/notification_poll_source.cpp',
  'src/dbus/notification/notification_service.cpp',
  'src/dbus/power/power_profiles_service.cpp',
  'src/dbus/bus_name_watch.cpp',
  'src/dbus/session_bus.cpp',
  'src/dbus/system_bus.cpp',
  'src/dbus/tray/fcitx_status.cpp',
  'src/dbus/tray/tray_service.cpp',
  'src/dbus/upower/upower_charge_limit_support.cpp',
  'src/dbus/upower/upower_service.cpp',
  'src/debug/debug_service.cpp',
  'src/i18n/i18n.cpp',
  'src/i18n/language_tag.cpp',
  'src/i18n/i18n_service.cpp',
  'src/hooks/battery_hook_state.cpp',
  'src/hooks/hook_manager.cpp',
  'src/idle/idle_inhibitor.cpp',
  'src/idle/idle_grace_overlay.cpp',
  'src/idle/idle_manager.cpp',
  'src/ipc/ipc_client.cpp',
  'src/ipc/ipc_service.cpp',
  'src/launcher/actions_provider.cpp',
  'src/launcher/app_provider.cpp',
  'src/launcher/desktop_actions.cpp',
  'src/launcher/dmenu_cli.cpp',
  'src/launcher/dmenu_ipc.cpp',
  'src/launcher/dmenu_provider.cpp',
  'src/launcher/dmenu_socket_path.cpp',
  'src/launcher/dmenu_stdin_provider.cpp',
  'src/launcher/emoji_provider.cpp',
  'src/launcher/math_provider.cpp',
  'src/launcher/panel_catalog.cpp',
  'src/launcher/panel_provider.cpp',
  'src/launcher/session_provider.cpp',
  'src/launcher/wallpaper_provider.cpp',
  'src/launcher/window_provider.cpp',
  'src/launcher/usage_tracker.cpp',
  'src/net/http_client.cpp',
  'src/net/uri.cpp',
  'src/net/url_open.cpp',
  'src/notification/notification_manager.cpp',
  'src/notification/notification_filter.cpp',
  'src/notification/notification_semantics.cpp',
  'src/notification/notification_display_name.cpp',
  'src/notification/notification_history_store.cpp',
  'src/pipewire/pipewire_service.cpp',
  'src/pipewire/audio_glyphs.cpp',
  'src/pipewire/audio_route_selection.cpp',
  'src/pipewire/wireplumber_mixer.cpp',
  'src/pipewire/privacy_filter.cpp',
  'src/pipewire/pipewire_spectrum.cpp',
  'src/pipewire/sound_player.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/async_texture_cache.cpp',
  'src/render/core/color.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/core/stb_image_resize_impl.cpp',
  'src/render/core/thumbnail_service.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/core/image_encoder.cpp',
  'src/render/core/image_orientation.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/linear_gradient_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/screen_corner_program.cpp',
  'src/render/programs/countdown_ring_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/programs/audio_spectrum_program.cpp',
  'src/render/programs/fancy_audio_visualizer_program.cpp',
  'src/render/programs/effect_program.cpp',
  'src/render/programs/graph_program.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/wallpaper_program.cpp',
  'src/render/core/blur_cache.cpp',
  'src/render/core/cached_layer.cpp',
  'src/render/core/shared_texture_cache.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/fancy_audio_visualizer_node.cpp',
  'src/render/scene/graph_node.cpp',
  'src/render/scene/node.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/font_registry.cpp',
  'src/render/text/font_weight_catalog.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/wallpaper_renderer.cpp',
  'src/security/encrypted_file_store.cpp',
  'src/security/secret_store.cpp',
  'src/security/secure_buffer.cpp',
  'src/security/storage_key_provider.cpp',
  'src/system/battery_warning_monitor.cpp',
  'src/shell/bar/bar.cpp',
  'src/shell/bar/bar_widget_layout.cpp',
  'src/shell/clipboard/clipboard_paste.cpp',
  'src/shell/dock/dock.cpp',
  'src/shell/dock/dock_context_menu.cpp',
  'src/shell/dock/dock_geometry.cpp',
  'src/shell/dock/dock_instance.cpp',
  'src/shell/dock/dock_items.cpp',
  'src/shell/dock/dock_model.cpp',
  'src/shell/dock/pinned_apps.cpp',
  'src/shell/desktop/desktop_widget.cpp',
  'src/shell/desktop/desktop_widget_factory.cpp',
  'src/shell/desktop/desktop_widget_settings_registry.cpp',
  'src/shell/desktop/desktop_widgets_controller.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor_types.cpp',
  'src/shell/desktop/editor/desktop_widgets_editor_settings.cpp',
  'src/shell/desktop/desktop_widgets_host.cpp',
  'src/shell/lockscreen/lockscreen_login_box.cpp',
  'src/shell/lockscreen/lockscreen_widgets_controller.cpp',
  'src/shell/lockscreen/lockscreen_widgets_host.cpp',
  'src/shell/desktop/widgets/desktop_audio_visualizer_widget.cpp',
  'src/shell/desktop/widgets/desktop_button_widget.cpp',
  'src/shell/desktop/widgets/desktop_path_widget.cpp',
  'src/shell/desktop/widgets/desktop_clock_widget.cpp',
  'src/shell/desktop/widgets/desktop_fancy_audio_visualizer_widget.cpp',
  'src/shell/desktop/widgets/desktop_label_widget.cpp',
  'src/shell/desktop/widgets/desktop_login_box_widget.cpp',
  'src/shell/desktop/widgets/desktop_media_player_widget.cpp',
  'src/shell/desktop/widgets/desktop_sticker_widget.cpp',
  'src/shell/desktop/widgets/desktop_sysmon_widget.cpp',
  'src/shell/desktop/widgets/desktop_volume_widget.cpp',
  'src/shell/clipboard/clipboard_picker.cpp',
  'src/shell/quick_insert/quick_insert_panel.cpp',
  'src/shell/control_center/shortcut_registry.cpp',
  'src/shell/quick_settings/quick_settings_panel.cpp',
  'src/shell/control_center/tab.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/ui/dialogs/color_picker_dialog.cpp',
  'src/ui/dialogs/color_picker_dialog_popup.cpp',
  'src/ui/dialogs/dialog_popup_host.cpp',
  'src/ui/dialogs/file_dialog.cpp',
  'src/ui/dialogs/file_dialog_popup.cpp',
  'src/ui/dialogs/file_dialog_view.cpp',
  'src/ui/dialogs/file_entry_row.cpp',
  'src/ui/dialogs/file_entry_tile.cpp',
  'src/ui/dialogs/glyph_picker_dialog.cpp',
  'src/ui/dialogs/glyph_picker_dialog_popup.cpp',
  'src/ui/dialogs/layer_popup_host.cpp',
  'src/shell/launcher/launcher_panel.cpp',
  'src/shell/switcher/window_switcher.cpp',
  'src/shell/switcher/window_switcher_tile.cpp',
  'src/shell/profile/avatar_path.cpp',
  'src/shell/greeter/greeter_appearance_sync.cpp',
  'src/shell/lockscreen/lock_screen.cpp',
  'src/shell/lockscreen/lock_surface.cpp',
  'src/shell/notification/notification_toast.cpp',
  'src/shell/osd/audio_osd.cpp',
  'src/shell/osd/media_osd.cpp',
  'src/shell/osd/brightness_osd.cpp',
  'src/shell/osd/keyboard_backlight_osd.cpp',
  'src/shell/osd/lock_keys_osd.cpp',
  'src/shell/osd/keyboard_layout_osd.cpp',
  'src/shell/osd/privacy_osd.cpp',
  'src/shell/osd/osd_overlay.cpp',
  'src/shell/tooltip/tooltip_manager.cpp',
  'src/shell/hot_corners/hot_corners.cpp',
  'src/shell/screen_corners/screen_corners.cpp',
  'src/shell/panel/panel_click_shield.cpp',
  'src/shell/panel/panel_manager.cpp',
  'src/shell/panel/persistent_panel_host.cpp',
  'src/shell/polkit/polkit_panel.cpp',
  'src/shell/agent/agent_confirmation_queue.cpp',
  'src/shell/agent/agent_confirm_panel.cpp',
  'src/shell/session/session_action_meta.cpp',
  'src/shell/session/session_action_runner.cpp',
  'src/shell/session/session_ipc.cpp',
  'src/shell/session/session_panel.cpp',
  'src/shell/setup_wizard/setup_wizard_panel.cpp',
  'src/shell/surface/shadow.cpp',
  'src/shell/settings/bar_widget_editor.cpp',
  'src/shell/settings/color_spec_picker.cpp',
  'src/shell/settings/config_export_dialog_modal.cpp',
  'src/shell/settings/settings_content.cpp',
  'src/shell/settings/settings_content_common.cpp',
  'src/shell/settings/path_browse.cpp',
  'src/shell/settings/template_store_content.cpp',
  'src/shell/settings/template_store_tile.cpp',
  'src/shell/settings/settings_control_factory.cpp',
  'src/shell/settings/settings_content_idle_behavior.cpp',
  'src/shell/settings/settings_content_notification_filter.cpp',
  'src/shell/settings/settings_content_session_action.cpp',
  'src/shell/settings/settings_bar_management.cpp',
  'src/shell/settings/settings_registry.cpp',
  'src/shell/settings/font_family_catalog.cpp',
  'src/shell/settings/font_weight_catalog.cpp',
  'src/shell/settings/widget_settings_registry.cpp',
  'src/shell/settings/widget_add_popup.cpp',
  'src/shell/settings/settings_modal_host.cpp',
  'src/shell/test/test_panel.cpp',
  'src/shell/tray/tray_drawer_panel.cpp',
  'src/shell/tray/tray_settings.cpp',
  'src/shell/tray/tray_menu.cpp',
  'src/shell/backdrop/backdrop.cpp',
  'src/shell/backdrop/backdrop_surface.cpp',
  'src/shell/overview/overview_launcher_capture.cpp',
  'src/system/app_identity.cpp',
  'src/shell/wallpaper/wallpaper_paths.cpp',
  'src/shell/wallpaper/wallpaper_geometry.cpp',
  'src/shell/wallpaper/wallpaper_shuffle_state.cpp',
  'src/shell/wallpaper/daily_wallpaper.cpp',
  'src/shell/wallpaper/daily_wallpaper_service.cpp',
  'src/shell/wallpaper/wallhaven_client.cpp',
  'src/shell/wallpaper/video_wallpaper.cpp',
  'src/shell/wallpaper/video_wallpaper_service.cpp',
  'src/shell/wallpaper/wallpaper.cpp',
  'src/shell/wallpaper/panel/wallpaper_panel.cpp',
  'src/shell/wallpaper/panel/wallpaper_picker.cpp',
  'src/shell/wallpaper/panel/wallpaper_scanner.cpp',
  'src/shell/wallpaper/panel/wallpaper_tile.cpp',
  'src/shell/keyboard_layout_label.cpp',
  'src/shell/bar/widget.cpp',
  'src/shell/bar/widget_action.cpp',
  'src/shell/bar/widget_action_dispatcher.cpp',
  'src/shell/bar/widget_custom_image.cpp',
  'src/shell/bar/widget_factory.cpp',
  'src/shell/bar/widget_gesture.cpp',
  'src/shell/bar/widget_gesture_defaults.cpp',
  'src/shell/bar/widgets/active_window_widget.cpp',
  'src/shell/bar/widgets/active_window_widget_definition.cpp',
  'src/shell/bar/widgets/audio_visualizer_widget.cpp',
  'src/shell/bar/widgets/audio_visualizer_widget_definition.cpp',
  'src/shell/bar/widgets/battery_widget.cpp',
  'src/shell/bar/widgets/battery_widget_definition.cpp',
  'src/shell/bar/widgets/bluetooth_widget.cpp',
  'src/shell/bar/widgets/bluetooth_widget_definition.cpp',
  'src/shell/bar/widgets/control_center_widget.cpp',
  'src/shell/bar/widgets/control_center_widget_definition.cpp',
  'src/shell/bar/widgets/custom_button_widget.cpp',
  'src/shell/bar/widgets/custom_button_widget_definition.cpp',
  'src/shell/bar/widgets/clipboard_widget.cpp',
  'src/shell/bar/widgets/clipboard_widget_definition.cpp',
  'src/shell/bar/widgets/clock_widget.cpp',
  'src/shell/bar/widgets/clock_widget_definition.cpp',
  'src/shell/bar/widgets/debug_indicator_widget.cpp',
  'src/shell/bar/widgets/idle_inhibitor_widget.cpp',
  'src/shell/bar/widgets/caffeine_widget_definition.cpp',
  'src/shell/bar/widgets/keyboard_layout_widget.cpp',
  'src/shell/bar/widgets/keyboard_layout_widget_definition.cpp',
  'src/shell/bar/widgets/lock_keys_widget.cpp',
  'src/shell/bar/widgets/lock_keys_widget_definition.cpp',
  'src/shell/bar/widgets/nightlight_widget.cpp',
  'src/shell/bar/widgets/nightlight_widget_definition.cpp',
  'src/shell/bar/widgets/launcher_widget.cpp',
  'src/shell/bar/widgets/launcher_widget_definition.cpp',
  'src/shell/bar/widgets/media_widget.cpp',
  'src/shell/bar/widgets/media_widget_definition.cpp',
  'src/shell/bar/widgets/network_widget.cpp',
  'src/shell/bar/widgets/network_widget_definition.cpp',
  'src/shell/bar/widgets/notification_widget.cpp',
  'src/shell/bar/widgets/notification_widget_definition.cpp',
  'src/shell/bar/widgets/power_profile_widget.cpp',
  'src/shell/bar/widgets/power_profile_widget_definition.cpp',
  'src/shell/bar/widgets/privacy_widget.cpp',
  'src/shell/bar/widgets/privacy_widget_definition.cpp',
  'src/shell/bar/widgets/screenshot_widget_definition.cpp',
  'src/shell/bar/widgets/session_widget.cpp',
  'src/shell/bar/widgets/session_widget_definition.cpp',
  'src/shell/bar/widgets/settings_widget.cpp',
  'src/shell/bar/widgets/settings_widget_definition.cpp',
  'src/shell/bar/widgets/spacer_widget.cpp',
  'src/shell/bar/widgets/spacer_widget_definition.cpp',
  'src/shell/bar/widgets/text_widget.cpp',
  'src/shell/bar/widgets/text_widget_definition.cpp',
  'src/shell/bar/widgets/sysmon_widget.cpp',
  'src/shell/bar/widgets/sysmon_widget_definition.cpp',
  'src/shell/bar/widgets/taskbar_widget.cpp',
  'src/shell/bar/widgets/taskbar_widget_definition.cpp',
  'src/shell/bar/widgets/test_widget.cpp',
  'src/shell/bar/widgets/test_widget_definition.cpp',
  'src/shell/bar/widgets/theme_mode_widget.cpp',
  'src/shell/bar/widgets/theme_mode_widget_definition.cpp',
  'src/shell/bar/widgets/tray_widget.cpp',
  'src/shell/bar/widgets/tray_widget_definition.cpp',
  'src/shell/bar/widgets/volume_widget.cpp',
  'src/shell/bar/widgets/volume_widget_definition.cpp',
  'src/shell/bar/widgets/brightness_widget.cpp',
  'src/shell/bar/widgets/brightness_widget_definition.cpp',
  'src/shell/bar/widgets/wallpaper_widget.cpp',
  'src/shell/bar/widgets/wallpaper_widget_definition.cpp',
  'src/shell/bar/widgets/workspaces_widget.cpp',
  'src/shell/bar/widgets/workspaces_widget_definition.cpp',
  'src/system/dependency_service.cpp',
  'src/system/desktop_entry.cpp',
  'src/system/freedesktop_key_file.cpp',
  'src/system/desktop_entry_launch.cpp',
  'src/system/terminal_launch.cpp',
  'src/system/brightness_service.cpp',
  'src/system/display_config.cpp',
  'src/system/display_config_service.cpp',
  'src/system/keyboard_backlight_service.cpp',
  'src/system/lock_keys_service.cpp',
  'src/system/distro_info.cpp',
  'src/system/fedora_upgrade.cpp',
  'src/system/fedora_upgrade_launcher.cpp',
  'src/system/package_kit_service.cpp',
  'src/system/package_spec.cpp',
  'src/system/disk_mounts.cpp',
  'src/system/format_units.cpp',
  'src/system/hardware_info.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/internal_app_metadata.cpp',
  'src/system/day_night_schedule.cpp',
  'src/system/gamma_service.cpp',
  'src/capture/screencopy_capture.cpp',
  'src/capture/pointer_cursor_capture.cpp',
  'src/capture/screencopy_util.cpp',
  'src/capture/screenshot_capture.cpp',
  'src/capture/screenshot_image.cpp',
  'src/capture/screenshot_service.cpp',
  'src/capture/screen_recorder_service.cpp',
  'src/capture/screenshot_region_overlay.cpp',
  'src/capture/annotation_document.cpp',
  'src/capture/annotation_raster.cpp',
  'src/capture/annotation_overlay.cpp',
  'src/shell/bar/widgets/screenshot_widget.cpp',
  'src/system/rfkill_helper.cpp',
  'src/system/cpu_stat.cpp',
  'src/system/cpu_freq.cpp',
  'src/system/cpu_temp_sensor.cpp',
  'src/system/intel_gpu.cpp',
  'src/system/system_monitor_service.cpp',
  'src/time/time_format.cpp',
  'src/time/time_service.cpp',
  'src/ui/visuals/audio_visualizer.cpp',
  'src/ui/visuals/fancy_audio_visualizer.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/drag_source.cpp',
  'src/ui/controls/drop_zone.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/color_picker.cpp',
  'src/ui/controls/collapsible.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/context_menu_popup.cpp',
  'src/ui/controls/countdown_ring.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/graph.cpp',
  'src/ui/controls/glyph_picker.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/ui/controls/grid_view.cpp',
  'src/ui/controls/grid_tile.cpp',
  'src/ui/controls/image.cpp',
  'src/ui/controls/input.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/ui/controls/keybind_recorder.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/list_editor.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/roving_list_nav.cpp',
  'src/ui/controls/search_picker.cpp',
  'src/ui/controls/range_slider.cpp',
  'src/ui/controls/screen_corner.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/select.cpp',
  'src/ui/controls/select_dropdown_popup.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/builders.cpp',
  'src/ui/app_icon_colorization.cpp',
  'src/ui/palette.cpp',
  'src/ui/popup_chrome.cpp',
  'src/ui/scroll_into_view.cpp',
  'src/ui/split_pane_focus.cpp',
  'src/ui/style.cpp',
  'src/ui/drag_drop_controller.cpp',
  'src/util/checksum.cpp',
  'src/util/clamp.cpp',
  'src/util/fuzzy_match.cpp',
  'src/util/qr_code.cpp',
  'src/wayland/clipboard_service.cpp',
  'src/wayland/text_input_service.cpp',
  'src/wayland/virtual_keyboard_service.cpp',
  'src/compositors/compositor_platform.cpp',
  'src/compositors/workspace_alert_service.cpp',
  'src/compositors/compositor_detect.cpp',
  'src/compositors/compositor_runtime.cpp',
  'src/compositors/ext_workspace/ext_workspace_output_backend.cpp',
  'src/compositors/ext_workspace/ext_workspace_backend.cpp',
  'src/compositors/niri/niri_runtime.cpp',
  'src/compositors/niri/niri_event_handler.cpp',
  'src/compositors/niri/niri_output_backend.cpp',
  'src/compositors/niri/niri_keyboard_backend.cpp',
  'src/compositors/niri/niri_workspace_backend.cpp',
  'src/compositors/niri/niri_config_files.cpp',
  'src/compositors/niri/niri_config_binds.cpp',
  'src/compositors/niri/niri_config_keyboard.cpp',
  'src/compositors/niri/niri_config_outputs.cpp',
  'src/compositors/niri/niri_window_rules.cpp',
  'src/compositors/niri/niri_layout_theme.cpp',
  'src/compositors/niri/niri_config_blur.cpp',
  'src/compositors/niri/niri_config_overview.cpp',
  'src/compositors/niri/niri_config_setup.cpp',
  'src/wayland/layer_surface.cpp',
  'src/wayland/output_probe.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/subsurface.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_seat.cpp',
  'src/wayland/wayland_toplevels.cpp',
  'src/wayland/ext_foreign_toplevels.cpp',
  'src/wayland/wayland_workspaces.cpp',
  'src/theme/scheme.cpp',
  'src/theme/color.cpp',
  'src/theme/contrast.cpp',
  'src/theme/image_loader.cpp',
  'src/theme/palette_generator.cpp',
  'src/theme/palette_transform.cpp',
  'src/theme/m3_schemes.cpp',
  'src/theme/custom_schemes.cpp',
  'src/theme/builtin_palettes.cpp',
  'src/theme/builtin_templates.cpp',
  'src/theme/adw_gtk3.cpp',
  'src/theme/community_palettes.cpp',
  'src/theme/community_templates.cpp',
  'src/theme/custom_palettes.cpp',
  'src/theme/fixed_palette.cpp',
  'src/theme/template_apply_service.cpp',
  'src/theme/theme_service.cpp',
  'src/theme/json_output.cpp',
  'src/theme/kde_color_scheme.cpp',
  'src/theme/firefox_theme/css.cpp',
  'src/theme/firefox_theme/firefox_theme.cpp',
  'src/theme/firefox_theme/native_messaging.cpp',
  'src/theme/firefox_theme/settings.cpp',
  'src/theme/hook_runner.cpp',
  'src/theme/template_engine.cpp',
  'src/cli/completions.cpp',
  'src/cli/schema_json.cpp',
  'src/cli/parse.cpp',
  'src/cli/help.cpp',
  'src/theme/cli.cpp',
  'src/ipc/cli.cpp',
)

_noctalia_sources += files('src/desktop/protocol.cpp', 'src/desktop/client.cpp', 'src/desktop/remote_network.cpp')

if sanitize
  _noctalia_sources += files('src/core/lsan_suppressions.cpp')
endif

git_program = find_program('git', required: false)
if git_program.found()
  _git_revision_h = vcs_tag(
    input: 'src/core/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  _git_revision_config = configuration_data()
  _git_revision_config.set('VCS_TAG', 'unknown')
  _git_revision_h = configure_file(
    input: 'src/core/git_revision.h.in',
    output: 'noctalia_git_revision.h',
    configuration: _git_revision_config,
  )
endif

# ── Include directories ────────────────────────────────────────────────────────
_noctalia_inc = [
  include_directories('src'),
  include_directories('third_party/wuffs', is_system: true),
  # '.' adds both the source root and the corresponding build-dir root, so
  # generated protocol headers (written to the build root) are found by #include.
  include_directories('.'),
]

# ── Application library and executable ────────────────────────────────────────
_noctalia_system_dependencies = [
  sdbus_cpp_dep,
  wayland_client_dep,
  egl_dep,
  gles2_dep,
  wayland_egl_dep,
  freetype2_dep,
  fontconfig_dep,
  cairo_dep,
  cairo_ft_dep,
  pango_dep,
  pangocairo_dep,
  pangoft2_dep,
  harfbuzz_dep,
  librsvg_dep,
  xkbcommon_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  libsecret_dep,
  libsodium_dep,
  polkit_agent_dep,
  polkit_gobject_dep,
  pipewire_dep,
  wireplumber_dep,
  curl_dep,
  libqalculate_dep,
  pam_dep,
  nlohmann_dep,
  libxml2_dep,
  libical_dep,
  cups_dep,
  tomlplusplus_dep,
  libwebp_dep,
  libwebpdemux_dep,
  libjxl_dep,
  libjxl_threads_dep,
  sndfile_dep,
] + jemalloc_deps + (dl_dep.found() ? [dl_dep] : [])

_noctalia_dependencies = _noctalia_system_dependencies + [
  fzy_dep,
  mcu_dep,
  qrcodegen_dep,
]

_noctalia_test_dependencies = _noctalia_system_dependencies + [
  _fzy_selective_dep,
  _mcu_selective_dep,
]

noctalia_system_upgrade_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'noctalia-system-upgrade')

_noctalia_compile_definitions = [
  '-DNOCTALIA_INSTALL_PREFIX="' + get_option('prefix') + '"',
  '-DNOCTALIA_INSTALL_DATADIR="' + get_option('datadir') + '"',
  '-DNOCTALIA_VERSION="' + meson.project_version() + '"',
  '-DNOCTALIA_SYSTEM_UPGRADE_PATH="' + noctalia_system_upgrade_path + '"',
] + jemalloc_cpp_args

_noctalia_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
] + _noctalia_compile_definitions

# Production sources and generated protocols compile once. The shell links the
# archive whole; tests link it normally so only the objects they exercise enter
# each test executable.
noctalia_core_lib = static_library('noctalia_core',
  sources: _noctalia_sources + _protocol_sources + _protocol_headers + [_git_revision_h],
  dependencies: _noctalia_dependencies,
  include_directories: _noctalia_inc,
  cpp_args: _noctalia_cpp_args,
)

noctalia_core_dep = declare_dependency(
  link_with: noctalia_core_lib,
  dependencies: _noctalia_test_dependencies,
  include_directories: _noctalia_inc,
  compile_args: _noctalia_compile_definitions,
  sources: _protocol_headers,
)

noctalia_exe = executable('noctalia',
  sources: files('src/main.cpp') + _protocol_headers,
  dependencies: _noctalia_dependencies,
  include_directories: _noctalia_inc,
  cpp_args: _noctalia_cpp_args,
  objects: noctalia_core_lib.extract_all_objects(recursive: false),
  install: true,
)

install_subdir('assets',
  install_dir: get_option('datadir') / 'noctalia',
  exclude_files: ['dev.noctalia.Noctalia.desktop'],
)

install_data(
  'assets/dev.noctalia.Noctalia.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data(
  'assets/noctalia.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# Helix Phone: BLE HID pointer for iPhone AssistiveTouch and its bluetoothd setup helper.
install_data(
  install_dir: get_option('datadir') / 'noctalia' / 'scripts',
  install_mode: 'rwxr-xr-x',
)

executable('noctalia-system-upgrade',
  sources: files(
    'src/system/fedora_upgrade.cpp',
    'src/system/fedora_upgrade_main.cpp',
  ),
  include_directories: include_directories('src'),
  cpp_args: ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow'],
  install: true,
  install_dir: get_option('libexecdir'),
)

noctalia_upgrade_policy_conf = configuration_data()
noctalia_upgrade_policy_conf.set('HELPER_PATH', noctalia_system_upgrade_path)
configure_file(
  input: 'assets/polkit/dev.noctalia.Noctalia.upgrade.policy.in',
  output: 'dev.noctalia.Noctalia.upgrade.policy',
  configuration: noctalia_upgrade_policy_conf,
  install: true,
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)

if build_tests
  _cpp_test_names = [
    'adw_gtk3',
    'annotation_document',
    'annotation_raster',
    'animation_manager',
    'app_identity',
    'audio_glyphs',
    'audio_route_selection',
    'battery_health',
    'battery_hook_state',
    'button_layout',
    'cairo_text_renderer',
    'capsule_group_lane_override',
    'capsule_group_reconcile',
    'cli_help',
    'cli_parse',
    'cli_schema',
    'clipboard_service',
    'clipboard_storage_permissions',
    'daily_wallpaper',
    'config_migration',
    'config_override_mutation',
    'config_path_resolution',
    'config_schema_roundtrip',
    'config_wallpaper_precedence',
    'config_widget',
    'context_menu_popup',
    'cpu_freq',
    'cpu_stat',
    'cpu_temp_sensor',
    'desktop_entry_launch',
    'desktop_entry_localization',
    'disk_mounts',
    'display_config',
    'dock_edge_overlap',
    'dock_pinned_apps',
    'enterprise_credentials',
    'fcitx_status',
    'fedora_upgrade',
    'freedesktop_key_file',
    'greeter_appearance_sync',
    'grid_item_cursor',
    'hook_manager',
    'i18n_language_tag',
    'i18n_supported_languages',
    'ico_decoder',
    'icon_resolver',
    'image_file_loader_data_uri',
    'image_orientation',
    'image_source_log',
    'inotify',
    'input_area',
    'input_dispatcher',
    'input_password_mode',
    'input_readline_shortcuts',
    'ipc_service',
    'jxl_decoder',
    'kde_color_scheme',
    'keyboard_layout_label',
    'lane_selection_token',
    'log',
    'math_provider',
    'monitor_selector',
    'network_display',
    'network_manager_security',
    'niri_config_binds',
    'niri_config_outputs',
    'niri_config_blur',
    'niri_config_overview',
    'niri_layout_theme',
    'niri_window_rules',
    'niri_config_setup',
    'niri_workspace_backend',
    'notification_dnd',
    'notification_filter',
    'notification_history_dismiss',
    'notification_history_utf8',
    'notification_live_activity',
    'package_spec',
    'path_browse',
    'popup_dismiss_order',
    'process',
    'scheme',
    'scrollbar_geometry',
    'secret_store',
    'security_primitives',
    'state_store',
    'string_utils',
    'surface_blur_region',
    'system_monitor_service',
    'taskbar_widget',
    'template_apply_notify',
    'template_hook_async',
    'template_hook_runner',
    'theme_shell_mode',
    'time_format',
    'tray_identifier',
    'upower_device_catalog',
    'uri',
    'virtual_grid_view_gesture',
    'wallpaper_scan_symlink',
    'wallpaper_shuffle_state',
    'wallhaven_client',
    'video_wallpaper',
    'wayland_output_scale',
    'wayland_toplevels_identity',
    'webp_decoder',
    'widget_action',
    'widget_definition',
    'workspace_alert_service',
  ]

  foreach test_name : _cpp_test_names
    test_exe = executable(test_name + '_test',
      sources: files('tests' / (test_name + '_test.cpp')),
      dependencies: noctalia_core_dep,
      override_options: ['b_ndebug=false'],
    )
    test_args = []
    if test_name == 'desktop_entry_localization'
      test_args = [meson.project_source_root() / 'assets']
    elif test_name == 'i18n_supported_languages'
      test_args = [meson.project_source_root() / 'assets' / 'translations']
    endif
    test(test_name, test_exe, args: test_args)
  endforeach

  config_validate_env = environment()
  config_validate_env.set('NOCTALIA_CONFIG_HOME', meson.current_source_dir() / 'tests/config_validate/config-home')
  config_validate_env.set('NOCTALIA_STATE_HOME', meson.current_source_dir() / 'tests/config_validate/state-home')
  config_validate_env.set('NOCTALIA_DATA_HOME', meson.current_source_dir() / 'tests/config_validate/data-home')

  sh_program = find_program('sh')
  test('config_validate_cli',
    sh_program,
    args: ['tests/config_validate_cli_test.sh', noctalia_exe.full_path()],
    depends: noctalia_exe,
    workdir: meson.current_source_dir(),
    env: config_validate_env,
  )

  bash_program = find_program('bash')
  test('template_undo_signal',
    bash_program,
    args: ['tests/template_undo_signal_test.sh', meson.current_source_dir() / 'assets/templates'],
    workdir: meson.current_source_dir(),
  )
endif # build_tests


# Independent desktop service and adaptive applications. GTK never enters shell dependencies.
# D-Bus contract: protocols/dbus/*.xml is the single source of truth for interface shapes.
helix_desktop_introspection_h = custom_target('helix-desktop-introspection',
  input: 'protocols/dbus/org.helix.Desktop.xml',
  output: 'helix_desktop_introspection.h',
  command: [find_program('python3'), files('scripts/embed_text.py'), '@INPUT@', '@OUTPUT@', 'desktopIntrospectionXml'],
)

helix_service = executable('helix-service',
  sources: [files('src/desktop/service_main.cpp', 'src/desktop/settings_catalog.cpp', 'src/desktop/input_meter.cpp', 'src/desktop/agent_policy.cpp', 'src/desktop/agent_broker.cpp', 'src/desktop/agent_endpoint.cpp'), helix_desktop_introspection_h],
  dependencies: noctalia_core_dep,
  cpp_args: _noctalia_cpp_args,
  install: true,
)
install_data('protocols/ipc/msg-risk.toml', install_dir: get_option('datadir') / 'helix')
helix_service_config = configuration_data()
helix_service_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/org.helix.Desktop.service.in',
  output: 'org.helix.Desktop.service', configuration: helix_service_config,
  install_dir: get_option('datadir') / 'dbus-1' / 'services')
configure_file(input: 'assets/services/helix-service.service.in',
  output: 'helix-service.service', configuration: helix_service_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')

gtk4_dep = dependency('gtk4', version: '>=4.12', required: get_option('desktop_apps'))
adwaita_dep = dependency('libadwaita-1', version: '>=1.7', required: get_option('desktop_apps'))
if gtk4_dep.found() and adwaita_dep.found()
  goa_backend_dep = dependency('goa-backend-1.0', version: '>=3.56', required: false)
  settings_deps = [gtk4_dep, adwaita_dep, gio_dep, nlohmann_dep]
  settings_args = ['-Wall', '-Wextra', '-Wno-missing-field-initializers']
  if goa_backend_dep.found()
    settings_deps += goa_backend_dep
    settings_args += '-DHELIX_HAVE_GOA'
  endif
  helix_settings = executable('helix-settings',
    sources: files('src/apps/settings/main.cpp', 'src/apps/settings/wallpaper_catalog.cpp', 'src/desktop/protocol.cpp', 'src/desktop/client.cpp'),
    include_directories: include_directories('src'),
    dependencies: settings_deps,
    cpp_args: settings_args,
    install: true,
  )
  install_data('assets/org.helix.Settings.desktop', install_dir: get_option('datadir') / 'applications')
  helix_software = executable('helix-software',
    sources: files('src/apps/software/main.cpp'),
    include_directories: include_directories('src'),
    dependencies: [noctalia_core_dep, gtk4_dep, adwaita_dep, gio_dep],
    cpp_args: ['-Wall', '-Wextra', '-Wno-missing-field-initializers'],
    install: true,
  )
  install_data('assets/org.helix.Software.desktop', install_dir: get_option('datadir') / 'applications')


  helix_monitor = executable('helix-system-monitor',
    sources: files('src/apps/system_monitor/main.cpp', 'src/apps/system_monitor/proc_reader.cpp',
      'src/apps/system_monitor/report_reader.cpp', 'src/desktop/protocol.cpp'),
    include_directories: include_directories('src'),
    dependencies: [gtk4_dep, adwaita_dep, gio_dep, nlohmann_dep],
    cpp_args: ['-Wall', '-Wextra', '-Wno-missing-field-initializers'],
    install: true,
  )
  install_data('assets/org.helix.SystemMonitor.desktop', install_dir: get_option('datadir') / 'applications')
  install_data('assets/org.helix.SystemMonitor.svg', install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
endif

if build_tests
  helix_monitor_test = executable('desktop_monitor_test',
    sources: files('tests/desktop_monitor_test.cpp', 'src/apps/system_monitor/proc_reader.cpp'),
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('desktop_monitor', helix_monitor_test)
  helix_wallpaper_test = executable('desktop_wallpaper_catalog_test',
    sources: files('tests/desktop_wallpaper_catalog_test.cpp', 'src/apps/settings/wallpaper_catalog.cpp'),
    include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('desktop_wallpaper_catalog', helix_wallpaper_test)
  helix_wallhaven_test = executable('desktop_wallhaven_test',
    sources: files('tests/desktop_wallhaven_test.cpp'), dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'])
  test('desktop_wallhaven', helix_wallhaven_test)
  helix_monitor_reports_test = executable('desktop_monitor_reports_test',
    sources: files('tests/desktop_monitor_reports_test.cpp', 'src/apps/system_monitor/report_reader.cpp'),
    include_directories: include_directories('src'), dependencies: [nlohmann_dep], override_options: ['b_ndebug=false'])
  test('desktop_monitor_reports', helix_monitor_reports_test)
  helix_dns_test = executable('desktop_dns_validation_test',
    sources: files('tests/desktop_dns_validation_test.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_dns_validation', helix_dns_test)
  helix_meter_test = executable('desktop_input_meter_test',
    sources: files('tests/desktop_input_meter_test.cpp', 'src/desktop/input_meter.cpp'),
    include_directories: include_directories('src'), dependencies: [pipewire_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_input_meter', helix_meter_test)
  helix_protocol_test = executable('desktop_protocol_test',
    sources: files('tests/desktop_protocol_test.cpp', 'src/desktop/protocol.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_protocol', helix_protocol_test)
  helix_agent_policy_unit = executable('desktop_agent_policy_test',
    sources: files('tests/desktop_agent_policy_test.cpp', 'src/desktop/agent_policy.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('desktop_agent_policy', helix_agent_policy_unit, args: [meson.project_source_root()])
  helix_agent_broker_unit = executable('desktop_agent_broker_test',
    sources: files('tests/desktop_agent_broker_test.cpp', 'src/desktop/agent_broker.cpp', 'src/desktop/agent_policy.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('desktop_agent_broker', helix_agent_broker_unit)
  helix_confirmation_queue_unit = executable('agent_confirmation_queue_test',
    sources: files('tests/agent_confirmation_queue_test.cpp', 'src/shell/agent/agent_confirmation_queue.cpp'),
    dependencies: noctalia_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('agent_confirmation_queue', helix_confirmation_queue_unit)
  helix_displays_test = executable('desktop_displays_test',
    sources: files('tests/desktop_displays_test.cpp', 'src/system/display_config.cpp', 'src/desktop/protocol.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_displays', helix_displays_test, timeout: 30)

  helix_service_test = executable('desktop_service_test',
    sources: files('tests/desktop_service_test.cpp', 'src/desktop/protocol.cpp', 'src/desktop/client.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  helix_dbus_session = find_program('dbus-run-session', required: false)
  if helix_dbus_session.found()
    test('desktop_service', helix_dbus_session, args: [helix_service_test, helix_service], timeout: 60)
    meter_python = find_program('python3')
    if run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
      test('desktop_agent_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/desktop_agent_integration.py',
          helix_service, meson.project_source_root() / 'protocols/ipc/msg-risk.toml'], timeout: 90)
    endif
    _adapter_python = meson.project_source_root() / 'tools/helix-agent/.venv/bin/python'
    if run_command('test', '-x', _adapter_python, check: false).returncode() == 0 and run_command(_adapter_python, '-c', 'import gi, mcp', check: false).returncode() == 0
      test('helix_agent_adapter_integration', helix_dbus_session,
        args: [_adapter_python, meson.project_source_root() / 'tests/helix_agent_adapter_integration.py',
          meson.project_source_root(), helix_service], timeout: 120)
    endif
    meter_tools = [find_program('pipewire', required: false), find_program('pw-cli', required: false),
      find_program('pw-link', required: false), find_program('pw-dump', required: false)]
    have_meter_tools = true
    foreach tool : meter_tools
      have_meter_tools = have_meter_tools and tool.found()
    endforeach
    if have_meter_tools and run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
      test('desktop_audio_meter_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/desktop_audio_meter_integration.py',
          helix_service, meson.project_source_root() / 'tests/desktop_audio/pipewire.conf'], timeout: 60)
    endif
  endif
  _agent_python = meson.project_source_root() / 'tools/helix-agent/.venv/bin/python'
  if run_command('test', '-x', _agent_python, check: false).returncode() == 0
    test('helix_agent_policy', find_program(_agent_python),
      args: [meson.project_source_root() / 'tests/helix_agent_policy_test.py', meson.project_source_root()])
  endif
  test('msg_risk_policy', find_program('python3'),
    args: [meson.project_source_root() / 'tests/msg_risk_policy_test.py', meson.project_source_root(), noctalia_exe])
  test('desktop_dbus_contract', find_program('python3'),
    args: [meson.project_source_root() / 'tests/desktop_dbus_contract_test.py', meson.project_source_root()])
  test('desktop_migration_inventory', find_program('python3'),
    args: [meson.project_source_root() / 'tests/desktop_migration_test.py', meson.project_source_root()])
endif
