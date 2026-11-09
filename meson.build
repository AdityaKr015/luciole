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
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')

if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Recent GLib headers use __COUNTER__, which clang 23 rejects as a C2y extension under
# -Wpedantic -Werror. lib/ adds GLib with -I and its own -Wpedantic after project arguments,
# so mark the GLib include directory as a system directory instead (-isystem wins over -I).
add_project_arguments('-isystem', dependency('glib-2.0').get_variable(pkgconfig: 'includedir') / 'glib-2.0', language: 'cpp')

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

build_config = configuration_data()
build_config.set_quoted('NOCTALIA_FILES_VERSION', meson.project_version())
build_config.set_quoted('NOCTALIA_FILES_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('NOCTALIA_FILES_BUILD_TYPE', get_option('buildtype'))
build_config.set_quoted('NOCTALIA_FILES_COMPILER', cpp.get_id() + ' ' + cpp.version())
build_config.set_quoted('NOCTALIA_FILES_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'noctalia-files/assets')
build_info = configure_file(
  input: 'src/app/build_info.h.in',
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
src_inc = include_directories('src')

# One static library per module. A module's dependencies list only the modules
# its sources include, following the layering in CONTRIBUTING.md. Modules that
# must not see lib/ (fs, model, config, browser, operations) never depend on
# noctalia_ui_dep, so including a lib/ header from them fails to compile.
model_dep = declare_dependency(include_directories: src_inc)

event_loop_lib = static_library('files-event-loop',
  files('src/event_loop/glib_poll_source.cpp'),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
event_loop_dep = declare_dependency(link_with: event_loop_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

fs_lib = static_library('files-fs',
  files(
    'src/fs/archive_type.cpp',
    'src/fs/atomic_file.cpp',
    'src/fs/background_job.cpp',
    'src/fs/cancel_token.cpp',
    'src/fs/directory_session.cpp',
    'src/fs/directory_watch.cpp',
    'src/fs/file_error.cpp',
    'src/fs/file_icon_source.cpp',
    'src/fs/file_metadata.cpp',
    'src/fs/file_query.cpp',
    'src/fs/filename.cpp',
    'src/fs/folder_totals.cpp',
    'src/fs/folder_totals_walk.cpp',
    'src/fs/gio_interop.cpp',
    'src/fs/location.cpp',
    'src/fs/posix_fd.cpp',
    'src/fs/recursive_search.cpp',
    'src/fs/trash_info.cpp',
    'src/fs/xdg_dirs.cpp',
  ),
  include_directories: src_inc,
  dependencies: gio,
)
fs_dep = declare_dependency(link_with: fs_lib, include_directories: src_inc, dependencies: gio)

config_lib = static_library('files-config',
  files(
    'src/config/diagnostic.cpp',
    'src/config/settings.cpp',
    'src/config/state_store.cpp',
    'src/config/toml_decoder.cpp',
    'src/config/view_preference_writer.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, tomlplusplus],
)
config_dep = declare_dependency(link_with: config_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep],
)

browser_lib = static_library('files-browser',
  files(
    'src/browser/action_registry.cpp',
    'src/browser/browser_controller.cpp',
    'src/browser/directory_model.cpp',
    'src/browser/directory_tree.cpp',
    'src/browser/entry_details.cpp',
    'src/browser/selection_details.cpp',
    'src/browser/job_routing.cpp',
    'src/browser/location_presentation.cpp',
    'src/browser/miller_columns.cpp',
    'src/browser/path_completion.cpp',
    'src/browser/places.cpp',
    'src/browser/selection_tools.cpp',
    'src/browser/tab_workspace.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep],
)
browser_dep = declare_dependency(link_with: browser_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, config_dep],
)

libacl = dependency('libacl')
libarchive = dependency('libarchive')
operations_lib = static_library('files-operations',
  files(
    'src/operations/archive_icons.cpp',
    'src/operations/archive_service.cpp',
    'src/operations/archive_reader.cpp',
    'src/operations/archive_support.cpp',
    'src/operations/archive_writer.cpp',
    'src/operations/batch_rename_service.cpp',
    'src/operations/create_service.cpp',
    'src/operations/empty_trash_service.cpp',
    'src/operations/permission_accounts.cpp',
    'src/operations/permission_jobs.cpp',
    'src/operations/permission_service.cpp',
    'src/operations/permission_target.cpp',
    'src/operations/permission_walk.cpp',
    'src/operations/posix_acl.cpp',
    'src/operations/rename_service.cpp',
    'src/operations/restore_service.cpp',
    'src/operations/template_catalog.cpp',
    'src/operations/transfer_copy.cpp',
    'src/operations/transfer_service.cpp',
    'src/operations/transfer_staging.cpp',
    'src/operations/trash_item.cpp',
    'src/operations/trash_service.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive],
)
operations_dep = declare_dependency(link_with: operations_lib, include_directories: src_inc,
  dependencies: [fs_dep, libacl, libarchive],
)

libpng = dependency('libpng')
imaging_lib = static_library('files-imaging',
  files(
    'src/imaging/pixbuf_image_decoder.cpp',
    'src/imaging/png_decoder.cpp',
    'src/imaging/raster_decoder.cpp',
    'src/imaging/svg_rasterizer.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, gdk_pixbuf, glycin, libpng, dependency('librsvg-2.0')],
)
imaging_dep = declare_dependency(link_with: imaging_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, gdk_pixbuf],
)

system_lib = static_library('files-system',
  files(
    'src/system/freedesktop_key_file.cpp',
    'src/system/icon_resolver.cpp',
    'src/system/icon_theme_poll_source.cpp',
  ),
  include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)
system_dep = declare_dependency(link_with: system_lib, include_directories: src_inc,
  dependencies: [gio, noctalia_ui_dep],
)

thumbnails_lib = static_library('files-thumbnails',
  files(
    'src/thumbnails/file_icon_cache.cpp',
    'src/thumbnails/preview_service.cpp',
    'src/thumbnails/preview_metadata.cpp',
    'src/thumbnails/metadata_probe.cpp',
    'src/thumbnails/metadata_parse.cpp',
    'src/thumbnails/metadata_worker.cpp',
    'src/thumbnails/provider_process.cpp',
    'src/thumbnails/thumbnail_codec.cpp',
    'src/thumbnails/thumbnail_provider.cpp',
    'src/thumbnails/thumbnail_service.cpp',
    'src/thumbnails/thumbnail_store.cpp',
    'src/thumbnails/thumbnail_worker.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, fs_dep, imaging_dep, system_dep, noctalia_ui_dep, libpng, dependency('nlohmann_json')],
)
thumbnails_dep = declare_dependency(link_with: thumbnails_lib, include_directories: src_inc,
  dependencies: [model_dep, fs_dep, imaging_dep, noctalia_ui_dep],
)

wayland_client = dependency('wayland-client')
clipboard_lib = static_library('files-clipboard',
  files(
    'src/clipboard/file_payload.cpp',
    'src/clipboard/text_clipboard.cpp',
  ),
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)
clipboard_dep = declare_dependency(link_with: clipboard_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
)

platform_lib = static_library('files-platform',
  files(
    'src/platform/desktop_integration.cpp',
    'src/platform/file_launcher.cpp',
    'src/platform/terminal_launcher.cpp',
  ),
  protocol_sources,
  protocol_headers,
  include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client, dependency('wayland-cursor')],
)
platform_dep = declare_dependency(link_with: platform_lib, include_directories: src_inc,
  dependencies: [fs_dep, noctalia_ui_dep, wayland_client],
  sources: protocol_headers,
)

ui_lib = static_library('files-ui',
  files(
    'src/ui/browser/action_shortcuts.cpp',
    'src/ui/browser/file_icon.cpp',
    'src/ui/browser/file_tooltip.cpp',
    'src/ui/browser/file_view.cpp',
    'src/ui/browser/places_sidebar.cpp',
    'src/ui/browser/sidebar_tree_view.cpp',
    'src/ui/controls/acl_editor.cpp',
    'src/ui/controls/archive_view.cpp',
    'src/ui/controls/batch_rename_dialog.cpp',
    'src/ui/controls/browser_settings_view.cpp',
    'src/ui/controls/bulk_acl_editor.cpp',
    'src/ui/controls/file_error_text.cpp',
    'src/ui/controls/file_preview_view.cpp',
    'src/ui/controls/file_metadata_view.cpp',
    'src/ui/controls/folder_totals_view.cpp',
    'src/ui/controls/link_dialog.cpp',
    'src/ui/controls/miller_view.cpp',
    'src/ui/controls/name_editor.cpp',
    'src/ui/controls/permission_editor.cpp',
    'src/ui/controls/permission_results_view.cpp',
    'src/ui/controls/recursive_permission_editor.cpp',
    'src/ui/controls/selection_surface.cpp',
    'src/ui/controls/tooltip_presenter.cpp',
  ),
  include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep],
)
ui_dep = declare_dependency(link_with: ui_lib, include_directories: src_inc,
  dependencies: [model_dep, browser_dep, config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep],
)

app = executable('noctalia-files',
  files(
    'src/app/application.cpp',
    'src/app/cli.cpp',
    'src/app/file_manager_service.cpp',
    'src/app/library_setup.cpp',
    'src/app/main_loop.cpp',
    'src/app/operation_hub.cpp',
    'src/app/page/about_controller.cpp',
    'src/app/page/active_dialog.cpp',
    'src/app/page/application_chooser_controller.cpp',
    'src/app/page/archive_controller.cpp',
    'src/app/page/batch_rename_controller.cpp',
    'src/app/page/browser_page.cpp',
    'src/app/page/create_controller.cpp',
    'src/app/page/details_header_controller.cpp',
    'src/app/page/drag_drop_controller.cpp',
    'src/app/page/file_keyboard_controller.cpp',
    'src/app/page/location_bar_controller.cpp',
    'src/app/page/menu_controller.cpp',
    'src/app/page/preview_controller.cpp',
    'src/app/page/properties_controller.cpp',
    'src/app/page/rename_controller.cpp',
    'src/app/page/settings_controller.cpp',
    'src/app/page/sidebar_controller.cpp',
    'src/app/page/status_bar_controller.cpp',
    'src/app/page/transfer_controller.cpp',
    'src/app/page/trash_controller.cpp',
    'src/app/workspace_application.cpp',
    'src/main.cpp',
  ),
  build_info,
  completions,
  include_directories: include_directories('src', '.'),
  dependencies: [ui_dep, clipboard_dep, platform_dep, imaging_dep, system_dep, event_loop_dep, browser_dep,
    config_dep, operations_dep, thumbnails_dep, noctalia_ui_dep, wayland_client, dependency('threads')],
  install: true,
)

configure_file(
  input: 'assets/dbus/org.freedesktop.FileManager1.service.in',
  output: 'org.freedesktop.FileManager1.service',
  configuration: {'bindir': get_option('prefix') / get_option('bindir')},
  install_dir: get_option('datadir') / 'dbus-1/services',
)

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  subdir('tests')
endif

install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/noctalia-files')
install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'noctalia-files/assets')
install_data('assets/completions/noctalia-files.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'noctalia-files',
)
install_data('assets/completions/_noctalia-files', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/noctalia-files.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
