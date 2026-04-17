python_empty_module_test_sources = [
  'test_empty_module.c',
  '../src/hc_python_module.c',
]

python_empty_module_test = executable('python_empty_module_test',
  python_empty_module_test_sources,
  dependencies: [python_dep],
  include_directories: python3_inc,
)

test('Python Empty Module', python_empty_module_test,
  protocol: 'tap',
  timeout: 30,
)

python_interp_test_sources = [
  'test_interp_lifecycle.c',
  '../src/hc_python_interp.c',
  '../src/hc_python_module.c',
]

python_interp_test = executable('python_interp_lifecycle_test',
  python_interp_test_sources,
  dependencies: [libgio_dep, python_dep],
  include_directories: python3_inc,
)

test('Python Interpreter Lifecycle', python_interp_test,
  protocol: 'tap',
  timeout: 30,
)
