project('noctalia-files', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=3',
    'werror=true',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')
scanner = find_program('wayland-scanner')
protocol_root = wayland_protocols.get_variable('pkgdatadir')
protocol_sources = []
protocol_headers = []
foreach protocol : [
  ['xdg-shell', 'stable/xdg-shell/xdg-shell.xml'],
  ['viewporter', 'stable/viewporter/viewporter.xml'],
  ['fractional-scale-v1', 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['tablet-v2', 'stable/tablet/tablet-v2.xml'],
  ['cursor-shape-v1', 'staging/cursor-shape/cursor-shape-v1.xml'],
  ['text-input-unstable-v3', 'unstable/text-input/text-input-unstable-v3.xml'],
  ['xdg-decoration-unstable-v1', 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
]
  header = custom_target(protocol[0] + '-header',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  source = custom_target(protocol[0] + '-source',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
  protocol_sources += source
  protocol_headers += header
endforeach
alias_target('generated-headers', protocol_headers)

gio = dependency('gio-2.0', version: '>=2.76')
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')
ui_dependencies = [
  gio,
  dependency('wayland-client'), dependency('wayland-egl'), dependency('wayland-cursor'),
  dependency('egl'), dependency('glesv2'), dependency('xkbcommon'),
  dependency('freetype2'), dependency('fontconfig'),
  dependency('cairo'), dependency('cairo-ft'),
  dependency('pango', version: '>=1.50'), dependency('pangocairo'), dependency('pangoft2'),
  dependency('harfbuzz'), dependency('libpng'), dependency('librsvg-2.0'), dependency('nlohmann_json'),
  dependency('threads'),
]
if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: 'cpp')
endif

if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
  add_project_link_arguments('-Wl,--gc-sections', '-Wl,--as-needed', language: 'cpp')
else
  add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
endif

build_config = configuration_data()
build_config.set_quoted('NOCTALIA_FILES_VERSION', meson.project_version())
build_config.set_quoted('NOCTALIA_FILES_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('NOCTALIA_FILES_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'noctalia-files/assets')
build_info = configure_file(
  input: 'src/core/build_info.h.in',
  output: 'noctalia_files_build_info.h',
  configuration: build_config,
)
fs = import('fs')
completion_config = configuration_data()
completion_config.set('BASH_COMPLETIONS', fs.read('assets/completions/noctalia-files.bash'))
completion_config.set('ZSH_COMPLETIONS', fs.read('assets/completions/_noctalia-files'))
completion_config.set('FISH_COMPLETIONS', fs.read('assets/completions/noctalia-files.fish'))
completions = configure_file(
  input: 'src/app/completion_scripts.h.in',
  output: 'noctalia_files_completions.h',
  configuration: completion_config,
)
app_sources = files(
  'src/app/application.cpp',
  'src/app/cli.cpp',
  'src/app/main_loop.cpp',
  'src/browser/file_view.cpp',
  'src/core/deferred_call.cpp',
  'src/core/files/resource_paths.cpp',
  'src/core/input/key_chord.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/core/log.cpp',
  'src/core/inotify/inotify.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/freedesktop_key_file.cpp',
  'src/system/icon_theme_poll_source.cpp',
  'src/browser/file_icon_cache.cpp',
  'src/browser/file_icon.cpp',
  'src/config/view_preference_writer.cpp',
  'src/platform/file_launcher.cpp',
  'src/platform/text_clipboard.cpp',
  'src/platform/terminal_launcher.cpp',
  'src/browser/places_sidebar.cpp',
  'src/ui/controls/name_editor.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/util/clamp.cpp',
  'src/main.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/color.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/node.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/font_registry.cpp',
  'src/render/text/font_weight_catalog.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/ui/builders.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/selection_surface.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/input.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/roving_list_nav.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/palette.cpp',
  'src/ui/scroll_into_view.cpp',
  'src/ui/style.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/text_input_service.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_seat.cpp',
)

filesystem_service = declare_dependency(
  sources: files(
    'src/fs/location.cpp',
    'src/fs/filename.cpp',
    'src/fs/file_metadata.cpp',
    'src/fs/trash_info.cpp',
    'src/fs/file_icon_source.cpp',
    'src/fs/directory_session.cpp',
    'src/fs/recursive_search.cpp',
    'src/app/glib_poll_source.cpp',
  ),
  include_directories: include_directories('src'),
  dependencies: gio,
)

browser_model = declare_dependency(
  sources: files('src/browser/directory_model.cpp', 'src/browser/browser_controller.cpp', 'src/browser/location_presentation.cpp', 'src/browser/path_completion.cpp', 'src/browser/action_registry.cpp', 'src/browser/selection_tools.cpp'),
  dependencies: filesystem_service,
)

persistence_core = declare_dependency(
  sources: files('src/config/atomic_file.cpp', 'src/config/diagnostic.cpp'),
  include_directories: include_directories('src'),
)

settings_service = declare_dependency(
  sources: files('src/config/settings.cpp'),
  dependencies: [persistence_core, tomlplusplus],
)

state_service = declare_dependency(
  sources: files('src/config/state_store.cpp'),
  dependencies: [persistence_core, filesystem_service, tomlplusplus],
)

places_service = declare_dependency(
  sources: files('src/browser/places.cpp'),
  dependencies: state_service,
)

create_service = declare_dependency(
  sources: files('src/operations/create_service.cpp'),
  dependencies: filesystem_service,
)

rename_service = declare_dependency(
  sources: files('src/operations/rename_service.cpp'),
  dependencies: filesystem_service,
)

trash_service = declare_dependency(
  sources: files('src/operations/trash_service.cpp', 'src/operations/empty_trash_service.cpp'),
  dependencies: filesystem_service,
)

transfer_service = declare_dependency(
  sources: files('src/operations/transfer_service.cpp'),
  dependencies: filesystem_service,
)

app = executable('noctalia-files',
  app_sources,
  protocol_sources,
  protocol_headers,
  build_info,
  completions,
  include_directories: include_directories('src', '.'),
  dependencies: ui_dependencies + [browser_model, settings_service, state_service, places_service, transfer_service, rename_service, create_service, trash_service],
  install: true,
)

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  python = import('python').find_installation('python3')
  actions_test = executable('browser-actions-test', 'tests/browser_actions_test.cpp', dependencies: browser_model)
  test('browser-actions', actions_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  empty_trash_test = executable('empty-trash-service-test', 'tests/empty_trash_service_test.cpp', dependencies: trash_service)
  test('empty-trash-service', empty_trash_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  trash_browse_test = executable('trash-browse-test', 'tests/trash_browse_test.cpp', dependencies: browser_model)
  test('trash-browse', trash_browse_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  trash_test = executable('trash-service-test', 'tests/trash_service_test.cpp', dependencies: [trash_service, browser_model])
  test('trash-service', trash_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  create_test = executable('create-service-test', 'tests/create_service_test.cpp', dependencies: [create_service, browser_model])
  test('create-service', create_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  rename_test = executable('rename-service-test', 'tests/rename_service_test.cpp', dependencies: [rename_service, browser_model])
  test('rename-service', rename_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  transfer_test = executable('transfer-service-test', 'tests/transfer_service_test.cpp', dependencies: transfer_service)
  test('transfer-service', transfer_test, timeout: 90, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  places_test = executable('places-test', 'tests/places_test.cpp', dependencies: places_service)
  test('places', places_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  icon_test = executable('file-icon-test',
    'tests/file_icon_test.cpp',
    'src/browser/file_icon_cache.cpp',
    'src/system/icon_resolver.cpp',
    'src/system/freedesktop_key_file.cpp',
    'src/render/core/image_file_loader.cpp',
    'src/render/core/image_decoder.cpp',
    'src/core/log.cpp',
    dependencies: ui_dependencies + [filesystem_service],
  )
  test('file-icon', icon_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  view_writer_test = executable('view-preference-writer-test',
    'tests/view_preference_writer_test.cpp',
    'src/config/view_preference_writer.cpp',
    dependencies: [settings_service, filesystem_service],
  )
  test('view-preference-writer', view_writer_test, timeout: 30, env: ['G_DEBUG=fatal-warnings'])
  directory_test = executable('directory-session-test',
    'tests/directory_session_test.cpp',
    include_directories: include_directories('src'),
    dependencies: filesystem_service,
  )
  test('directory-session', directory_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  browser_test = executable('directory-model-test',
    'tests/directory_model_test.cpp',
    dependencies: browser_model,
  )
  test('directory-model', browser_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  file_launcher_test = executable('file-launcher-test',
    'tests/file_launcher_test.cpp', 'src/platform/file_launcher.cpp', 'src/core/log.cpp',
    dependencies: filesystem_service,
  )
  test('file-launcher', python,
    args: [files('tests/file_launcher_test.py'), file_launcher_test], timeout: 30,
  )
  controller_test = executable('browser-controller-test',
    'tests/browser_controller_test.cpp',
    dependencies: browser_model,
  )
  test('browser-controller', controller_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  navigation_test = executable('navigation-services-test',
    'tests/navigation_services_test.cpp', 'src/platform/terminal_launcher.cpp',
    dependencies: browser_model,
  )
  test('navigation-services', navigation_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  settings_test = executable('settings-test',
    'tests/settings_test.cpp',
    dependencies: [settings_service, browser_model],
  )
  test('settings', settings_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings'],
  )
  atomic_test = executable('atomic-file-test',
    'tests/atomic_file_test.cpp',
    dependencies: settings_service,
  )
  test('atomic-file', atomic_test, timeout: 30)
  state_test = executable('state-store-test',
    'tests/state_store_test.cpp',
    dependencies: state_service,
  )
  test('state-store', state_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  test('headless-cli', python,
    args: [files('tests/cli_test.py'), app, meson.project_version()],
    timeout: 30,
  )
endif

install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/noctalia-files')
install_subdir('assets/fonts', install_dir: get_option('datadir') / 'noctalia-files/assets')
install_data('assets/completions/noctalia-files.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'noctalia-files',
)
install_data('assets/completions/_noctalia-files', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/noctalia-files.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
