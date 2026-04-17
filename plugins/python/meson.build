python_opt = get_option('with-python')
if python_opt.startswith('python3')
  # Python 3.8 introduced the -embed pkg-config variant; prefer it when
  # available, fall back to the plain name otherwise.
  if not python_opt.endswith('-embed')
    python_dep = dependency(python_opt + '-embed', version: '>= 3.8', required: false)
    if not python_dep.found()
      python_dep = dependency(python_opt, version: '>= 3.8')
    endif
  else
    python_dep = dependency(python_opt, version: '>= 3.8')
  endif
else
  error('Python 2 is no longer supported; use python3 or python3-embed')
endif

python3_sources = files(
  'src/hc_python.c',
  'src/hc_python_module.c',
)

python3_inc = include_directories('src')

install_data(['_hexchat.py', 'hexchat.py', 'xchat.py'],
  install_dir: join_paths(get_option('libdir'), 'hexchat/python')
)

shared_module('python', python3_sources,
  include_directories: python3_inc,
  dependencies: [libgio_dep, hexchat_plugin_dep, python_dep],
  install: true,
  install_dir: plugindir,
  name_prefix: '',
  vs_module_defs: 'python.def'
)

subdir('tests')
