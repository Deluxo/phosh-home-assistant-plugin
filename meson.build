project('phosh-home-assistant-plugin', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.60',
  default_options: ['warning_level=1'],
)

gnome = import('gnome')

prefix = get_option('prefix')
libdir = get_option('libdir')
datadir = get_option('datadir')

phosh_plugins_dep = dependency('phosh-plugins')
plugins_dir = phosh_plugins_dep.get_variable('quick_setting_plugins_dir')
plugin_prefs_dir = phosh_plugins_dep.get_variable('quick_setting_prefs_dir')

message('plugins_dir: ' + plugins_dir)
message('plugin_prefs_dir: ' + plugin_prefs_dir)

subdir('src')
