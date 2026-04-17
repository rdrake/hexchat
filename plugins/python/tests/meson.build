# stubs.c provides test-time definitions of `ph` and the hexchat_*
# host API symbols that the plugin normally resolves at dlopen time.
# Every test binary that links hc_python_module.c needs it.
python_test_common_deps = [libgio_dep, hexchat_plugin_dep, python_dep]
python_test_common_inc = [python3_inc, include_directories('.')]
python_test_common_sources = files(
  'stubs.c',
  '../src/hc_python_attrs.c',
  '../src/hc_python_hooks.c',
  '../src/hc_python_module.c',
  '../src/hc_python_plugin.c',
)

python_empty_module_test = executable('python_empty_module_test',
  ['test_empty_module.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python Empty Module', python_empty_module_test,
  protocol: 'tap',
  timeout: 30,
)
# (no env — this test never starts a full interpreter, so it doesn't
# need to find the hexchat package.)

python_interp_test = executable('python_interp_lifecycle_test',
  ['test_interp_lifecycle.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

# Every test that starts the interpreter needs to find the hexchat
# package; out-of-install, point at the in-tree source dir.
python_source_pkgdir = meson.project_source_root() / 'plugins' / 'python' / 'py'
python_test_env = {'HEXCHAT_PYTHON_DIR': python_source_pkgdir}

test('Python Interpreter Lifecycle', python_interp_test,
  env: python_test_env,
  protocol: 'tap',
  timeout: 30,
)

python_api_core_test = executable('python_api_core_test',
  ['test_api_core.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python API core', python_api_core_test,
  env: python_test_env,
  protocol: 'tap',
  timeout: 30,
)

python_api_hooks_test = executable('python_api_hooks_test',
  ['test_api_hooks.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python API hooks', python_api_hooks_test,
  env: python_test_env,
  protocol: 'tap',
  timeout: 30,
)

python_plugin_owner_test = executable('python_plugin_owner_test',
  ['test_plugin_owner.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python plugin ownership', python_plugin_owner_test,
  env: python_test_env,
  protocol: 'tap',
  timeout: 30,
)

# Integration test: dlopens the real plugin .dylib and drives the
# registered /py command through the stubs. Catches regressions in
# symbol resolution and init ordering that the unit tests can't see.
python_integration_test = executable('python_integration_test',
  ['test_integration.c', 'stubs.c'],
  dependencies: [libgio_dep, hexchat_plugin_dep],
  include_directories: python_test_common_inc,
  export_dynamic: true,
)

# Tests run out of the source tree, so point the plugin at the in-tree
# py/ package directory instead of the install location.
test('Python integration (dlopen)',
  python_integration_test,
  args: [meson.project_build_root() / 'plugins' / 'python' / 'python.dylib'],
  depends: [python_plugin_module],
  env: python_test_env,
  protocol: 'tap',
  timeout: 30,
)

