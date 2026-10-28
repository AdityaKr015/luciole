project('noctalia-files', 'cpp',
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
app = executable('noctalia-files',
  'src/main.cpp',
  'src/app/cli.cpp',
  build_info,
  completions,
  include_directories: include_directories('src', '.'),
  install: true,
)

tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto() and get_option('buildtype') == 'debug' and get_option('b_sanitize') == 'none'
)
if build_tests
  python = import('python').find_installation('python3')
  test('headless-cli', python,
    args: [files('tests/cli_test.py'), app, meson.project_version()],
    timeout: 30,
  )
endif

install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/noctalia-files')
install_data('assets/completions/noctalia-files.bash',
  install_dir: get_option('datadir') / 'bash-completion/completions',
  rename: 'noctalia-files',
)
install_data('assets/completions/_noctalia-files', install_dir: get_option('datadir') / 'zsh/site-functions')
install_data('assets/completions/noctalia-files.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')
