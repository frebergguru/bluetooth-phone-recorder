# Opens the kernel HCI user channel; install with: setcap cap_net_admin+ep
executable('bpr-userchan-helper', 'userchan-helper.c',
  c_args : ['-Wshadow'],
  install : true,
  install_dir : get_option('libexecdir'),
)
