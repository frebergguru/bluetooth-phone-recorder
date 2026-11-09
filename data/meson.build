i18n.merge_file(
  input : app_id + '.desktop.in',
  output : app_id + '.desktop',
  type : 'desktop',
  po_dir : meson.project_source_root() / 'po',
  install : true,
  install_dir : get_option('datadir') / 'applications',
)
install_data(app_id + '.metainfo.xml', install_dir : get_option('datadir') / 'metainfo')
install_data(app_id + '.svg', install_dir : get_option('datadir') / 'icons/hicolor/scalable/apps')
install_data('70-bt-phone-recorder.rules', install_dir : prefix / 'lib/udev/rules.d')
