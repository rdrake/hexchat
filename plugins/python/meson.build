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
  'src/hc_python_attrs.c',
  'src/hc_python_console.c',
  'src/hc_python_hooks.c',
  'src/hc_python_interp.c',
  'src/hc_python_module.c',
  'src/hc_python_plugin.c',
)

python3_inc = include_directories('src')

# Legacy cffi-era .py files. Kept on-disk until step 8 so existing
# installations don't lose their current runtime during the rollout.
install_data(['_hexchat.py', 'hexchat.py', 'xchat.py'],
  install_dir: join_paths(get_option('libdir'), 'hexchat/python')
)

# New Python-side package: hexchat (re-exports _hexchat) + xchat
# compatibility alias. Installed alongside the legacy .py files;
# sys.path ordering decides which wins at import time.
python_pkgdir = join_paths(get_option('libdir'), 'hexchat/python')
install_data(
  'py/hexchat/__init__.py',
  'py/hexchat/_stdio.py',
  install_dir: join_paths(python_pkgdir, 'hexchat')
)
install_data('py/xchat/__init__.py',
  install_dir: join_paths(python_pkgdir, 'xchat')
)

# Full install prefix path, resolved at build time, so the plugin can
# hand it to PyConfig without guessing at runtime. The HEXCHAT_PYTHON_DIR
# environment variable overrides this (useful for tests and for running
# from a build tree without installing).
python_pkgdir_install = join_paths(get_option('prefix'), python_pkgdir)

python_plugin_module = shared_module('python', python3_sources,
  include_directories: python3_inc,
  dependencies: [libgio_dep, hexchat_plugin_dep, python_dep],
  c_args: ['-DHEXCHAT_PYTHONDIR="' + python_pkgdir_install + '"'],
  install: true,
  install_dir: plugindir,
  name_prefix: '',
  vs_module_defs: 'python.def'
)

subdir('tests')
