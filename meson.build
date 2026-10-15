project('helix-shell', 'c',
  version: '5.1.0',
  default_options: [
    'warning_level=0',
    'b_ndebug=if-release',
  ],
)

# Sanitizers are enabled via meson's built-in -Db_sanitize=address,undefined.
sanitize = get_option('b_sanitize') != 'none'

# Auto mode defines tests for unsanitized debug builds.
tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') == 'debug' and not sanitize)

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-ffunction-sections', '-fdata-sections',
    language: ['c'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c'],
  )
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: ['c'])
endif

# Platform C++ (`helix_core`) is retired. Config, theme, desktop, and inference are Cargo.
# Feature options stay so existing `-Djemalloc` / `-Dnative_optimizations` lines still configure.
summary({
  'jemalloc': get_option('jemalloc'),
  'native_optimizations': get_option('native_optimizations'),
}, section: 'Retired with the C++ library')

helix_system_upgrade_path = join_paths(get_option('prefix'), get_option('libexecdir'), 'helix-system-upgrade')

# C++ session binary is retired. The live session shell is Tauri (`tools/switch-shell.sh install`).

install_data(
  'assets/helix.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

# Helix Phone: BLE HID pointer for iPhone AssistiveTouch and its bluetoothd setup helper.
install_data(
  install_dir: get_option('datadir') / 'helix' / 'scripts',
  install_mode: 'rwxr-xr-x',
)

# helix-system-upgrade (the pkexec helper the polkit policy below points at) is Rust: services/helix-system-upgrade;
# `just install` puts it in libexecdir.
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

# Template golden regen: `cargo run -p helix-theme --bin gen-golden-template` (see tools/regen_golden_template.sh).

helix_assistant_config = configuration_data()
helix_assistant_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/helix-assistant-warm.service.in',
  output: 'helix-assistant-warm.service', configuration: helix_assistant_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
# helix-assistant is Rust (services/helix-assistant); `just install` installs it next to these units.
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

# Inference workers are Cargo (`helix-inference`, `helix-transcribe`, `helix-image`). Meson only installs the llama engine libs.
if llama_dep.found()
  meson.add_install_script(find_program('python3'), files('tools/scripts/install_engine.py'), llama_prefix / 'lib64', 'lib64/helix', install_tag: 'inference')
endif

helix_inference_config = configuration_data()
helix_inference_config.set('HELIX_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'assets/services/org.helix.Inference.service.in',
  output: 'org.helix.Inference.service', configuration: helix_inference_config,
  install_dir: get_option('datadir') / 'dbus-1' / 'services')
configure_file(input: 'assets/services/helix-inference.service.in',
  output: 'helix-inference.service', configuration: helix_inference_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')

# GTK desktop apps retired; Tauri apps under apps/.

if build_tests
  # Agent policy, inference workers, and the desktop bus are Rust. The llama shim
  # is compiled by services/helix-inference/build.rs when llama-install is present.
  helix_dbus_session = find_program('dbus-run-session', required: false)
  if helix_dbus_session.found()
    _helix_desktop_rust_fast = meson.project_source_root() / 'target' / 'fast' / 'helix-desktop'
    _helix_desktop_rust_debug = meson.project_source_root() / 'target' / 'debug' / 'helix-desktop'
    if run_command('test', '-x', _helix_desktop_rust_fast, check: false).returncode() == 0
      _helix_desktop_rust = _helix_desktop_rust_fast
    else
      _helix_desktop_rust = _helix_desktop_rust_debug
    endif
    _helix_assistant_rust = meson.project_source_root() / 'target' / 'fast' / 'helix-assistant'
    if run_command('test', '-x', _helix_assistant_rust, check: false).returncode() != 0
      _helix_assistant_rust = meson.project_source_root() / 'target' / 'debug' / 'helix-assistant'
    endif
    _have_rust_assistant = run_command('test', '-x', _helix_assistant_rust, check: false).returncode() == 0
    _helix_inference_rust = meson.project_source_root() / 'target' / 'fast' / 'helix-inference'
    if run_command('test', '-x', _helix_inference_rust, check: false).returncode() != 0
      _helix_inference_rust = meson.project_source_root() / 'target' / 'debug' / 'helix-inference'
    endif
    _have_rust_inference = run_command('test', '-x', _helix_inference_rust, check: false).returncode() == 0
    helix_service = _helix_desktop_rust  # Rust org.helix.Desktop (cargo build -p helix-desktop [--profile fast])
    _have_rust_desktop = run_command('test', '-x', _helix_desktop_rust, check: false).returncode() == 0
    meter_python = find_program('python3')
    if run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
      if _have_rust_inference
        test('inference_service_integration', helix_dbus_session,
          args: [meter_python, meson.project_source_root() / 'tests/inference_service_integration.py', _helix_inference_rust], timeout: 300, is_parallel: false)
      endif
      if _have_rust_desktop
        if _have_rust_assistant and _have_rust_inference
          test('assistant_integration', helix_dbus_session,
            args: [meter_python, meson.project_source_root() / 'tests/assistant_integration.py', _helix_assistant_rust, helix_service, _helix_inference_rust,
              meson.project_source_root()], timeout: 120)
        endif
        test('desktop_agent_manager_integration', helix_dbus_session,
          args: [meter_python, meson.project_source_root() / 'tests/desktop_agent_manager_integration.py',
            helix_service, meson.project_source_root() / 'protocols/ipc/msg-risk.toml'], timeout: 90)
        test('desktop_agent_integration', helix_dbus_session,
          args: [meter_python, meson.project_source_root() / 'tests/desktop_agent_integration.py',
            helix_service, meson.project_source_root() / 'protocols/ipc/msg-risk.toml'], timeout: 90)
      endif
    endif
    _adapter_python = meson.project_source_root() / 'tools/helix-agent/.venv/bin/python'
    if _have_rust_desktop and run_command('test', '-x', _adapter_python, check: false).returncode() == 0 and run_command(_adapter_python, '-c', 'import gi, mcp', check: false).returncode() == 0
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
    if _have_rust_desktop and have_meter_tools and run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
      test('desktop_audio_meter_integration', helix_dbus_session,
        args: [meter_python, meson.project_source_root() / 'tests/desktop_audio_meter_integration.py',
          helix_service, meson.project_source_root() / 'tests/desktop_audio/pipewire.conf'], timeout: 60)
    endif
    _config_smoke = meson.project_source_root() / 'tools' / 'nested' / 'helix-desktop-config-smoke.sh'
    _have_config_smoke = run_command('test', '-x', _helix_desktop_rust, check: false).returncode() == 0 and run_command('test', '-x', _config_smoke, check: false).returncode() == 0 and run_command(meter_python, '-c', 'from gi.repository import Gio, GLib', check: false).returncode() == 0
    if _have_config_smoke
      _config_smoke_env = [
        'env',
        'HELIX_SERVICE_BIN=' + _helix_desktop_rust,
        'HELIX_ASSETS_DIR=' + (meson.project_source_root() / 'assets'),
        _config_smoke,
      ]
      test('helix_desktop_config_smoke', helix_dbus_session,
        args: _config_smoke_env, timeout: 120)
    endif
    _config_contract = meson.project_source_root() / 'tools' / 'nested' / 'helix-desktop-configuration-contract.sh'
    if _have_rust_desktop and run_command('test', '-x', _config_contract, check: false).returncode() == 0
      test('helix_desktop_configuration_contract', helix_dbus_session,
        args: [_config_contract], timeout: 180)
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
endif
