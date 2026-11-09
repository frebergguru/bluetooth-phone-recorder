test_env = environment()
test_env.set('XDG_CACHE_HOME', meson.current_build_dir() / 'xdg-cache')
test_env.set('XDG_STATE_HOME', meson.current_build_dir() / 'xdg-state')

foreach t : ['ringbuf', 'mixer', 'adaptive', 'recorder', 'firmware']
  exe = executable('test_' + t, 'test_' + t + '.c',
    dependencies : [core_dep],
    c_args : ['-Wno-unused-function'],
  )
  test(t, exe, workdir : meson.current_build_dir(), env : test_env, timeout : 120)
endforeach
