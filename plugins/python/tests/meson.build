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
