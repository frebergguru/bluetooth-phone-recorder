conf = configuration_data()
conf.set_quoted('BPR_VERSION', meson.project_version())
conf.set_quoted('BPR_APP_ID', app_id)
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('BPR_LOCALEDIR', localedir)
conf.set10('HAVE_OBS_WEBSOCKET', soup_dep.found() and json_dep.found())
configure_file(output : 'config.h', configuration : conf)
config_inc = include_directories('.')

ui_src = files(
  '../main.c',
  'app.c', 'window.c', 'mixer_view.c', 'dialogs.c', 'settings.c', 'docshots.c',
  'calldata.c', 'calls_view.c',
  '../obs/obs_ws.c',
)

ui_deps = [core_dep, gtk_dep, adw_dep]
if soup_dep.found() and json_dep.found()
  ui_deps += [soup_dep, json_dep]
endif

executable('bt-phone-recorder', ui_src,
  include_directories : config_inc,
  dependencies : ui_deps,
  c_args : core_args + ['-DG_LOG_DOMAIN="bt-phone-recorder"'],
  install : true,
)
