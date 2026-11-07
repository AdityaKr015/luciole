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
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version: '>=2.44')
glycin = dependency('glycin-2', version: '>=2.0.1')
if not cpp.has_header('stb/stb_image_resize2.h')
  error('system stb/stb_image_resize2.h is required')
endif
webp = dependency('libwebp')
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')
ui_dependencies = [gio, gdk_pixbuf, glycin, webp,
  dependency('wayland-cursor'), dependency('libpng'), dependency('librsvg-2.0'), dependency('threads'),
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

subdir('lib')
ui_dependencies += noctalia_ui_dep
protocol_headers += noctalia_ui_protocol_headers

build_config = configuration_data()
build_config.set_quoted('NOCTALIA_FILES_VERSION', meson.project_version())
build_config.set_quoted('NOCTALIA_FILES_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('NOCTALIA_FILES_BUILD_TYPE', get_option('buildtype'))
build_config.set_quoted('NOCTALIA_FILES_COMPILER', cpp.get_id() + ' ' + cpp.version())
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
  'src/app/library_setup.cpp',
  'src/platform/artwork_loader.cpp',
  'src/platform/pixbuf_image_decoder.cpp',
  'src/platform/desktop_integration.cpp',
  'src/browser/png_decoder.cpp',
  'src/app/cli.cpp',
  'src/app/main_loop.cpp',
  'src/browser/file_view.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/freedesktop_key_file.cpp',
  'src/system/icon_theme_poll_source.cpp',
  'src/browser/file_icon_cache.cpp',
  'src/browser/file_icon.cpp',
  'src/browser/thumbnail_codec.cpp',
  'src/browser/thumbnail_store.cpp',
  'src/browser/thumbnail_provider.cpp',
  'src/browser/thumbnail_worker.cpp',
  'src/browser/thumbnail_service.cpp',
  'src/browser/preview_service.cpp',
  'src/ui/controls/file_preview_view.cpp',
  'src/ui/controls/link_dialog.cpp',
  'src/ui/controls/permission_editor.cpp',
  'src/ui/controls/permission_results_view.cpp',
  'src/ui/controls/batch_rename_dialog.cpp',
  'src/config/view_preference_writer.cpp',
  'src/platform/file_launcher.cpp',
  'src/platform/text_clipboard.cpp',
  'src/platform/file_payload.cpp',
  'src/platform/terminal_launcher.cpp',
  'src/browser/places_sidebar.cpp',
  'src/ui/controls/name_editor.cpp',
  'src/ui/controls/browser_settings_view.cpp',
  'src/ui/controls/archive_view.cpp',
  'src/ui/controls/miller_view.cpp',
  'src/ui/controls/tooltip_presenter.cpp',
  'src/main.cpp',
  'src/ui/controls/selection_surface.cpp',
)

filesystem_service = declare_dependency(
  sources: files(
    'src/fs/location.cpp',
    'src/fs/filename.cpp',
    'src/fs/archive_type.cpp',
    'src/fs/file_metadata.cpp',
    'src/fs/trash_info.cpp',
    'src/fs/file_icon_source.cpp',
    'src/fs/directory_session.cpp',
    'src/fs/recursive_search.cpp',
    'src/app/glib_poll_source.cpp',
  ),
  include_directories: include_directories('src'),
  dependencies: [gio, noctalia_ui_dep],
)

browser_model = declare_dependency(
  sources: files('src/browser/directory_model.cpp', 'src/browser/browser_controller.cpp',
    'src/browser/miller_columns.cpp', 'src/browser/tab_workspace.cpp', 'src/browser/location_presentation.cpp', 'src/browser/path_completion.cpp', 'src/browser/action_registry.cpp', 'src/browser/selection_tools.cpp'),
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
  sources: files('src/operations/create_service.cpp', 'src/operations/template_catalog.cpp'),
  dependencies: filesystem_service,
)

rename_service = declare_dependency(
  sources: files('src/operations/rename_service.cpp'),
  dependencies: filesystem_service,
)

permission_service = declare_dependency(
  sources: files('src/operations/permission_service.cpp'),
  dependencies: filesystem_service,
)

batch_rename_service = declare_dependency(
  sources: files('src/operations/batch_rename_service.cpp'),
  dependencies: filesystem_service,
)

trash_service = declare_dependency(
  sources: files('src/operations/trash_service.cpp', 'src/operations/empty_trash_service.cpp',
    'src/operations/trash_item.cpp', 'src/operations/restore_service.cpp'),
  dependencies: filesystem_service,
)

transfer_service = declare_dependency(
  sources: files('src/operations/transfer_service.cpp'),
  dependencies: filesystem_service,
)

archive_service = declare_dependency(
  sources: files('src/operations/archive_service.cpp', 'src/operations/archive_work.cpp'),
  dependencies: [transfer_service, dependency('libarchive')],
)

app = executable('noctalia-files',
  app_sources,
  protocol_sources,
  protocol_headers,
  build_info,
  completions,
  include_directories: include_directories('src', '.'),
  dependencies: ui_dependencies + [browser_model, settings_service, state_service, places_service, transfer_service, rename_service, batch_rename_service, permission_service, create_service, trash_service, archive_service],
  install: true,
)

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  input_test = executable('input-lifetime-test', 'tests/library_test_setup.cpp', 'tests/input_lifetime_test.cpp',
    protocol_headers,
    include_directories: include_directories('src', '.'), dependencies: ui_dependencies, override_options: ['b_ndebug=false'])
  test('input-lifetime', input_test)
  tab_test = executable('tab-workspace-test', 'tests/library_test_setup.cpp', 'tests/tab_workspace_test.cpp', 'src/browser/tab_workspace.cpp', dependencies: noctalia_ui_dep, include_directories: include_directories('src'), override_options: ['b_ndebug=false'])
  test('tab-workspace', tab_test)
  archive_test = executable('archive-service-test', 'tests/library_test_setup.cpp', 'tests/archive_service_test.cpp', dependencies: archive_service, override_options: ['b_ndebug=false'])
  test('archive-service', archive_test, timeout: 90, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  thumbnail_test = executable('thumbnail-test', 'tests/library_test_setup.cpp',
    'src/platform/pixbuf_image_decoder.cpp', 'src/platform/artwork_loader.cpp',
    'tests/thumbnail_test.cpp',
    'src/browser/png_decoder.cpp',
    'src/browser/thumbnail_codec.cpp',
    'src/browser/thumbnail_store.cpp',
    'src/browser/thumbnail_provider.cpp',
    'src/browser/thumbnail_worker.cpp',
    'src/browser/thumbnail_service.cpp',
    dependencies: ui_dependencies + [filesystem_service, settings_service], override_options: ['b_ndebug=false']
  )
  test('thumbnail', thumbnail_test, timeout: 90, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  preview_test = executable('preview-service-test', 'tests/library_test_setup.cpp', 'tests/preview_service_test.cpp',
    'src/browser/preview_service.cpp',
    objects: thumbnail_test.extract_objects(files(
      'src/browser/png_decoder.cpp', 'src/browser/thumbnail_codec.cpp', 'src/browser/thumbnail_store.cpp',
      'src/browser/thumbnail_provider.cpp', 'src/browser/thumbnail_worker.cpp',
      'src/browser/thumbnail_service.cpp',)),
    dependencies: ui_dependencies + [filesystem_service, settings_service], override_options: ['b_ndebug=false'])
  test('preview-service', preview_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  python = import('python').find_installation('python3')
  clipboard_test = executable('clipboard-test', 'tests/library_test_setup.cpp', 'tests/clipboard_test.cpp', 'src/platform/text_clipboard.cpp', 'src/platform/file_payload.cpp', dependencies: [noctalia_ui_dep, filesystem_service, dependency('wayland-client'), dependency('wayland-server')], override_options: ['b_ndebug=false'])
  test('clipboard', clipboard_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  payload_test = executable('file-payload-test', 'tests/library_test_setup.cpp', 'tests/file_payload_test.cpp', 'src/platform/file_payload.cpp', dependencies: filesystem_service, override_options: ['b_ndebug=false'])
  test('file-payload', payload_test, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  actions_test = executable('browser-actions-test', 'tests/library_test_setup.cpp', 'tests/browser_actions_test.cpp', dependencies: browser_model, override_options: ['b_ndebug=false'])
  test('browser-actions', actions_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  empty_trash_test = executable('empty-trash-service-test', 'tests/library_test_setup.cpp', 'tests/empty_trash_service_test.cpp', dependencies: trash_service, override_options: ['b_ndebug=false'])
  test('empty-trash-service', empty_trash_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  restore_test = executable('restore-service-test', 'tests/library_test_setup.cpp', 'tests/restore_service_test.cpp', dependencies: trash_service, override_options: ['b_ndebug=false'])
  test('restore-service', restore_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  trash_browse_test = executable('trash-browse-test', 'tests/library_test_setup.cpp', 'tests/trash_browse_test.cpp', dependencies: browser_model, override_options: ['b_ndebug=false'])
  test('trash-browse', trash_browse_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  trash_test = executable('trash-service-test', 'tests/library_test_setup.cpp', 'tests/trash_service_test.cpp', dependencies: [trash_service, browser_model], override_options: ['b_ndebug=false'])
  test('trash-service', trash_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  create_test = executable('create-service-test', 'tests/library_test_setup.cpp', 'tests/create_service_test.cpp', dependencies: [create_service, browser_model], override_options: ['b_ndebug=false'])
  test('create-service', create_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  rename_test = executable('rename-service-test', 'tests/library_test_setup.cpp', 'tests/rename_service_test.cpp', dependencies: [rename_service, browser_model], override_options: ['b_ndebug=false'])
  test('rename-service', rename_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  batch_rename_test = executable('batch-rename-service-test', 'tests/library_test_setup.cpp', 'tests/batch_rename_service_test.cpp', dependencies: batch_rename_service, override_options: ['b_ndebug=false'])
  test('batch-rename-service', batch_rename_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  permission_test = executable('permission-service-test', 'tests/library_test_setup.cpp', 'tests/permission_service_test.cpp', dependencies: permission_service, override_options: ['b_ndebug=false'])
  test('permission-service', permission_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  transfer_test = executable('transfer-service-test', 'tests/library_test_setup.cpp', 'tests/transfer_service_test.cpp', dependencies: transfer_service, override_options: ['b_ndebug=false'])
  test('transfer-service', transfer_test, timeout: 90, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  places_test = executable('places-test', 'tests/library_test_setup.cpp', 'tests/places_test.cpp', dependencies: places_service, override_options: ['b_ndebug=false'])
  test('places', places_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  icon_test = executable('file-icon-test', 'tests/library_test_setup.cpp',
    'tests/file_icon_test.cpp',
    'src/platform/artwork_loader.cpp',
    'src/platform/pixbuf_image_decoder.cpp',
    'src/browser/png_decoder.cpp',
    'src/browser/thumbnail_codec.cpp',
    'src/browser/file_icon_cache.cpp',
    'src/system/icon_resolver.cpp',
    'src/system/freedesktop_key_file.cpp',
    dependencies: ui_dependencies + [filesystem_service], override_options: ['b_ndebug=false']
  )
  test('file-icon', icon_test, timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'])
  view_writer_test = executable('view-preference-writer-test', 'tests/library_test_setup.cpp',
    'tests/view_preference_writer_test.cpp',
    'src/config/view_preference_writer.cpp',
    dependencies: [settings_service, filesystem_service], override_options: ['b_ndebug=false']
  )
  test('view-preference-writer', view_writer_test, timeout: 30, env: ['G_DEBUG=fatal-warnings'])
  directory_test = executable('directory-session-test', 'tests/library_test_setup.cpp',
    'tests/directory_session_test.cpp',
    include_directories: include_directories('src'),
    dependencies: filesystem_service, override_options: ['b_ndebug=false']
  )
  test('directory-session', directory_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  browser_test = executable('directory-model-test', 'tests/library_test_setup.cpp',
    'tests/directory_model_test.cpp',
    dependencies: browser_model, override_options: ['b_ndebug=false']
  )
  test('directory-model', browser_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  file_launcher_test = executable('file-launcher-test', 'tests/library_test_setup.cpp',
    'tests/file_launcher_test.cpp', 'src/platform/file_launcher.cpp',
    dependencies: filesystem_service, override_options: ['b_ndebug=false']
  )
  test('file-launcher', python,
    args: [files('tests/file_launcher_test.py'), file_launcher_test], timeout: 30,
  )
  controller_test = executable('browser-controller-test', 'tests/library_test_setup.cpp',
    'tests/browser_controller_test.cpp',
    dependencies: browser_model, override_options: ['b_ndebug=false']
  )
  miller_test = executable('miller-columns-test', 'tests/library_test_setup.cpp',
    'tests/miller_columns_test.cpp',
    dependencies: browser_model, override_options: ['b_ndebug=false']
  )
  test('miller-columns', miller_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  test('browser-controller', controller_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  navigation_test = executable('navigation-services-test', 'tests/library_test_setup.cpp',
    'tests/navigation_services_test.cpp', 'src/platform/terminal_launcher.cpp',
    dependencies: browser_model, override_options: ['b_ndebug=false']
  )
  test('navigation-services', navigation_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  settings_test = executable('settings-test', 'tests/library_test_setup.cpp',
    'tests/settings_test.cpp',
    dependencies: [settings_service, browser_model], override_options: ['b_ndebug=false']
  )
  test('settings', settings_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings'],
  )
  atomic_test = executable('atomic-file-test',
    'tests/atomic_file_test.cpp',
    dependencies: settings_service, override_options: ['b_ndebug=false']
  )
  test('atomic-file', atomic_test, timeout: 30)
  state_test = executable('state-store-test', 'tests/library_test_setup.cpp',
    'tests/state_store_test.cpp',
    dependencies: state_service, override_options: ['b_ndebug=false']
  )
  test('state-store', state_test, timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GIO_USE_VFS=local'],
  )
  test('headless-cli', python,
    args: [files('tests/cli_test.py'), app, meson.project_version()],
    timeout: 30,
    env: ['NOCTALIA_FILES_ASSETS_DIR=' + (meson.project_source_root() / 'lib/assets')],
  )
endif

install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/noctalia-files')
install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'noctalia-files/assets')
install_data('assets/completions/noctalia-files.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'noctalia-files',
)
install_data('assets/completions/_noctalia-files', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/noctalia-files.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
