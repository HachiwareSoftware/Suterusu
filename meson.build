project('suterusu', 'cpp',
  version: '1.0.0',
  default_options: [
    'cpp_std=c++17',
    'warning_level=3',
    'buildtype=release'
  ]
)

meson.add_install_script('scripts/copy_deps.py')

# Find dependencies
curl_dep = dependency('libcurl', required: true)
nlohmann_json_dep = dependency('nlohmann_json', required: true)

# Windows-specific libraries
windows_libs = []
if host_machine.system() == 'windows'
  windows_libs = [
    meson.get_compiler('cpp').find_library('ws2_32'),
  ]
endif

# Source files
sources = files('main.cpp')

# Build executable
executable('suterusu',
  sources,
  dependencies: [curl_dep, nlohmann_json_dep],
  win_subsystem: 'windows',  # Hide console by default (use --debug to show)
  install_dir: meson.current_source_dir() + '/bin',
  install: true
)

# Install config example
install_data('config.json', install_dir: meson.current_source_dir() + '/bin')
install_data('system_prompt.md', install_dir: meson.current_source_dir() + '/bin')