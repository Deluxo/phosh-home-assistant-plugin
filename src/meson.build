name = 'home-assistant-quick-setting'

# --- plugin (quick setting widget, gtk3 + libphosh) ---
plugin_deps = [
  dependency('libphosh-0.45'),
  dependency('phosh-plugins'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
  dependency('gtk+-3.0'),
  dependency('libsoup-3.0'),
]

plugin_resources = gnome.compile_resources(
  'phosh-plugin-home-assistant-quick-setting-resources',
  'phosh-plugin-home-assistant-quick-setting.gresources.xml',
  c_name: 'phosh_plugin_home_assistant_quick_setting',
)

shared_module(
  'phosh-plugin-' + name,
  'home-assistant-quick-setting.c',
  'phosh-plugin-home-assistant-quick-setting.c',
  plugin_resources,
  c_args: [
    '-DG_LOG_DOMAIN="phosh-plugin-@0@"'.format(name),
    '-DPLUGIN_NAME="@0@"'.format(name),
  ],
  dependencies: plugin_deps,
  install: true,
  install_dir: plugins_dir,
)

# .plugin file: fasttrack skips i18n merge, English only
pluginconf = configuration_data()
pluginconf.set('name', name)
pluginconf.set('plugins_dir', plugins_dir)
pluginconf.set('plugin_prefs_dir', plugin_prefs_dir)

configure_file(
  input: name + '.desktop.in.in',
  output: name + '.plugin',
  configuration: pluginconf,
  install: true,
  install_dir: plugins_dir,
)

install_data(
  'icons/home-assistant-quick-setting-symbolic.svg',
  rename: 'home-assistant-quick-setting-symbolic.svg',
  install_dir: get_option('datadir') / 'phosh' / 'plugins' / 'icons',
)

# gsettings schema (device needs glib-compile-schemas after install)
schema = 'mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml'
compile_schemas = find_program('glib-compile-schemas', required: false)
if compile_schemas.found()
  test('validate schema', compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()])
endif
install_data(schema, install_dir: 'share/glib-2.0/schemas')

# --- prefs (gtk4 + libadwaita dialog) ---
plugin_prefs_config = configuration_data()
plugin_prefs_config.set_quoted('GETTEXT_PACKAGE', 'phosh')
plugin_prefs_config.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
plugin_prefs_config_h = configure_file(
  output: 'phosh-plugin-prefs-config.h',
  configuration: plugin_prefs_config,
)

prefs_deps = [
  dependency('phosh-plugins'),
  dependency('gtk4'),
  dependency('libadwaita-1', version: '>=1.5'),
]

prefs_resources = gnome.compile_resources(
  'phosh-plugin-prefs-home-assistant-quick-setting-resources',
  'prefs/phosh-plugin-prefs-home-assistant-quick-setting.gresources.xml',
  source_dir: meson.current_source_dir() / 'prefs',
  c_name: 'phosh_plugin_prefs_home_assistant_quick_setting',
)

shared_module(
  'phosh-plugin-prefs-' + name,
  'prefs/home-assistant-quick-setting-prefs.c',
  'prefs/phosh-plugin-prefs-home-assistant-quick-setting.c',
  plugin_prefs_config_h,
  prefs_resources,
  c_args: [
    '-DG_LOG_DOMAIN="phosh-plugin-prefs-@0@"'.format(name),
    '-DPLUGIN_PREFS_NAME="@0@-prefs"'.format(name),
  ],
  dependencies: prefs_deps,
  install: true,
  install_dir: plugin_prefs_dir,
)
