project('helix-shell', ['c', 'cpp'],
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
    jemalloc_cpp_args += ['-DHELIX_USE_JEMALLOC=1']
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
_helix_sources = files(
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
  'src/idle/idle_manager.cpp',
  'src/ipc/ipc_client.cpp',
  'src/ipc/ipc_service.cpp',
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
  'src/core/color.cpp',
  'src/core/image_decoder.cpp',
  'src/core/image_orientation.cpp',
  'src/security/encrypted_file_store.cpp',
  'src/security/secret_store.cpp',
  'src/security/secure_buffer.cpp',
  'src/security/storage_key_provider.cpp',
  'src/system/battery_warning_monitor.cpp',
  'src/config/widgets/desktop_widget_settings_registry.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/desktop_api/display_text.cpp',
  'src/desktop/agent_confirmation_queue.cpp',
  'src/assistant/assistant_session.cpp',
  'src/config/widgets/widget_settings_registry.cpp',
  'src/system/app_identity.cpp',
  'src/net/wallhaven_client.cpp',
  'src/config/widget_action.cpp',
  'src/config/widget_gesture.cpp',
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
  'src/system/rfkill_helper.cpp',
  'src/system/cpu_stat.cpp',
  'src/system/cpu_freq.cpp',
  'src/system/cpu_temp_sensor.cpp',
  'src/system/intel_gpu.cpp',
  'src/system/system_monitor_service.cpp',
  'src/time/time_format.cpp',
  'src/time/time_service.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/theme/color_roles.cpp',
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
  'src/wayland/output_probe.cpp',
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
  'src/theme/runtime_palette.cpp',
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
  'src/ipc/cli.cpp',)

_helix_sources += files('src/desktop_api/protocol.cpp', 'src/desktop_api/client.cpp', 'src/desktop_api/remote_network.cpp', 'src/desktop_api/peer_identity.cpp')

if sanitize
  _helix_sources += files('src/core/lsan_suppressions.cpp')
endif

git_program = find_program('git', required: false)
if git_program.found()
  _git_revision_h = vcs_tag(
    input: 'src/core/git_revision.h.in',
    output: 'helix_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  _git_revision_config = configuration_data()
  _git_revision_config.set('VCS_TAG', 'unknown')
  _git_revision_h = configure_file(
    input: 'src/core/git_revision.h.in',
    output: 'helix_git_revision.h',
    configuration: _git_revision_config,
  )
endif

# ── Include directories ────────────────────────────────────────────────────────
_helix_inc = [
  include_directories('src'),
  include_directories('third_party/wuffs', is_system: true),
  # '.' adds both the source root and the corresponding build-dir root, so
  # generated protocol headers (written to the build root) are found by #include.
  include_directories('.'),
]

# ── Application library and executable ────────────────────────────────────────
_helix_system_dependencies = [
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

_helix_dependencies = _helix_system_dependencies + [
  fzy_dep,
  mcu_dep,
  qrcodegen_dep,
]

_helix_test_dependencies = _helix_system_dependencies + [
  _fzy_selective_dep,
  _mcu_selective_dep,
]

helix_system_upgrade_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'helix-system-upgrade')

_helix_compile_definitions = [
  '-DHELIX_INSTALL_PREFIX="' + get_option('prefix') + '"',
  '-DHELIX_INSTALL_DATADIR="' + get_option('datadir') + '"',
  '-DHELIX_VERSION="' + meson.project_version() + '"',
  '-DHELIX_SYSTEM_UPGRADE_PATH="' + helix_system_upgrade_path + '"',
] + jemalloc_cpp_args

_helix_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
] + _helix_compile_definitions

# Production sources and generated protocols compile once. The shell links the
# archive whole; tests link it normally so only the objects they exercise enter
# each test executable.
helix_core_lib = static_library('helix_core',
  sources: _helix_sources + _protocol_sources + _protocol_headers + [_git_revision_h],
  dependencies: _helix_dependencies,
  include_directories: _helix_inc,
  cpp_args: _helix_cpp_args,
)

helix_core_dep = declare_dependency(
  link_with: helix_core_lib,
  dependencies: _helix_test_dependencies,
  include_directories: _helix_inc,
  compile_args: _helix_compile_definitions,
  sources: _protocol_headers,
)

# C++ session binary is still built (tests / reference). Install only when explicitly enabled —
# the live session shell is Tauri (`tools/switch-shell.sh install`).
# Retired C++ session binary removed; live shell is shell/helix-shell-tauri.

install_data(
  'assets/helix.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# Helix Phone: BLE HID pointer for iPhone AssistiveTouch and its bluetoothd setup helper.
install_data(
  install_dir: get_option('datadir') / 'helix' / 'scripts',
  install_mode: 'rwxr-xr-x',
)

executable('helix-system-upgrade',
  sources: files(
    'src/system/fedora_upgrade.cpp',
    'src/system/fedora_upgrade_main.cpp',
  ),
  include_directories: include_directories('src'),
  cpp_args: ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow'],
  install: true,
  install_dir: get_option('libexecdir'),
)

helix_upgrade_policy_conf = configuration_data()
helix_upgrade_policy_conf.set('HELPER_PATH', helix_system_upgrade_path)
configure_file(
  input: 'assets/polkit/org.helix.Shell.upgrade.policy.in',
  output: 'org.helix.Shell.upgrade.policy',
  configuration: helix_upgrade_policy_conf,
  install: true,
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)

if build_tests
  _cpp_test_names = [
    'adw_gtk3',
    'app_identity',
    'audio_glyphs',
    'audio_route_selection',
    'battery_health',
    'battery_hook_state',
    'cli_help',
    'cli_parse',
    'cli_schema',
    'config_migration',
    'config_override_mutation',
    'config_path_resolution',
    'config_schema_roundtrip',
    'config_wallpaper_precedence',
    'config_widget',
    'cpu_freq',
    'cpu_stat',
    'cpu_temp_sensor',
    'desktop_entry_launch',
    'disk_mounts',
    'display_config',
    'enterprise_credentials',
    'fcitx_status',
    'fedora_upgrade',
    'freedesktop_key_file',
    'hook_manager',
    'i18n_language_tag',
    'i18n_supported_languages',
    'inotify',
    'ipc_service',
    'kde_color_scheme',
    'log',
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
    'process',
    'scheme',
    'secret_store',
    'security_primitives',
    'state_store',
    'string_utils',
    'system_monitor_service',
    'template_apply_notify',
    'template_hook_async',
    'template_hook_runner',
    'theme_shell_mode',
    'time_format',
    'upower_device_catalog',
    'wallhaven_client',
    'workspace_alert_service',
    'capsule_group_lane_override',
    'capsule_group_reconcile',
    'clipboard_service',
    'clipboard_storage_permissions',
    'ico_decoder',
    'icon_resolver',
    'jxl_decoder',
    'monitor_selector',
    'popup_dismiss_order',
    'wayland_output_scale',
    'wayland_toplevels_identity',
    'webp_decoder',
  ]

  foreach test_name : _cpp_test_names
    test_exe = executable(test_name + '_test',
      sources: files('tests' / (test_name + '_test.cpp')),
      dependencies: helix_core_dep,
      override_options: ['b_ndebug=false'],
    )
    test_args = []
    if test_name == 'i18n_supported_languages'
      test_args = [meson.project_source_root() / 'assets' / 'translations']
    endif
    test(test_name, test_exe, args: test_args)
  endforeach

  inference_routing_test_exe = executable('inference_routing_test',
    sources: files('tests/inference_routing_test.cpp', 'src/inference/routing.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('inference_routing', inference_routing_test_exe, args: [meson.project_source_root()])

  config_validate_env = environment()
  config_validate_env.set('HELIX_CONFIG_HOME', meson.current_source_dir() / 'tests/config_validate/config-home')
  config_validate_env.set('HELIX_STATE_HOME', meson.current_source_dir() / 'tests/config_validate/state-home')
  config_validate_env.set('HELIX_DATA_HOME', meson.current_source_dir() / 'tests/config_validate/data-home')

  sh_program = find_program('sh')
  # config_validate_cli retired with helix-shell binary

  bash_program = find_program('bash')
  test('template_undo_signal',
    bash_program,
    args: ['tests/template_undo_signal_test.sh', meson.current_source_dir() / 'assets/templates'],
    workdir: meson.current_source_dir(),
  )
endif # build_tests


# Inference engine (llama.cpp), built by hand into third_party/llama-install; see docs/design/agent-and-inference.md.
# Optional: without it the service still builds, with only the scripted backend.
llama_prefix = meson.project_source_root() / 'third_party/llama-install'
llama_dep = dependency('', required: false)
if run_command('test', '-e', llama_prefix / 'lib64/libllama.so', check: false).returncode() == 0
  llama_dep = declare_dependency(
    include_directories: include_directories('third_party/llama-install/include', is_system: true),
    link_args: ['-L' + llama_prefix / 'lib64', '-lllama', '-lmtmd', '-lggml', '-lggml-base'],
    compile_args: ['-DHELIX_HAVE_LLAMA'],
  )
endif

# Independent desktop service and adaptive applications. GTK never enters shell dependencies.
# D-Bus contract: protocols/dbus/*.xml is the single source of truth for interface shapes.
helix_desktop_introspection_h = custom_target('helix-desktop-introspection',
  input: 'protocols/dbus/org.helix.Desktop.xml',
  output: 'helix_desktop_introspection.h',
  command: [find_program('python3'), files('tools/scripts/embed_text.py'), '@INPUT@', '@OUTPUT@', 'desktopIntrospectionXml'],
)

helix_service = executable('helix-service',
  sources: [files('src/desktop/service_main.cpp', 'src/desktop/settings_catalog.cpp', 'src/desktop/input_meter.cpp', 'src/desktop/agent_policy.cpp', 'src/desktop/agent_broker.cpp', 'src/desktop/agent_endpoint.cpp', 'src/desktop/shell_tools.cpp', 'src/inference/routing.cpp'), helix_desktop_introspection_h],
  dependencies: helix_core_dep,
  cpp_args: _helix_cpp_args,
  install: true,
)
executable('helix-template-apply',
  sources: files('src/theme/template_apply_main.cpp'),
  dependencies: helix_core_dep,
  cpp_args: _helix_cpp_args,
  install: true,
)
executable('helix-theme-palette',
  sources: files('src/theme/theme_palette_main.cpp'),
  dependencies: helix_core_dep,
  cpp_args: _helix_cpp_args,
  install: true,
)
helix_assistant_config = configuration_data()
helix_assistant_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/helix-assistant-warm.service.in',
  output: 'helix-assistant-warm.service', configuration: helix_assistant_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
helix_assistant = executable('helix-assistant',
  sources: files('src/assistant/assistant_main.cpp', 'src/assistant/assistant_core.cpp', 'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp', 'src/desktop/shell_tools.cpp',
    'src/inference/routing.cpp'),
  dependencies: helix_core_dep,
  cpp_args: _helix_cpp_args,
  install: true,
)
helix_assistant_unit = executable('assistant_core_test',
  sources: files('tests/assistant_core_test.cpp', 'src/assistant/assistant_core.cpp', 'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp'),
  dependencies: helix_core_dep,
  cpp_args: _helix_cpp_args,
)
test('assistant_core', helix_assistant_unit, args: [meson.project_source_root()])
install_data('protocols/ipc/msg-risk.toml', install_dir: get_option('datadir') / 'helix')
install_data('protocols/ipc/tool-hints.toml', install_dir: get_option('datadir') / 'helix')
helix_service_config = configuration_data()
helix_service_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/org.helix.Desktop.service.in',
  output: 'org.helix.Desktop.service', configuration: helix_service_config,
  install_dir: get_option('datadir') / 'dbus-1' / 'services')
configure_file(input: 'assets/services/helix-service.service.in',
  output: 'helix-service.service', configuration: helix_service_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')

# The model runtime: its own process, its own bus name (org.helix.Inference), sandboxed by its unit.
image_worker_sources = files('src/inference/image_generator.cpp', 'src/inference/image_input.cpp')
image_png_dep = dependency('libpng')
helix_inference_introspection_h = custom_target('helix-inference-introspection',
  input: 'protocols/dbus/org.helix.Inference.xml',
  output: 'helix_inference_introspection.h',
  command: [find_program('python3'), files('tools/scripts/embed_text.py'), '@INPUT@', '@OUTPUT@', 'inferenceIntrospectionXml'],
)
helix_inference = executable('helix-inference',
  sources: [files('src/inference/service_main.cpp', 'src/inference/inference_service.cpp', 'src/inference/inference_queue.cpp',
    'src/inference/schema_grammar.cpp', 'src/inference/routing.cpp', 'src/inference/image_input.cpp',
    'src/inference/transcriber.cpp', 'src/inference/image_generator.cpp'), helix_inference_introspection_h]
    + (llama_dep.found() ? files('src/inference/llama_backend.cpp') : []),
  dependencies: [helix_core_dep, dependency('threads'), image_png_dep] + (llama_dep.found() ? [llama_dep] : []),
  build_rpath: llama_prefix / 'lib64',  # development: the engine is built into third_party/
  install_rpath: get_option('prefix') / 'lib64' / 'helix',  # installed: the private copy made below
  cpp_args: _helix_cpp_args,
  install: true,
  install_tag: 'inference',
)
if llama_dep.found()
  meson.add_install_script(find_program('python3'), files('tools/scripts/install_engine.py'), llama_prefix / 'lib64', 'lib64/helix', install_tag: 'inference')
endif

# Speech to text: whisper.cpp (built by hand into third_party/whisper-install, CPU only) in a worker process of its own, because it
# carries its own ggml and because decoding untrusted audio must not be able to take the chat service down.
whisper_prefix = meson.project_source_root() / 'third_party/whisper-install'
if run_command('test', '-e', whisper_prefix / 'lib64/libwhisper.a', check: false).returncode() == 0
  helix_transcribe = executable('helix-transcribe',
    sources: files('src/inference/transcribe_worker.cpp'),
    include_directories: include_directories('third_party/whisper-install/include', is_system: true),
    dependencies: [helix_core_dep, dependency('threads'), dependency('openmp', required: false)],
    link_args: ['-L' + whisper_prefix / 'lib64', '-Wl,--start-group', '-lwhisper', '-lggml', '-lggml-base', '-lggml-cpu', '-Wl,--end-group', '-fopenmp'],
    cpp_args: _helix_cpp_args,
    install: true,
    install_tag: 'inference',
  )
endif

helix_inference_config = configuration_data()
sd_prefix = meson.project_source_root() / 'third_party/sd-install'
if run_command('test', '-e', sd_prefix / 'lib64/libstable-diffusion.a', check: false).returncode() == 0
  sd_link_args = ['-L' + sd_prefix / 'lib64', '-Wl,--start-group', '-lstable-diffusion', '-lggml', '-lggml-base', '-lggml-cpu']
  if run_command('test', '-e', sd_prefix / 'lib64/libggml-vulkan.a', check: false).returncode() == 0
    sd_link_args += ['-lggml-vulkan', '-lvulkan']
  endif
  sd_link_args += ['-Wl,--end-group', '-fopenmp', '-ldl']
  helix_image = executable('helix-image',
    sources: files('src/inference/image_worker.cpp') + image_worker_sources,
    include_directories: include_directories('third_party/sd-install/include', is_system: true),
    dependencies: [helix_core_dep, dependency('threads'), image_png_dep, dependency('openmp', required: false)],
    link_args: sd_link_args, cpp_args: _helix_cpp_args,
    install: true, install_tag: 'inference',
  )
endif
helix_inference_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/org.helix.Inference.service.in',
  output: 'org.helix.Inference.service', configuration: helix_inference_config,
  install_dir: get_option('datadir') / 'dbus-1' / 'services')
configure_file(input: 'assets/services/helix-inference.service.in',
  output: 'helix-inference.service', configuration: helix_inference_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')

# GTK desktop apps retired; Tauri apps under apps/.

if build_tests
  helix_wallhaven_test = executable('desktop_wallhaven_test',
    sources: files('tests/desktop_wallhaven_test.cpp'), dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'])
  test('desktop_wallhaven', helix_wallhaven_test)
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
    sources: files('tests/desktop_protocol_test.cpp', 'src/desktop_api/protocol.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_protocol', helix_protocol_test)
  helix_agent_policy_unit = executable('desktop_agent_policy_test',
    sources: files('tests/desktop_agent_policy_test.cpp', 'src/desktop/agent_policy.cpp', 'src/desktop_api/peer_identity.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('desktop_agent_policy', helix_agent_policy_unit, args: [meson.project_source_root()])
  helix_agent_broker_unit = executable('desktop_agent_broker_test',
    sources: files('tests/desktop_agent_broker_test.cpp', 'src/desktop/agent_broker.cpp', 'src/desktop/agent_policy.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('desktop_agent_broker', helix_agent_broker_unit)
  helix_tool_schema_unit = executable('desktop_tool_schema_test',
    sources: files('tests/desktop_tool_schema_test.cpp', 'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('desktop_tool_schema', helix_tool_schema_unit, args: [meson.project_source_root()])
  helix_inference_queue_unit = executable('inference_queue_test',
    sources: files('tests/inference_queue_test.cpp', 'src/inference/inference_queue.cpp') + image_worker_sources,
    dependencies: [helix_core_dep, dependency('threads'), image_png_dep],
    override_options: ['b_ndebug=false'],
  )
  test('inference_queue', helix_inference_queue_unit, timeout: 60)
  helix_image_generation_unit = executable('inference_image_generation_test',
    sources: files('tests/inference_image_generation_test.cpp', 'src/inference/inference_queue.cpp') + image_worker_sources,
    dependencies: [helix_core_dep, dependency('threads'), image_png_dep], override_options: ['b_ndebug=false'],
  )
  test('inference_image_generation', helix_image_generation_unit, timeout: 60)
  helix_image_input_unit = executable('inference_image_input_test',
    sources: files('tests/inference_image_input_test.cpp', 'src/inference/image_input.cpp'),
    dependencies: [helix_core_dep, dependency('threads')],
    override_options: ['b_ndebug=false'],
  )
  test('inference_image_input', helix_image_input_unit, timeout: 60)
  helix_schema_grammar_unit = executable('inference_schema_grammar_test',
    sources: files('tests/inference_schema_grammar_test.cpp', 'src/inference/schema_grammar.cpp',
      'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('inference_schema_grammar', helix_schema_grammar_unit, args: [meson.project_source_root()])
  if llama_dep.found()
    helix_llama_test = executable('inference_llama_test',
      sources: files('tests/inference_llama_test.cpp', 'src/inference/llama_backend.cpp', 'src/inference/inference_queue.cpp',
        'src/inference/schema_grammar.cpp', 'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp') + image_worker_sources,
      dependencies: [helix_core_dep, llama_dep, dependency('threads'), image_png_dep],
      build_rpath: llama_prefix / 'lib64',  # development: the engine is built into third_party/
      override_options: ['b_ndebug=false'],
    )
    # Evaluation harness (not a test: it takes minutes). Run it with tools/run-inference-eval.py.
    executable('inference_eval',
      sources: files('tests/inference_eval.cpp', 'src/inference/llama_backend.cpp', 'src/inference/inference_queue.cpp',
        'src/inference/schema_grammar.cpp', 'src/desktop/tool_schema.cpp', 'src/desktop/agent_policy.cpp') + image_worker_sources,
      dependencies: [helix_core_dep, llama_dep, dependency('threads'), image_png_dep],
      build_rpath: llama_prefix / 'lib64',
      override_options: ['b_ndebug=false'],
    )
    _model_path = run_command('sh', '-c', 'echo "$HOME/.local/share/helix/models/qwen2.5-1.5b-instruct-q4_k_m.gguf"', check: false).stdout().strip()
    test('inference_llama', helix_llama_test, timeout: 600, is_parallel: false, args: [meson.project_source_root(), _model_path])
    _model_path_7b = run_command('sh', '-c', 'echo "$HOME/.local/share/helix/models/qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"', check: false).stdout().strip()
    test('inference_llama_7b', helix_llama_test, is_parallel: false, timeout: 900, args: [meson.project_source_root(), _model_path_7b])
  endif
  helix_assistant_session_unit = executable('assistant_session_test',
    sources: files('tests/assistant_session_test.cpp', 'src/assistant/assistant_session.cpp', 'src/desktop_api/display_text.cpp'),
    dependencies: helix_core_dep,
    cpp_args: _helix_cpp_args,
  )
  test('assistant_session', helix_assistant_session_unit)
  helix_confirmation_queue_unit = executable('agent_confirmation_queue_test',
    sources: files('tests/agent_confirmation_queue_test.cpp', 'src/desktop/agent_confirmation_queue.cpp', 'src/desktop_api/display_text.cpp'),
    dependencies: helix_core_dep,
    override_options: ['b_ndebug=false'],
  )
  test('agent_confirmation_queue', helix_confirmation_queue_unit)
  helix_displays_test = executable('desktop_displays_test',
    sources: files('tests/desktop_displays_test.cpp', 'src/system/display_config.cpp', 'src/desktop_api/protocol.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  test('desktop_displays', helix_displays_test, timeout: 30)

  helix_service_test = executable('desktop_service_test',
    sources: files('tests/desktop_service_test.cpp', 'src/desktop_api/protocol.cpp', 'src/desktop_api/client.cpp'),
    include_directories: include_directories('src'), dependencies: [gio_dep, nlohmann_dep],
    override_options: ['b_ndebug=false'])
  helix_dbus_session = find_program('dbus-run-session', required: false)
  if helix_dbus_session.found()
    test('desktop_service', helix_dbus_session, args: [helix_service_test, helix_service], timeout: 60)
    meter_python = find_program('python3')
    if run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
      test('inference_service_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/inference_service_integration.py', helix_inference], timeout: 120)
      test('assistant_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/assistant_integration.py', helix_assistant, helix_service, helix_inference,
          meson.project_source_root()], timeout: 120)
      test('desktop_agent_manager_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/desktop_agent_manager_integration.py',
          helix_service, meson.project_source_root() / 'protocols/ipc/msg-risk.toml'], timeout: 90)
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
    _helix_desktop_rust_fast = meson.project_source_root() / 'target' / 'fast' / 'helix-desktop'
    _helix_desktop_rust_debug = meson.project_source_root() / 'target' / 'debug' / 'helix-desktop'
    if run_command('test', '-x', _helix_desktop_rust_fast, check: false).returncode() == 0
      _helix_desktop_rust = _helix_desktop_rust_fast
    else
      _helix_desktop_rust = _helix_desktop_rust_debug
    endif
    _config_smoke = meson.project_source_root() / 'tools' / 'nested' / 'helix-desktop-config-smoke.sh'
    _have_config_smoke = run_command('test', '-x', _helix_desktop_rust, check: false).returncode() == 0 and run_command('test', '-x', _config_smoke, check: false).returncode() == 0 and run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
    if _have_config_smoke
      _config_smoke_env = ['env', 'HELIX_SERVICE_BIN=' + _helix_desktop_rust, 'HELIX_ASSETS_DIR=' + (meson.project_source_root() / 'assets')]
      _template_apply = meson.project_source_root() / '_build' / 'helix-template-apply'
      if run_command('test', '-x', _template_apply, check: false).returncode() == 0
        _config_smoke_env += ['HELIX_TEMPLATE_APPLY_BIN=' + _template_apply]
      endif
      _config_smoke_env += [_config_smoke]
      test('helix_desktop_config_smoke', helix_dbus_session,
        args: _config_smoke_env, timeout: 120)
    endif
  endif
  _agent_python = meson.project_source_root() / 'tools/helix-agent/.venv/bin/python'
  if run_command('test', '-x', _agent_python, check: false).returncode() == 0
    test('helix_agent_policy', find_program(_agent_python),
      args: [meson.project_source_root() / 'tests/helix_agent_policy_test.py', meson.project_source_root()])
  endif
  test('layering', find_program('python3'),
    args: [meson.project_source_root() / 'tools/check-layers.py', meson.project_source_root()])
  # msg_risk_policy CLI host retired with helix-shell binary
  test('desktop_dbus_contract', find_program('python3'),
    args: [meson.project_source_root() / 'tests/desktop_dbus_contract_test.py', meson.project_source_root()])
  test('desktop_migration_inventory', find_program('python3'),
    args: [meson.project_source_root() / 'tests/desktop_migration_test.py', meson.project_source_root()])
endif
