python_test_sources = [
  'test_empty_module.c',
  '../src/hc_python_module.c',
]

python_empty_module_test = executable('python_empty_module_test',
  python_test_sources,
  dependencies: [python_dep],
  include_directories: python3_inc,
)

test('Python Empty Module', python_empty_module_test,
  protocol: 'tap',
  timeout: 30,
)
