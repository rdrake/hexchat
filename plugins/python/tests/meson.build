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

python_interp_test = executable('python_interp_lifecycle_test',
  ['test_interp_lifecycle.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python Interpreter Lifecycle', python_interp_test,
  protocol: 'tap',
  timeout: 30,
)

python_api_core_test = executable('python_api_core_test',
  ['test_api_core.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python API core', python_api_core_test,
  protocol: 'tap',
  timeout: 30,
)

python_api_hooks_test = executable('python_api_hooks_test',
  ['test_api_hooks.c', '../src/hc_python_interp.c', python_test_common_sources],
  dependencies: python_test_common_deps,
  include_directories: python_test_common_inc,
)

test('Python API hooks', python_api_hooks_test,
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

# The plugin target is declared in plugins/python/meson.build; grab
# its built path so the integration test can dlopen it.
test('Python integration (dlopen)',
  python_integration_test,
  args: [meson.project_build_root() / 'plugins' / 'python' / 'python.dylib'],
  depends: [python_plugin_module],
  protocol: 'tap',
  timeout: 30,
)

