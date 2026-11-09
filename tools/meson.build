# Opens the kernel HCI user channel; install with: setcap cap_net_admin+ep
executable('bpr-userchan-helper', 'userchan-helper.c',
  c_args : ['-Wshadow'],
  install : true,
  install_dir : get_option('libexecdir'),
)

# Headless hardware test (not installed): pair, connect, record a call.
executable('bpr-bt-test', 'bt-test.c',
  dependencies : [core_dep],
  c_args : core_args,
)
