hexchat_apple_core = static_library('hexchatapplecore',
  sources: [
  'apple-runtime.c',
  'apple-frontend.c',
  'apple-callback-log.c',
  ],
  dependencies: [hexchat_common_dep, dependency('openssl')],
  install: false,
)

hexchat_apple_adapter = shared_library('hexchatappleadapter',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
  install: false,
)

executable('hexchat-apple-smoke',
  sources: 'apple-smoke.c',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
  install: false,
)

test_cfgdir = executable('test-fe-apple-cfgdir',
  sources: 'test-cfgdir.c',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
)

test('fe-apple-cfgdir', test_cfgdir)

test_callback_log = executable('test-fe-apple-callback-log',
  sources: ['test-callback-log.c', 'apple-callback-log.c'],
  dependencies: hexchat_common_dep,
)

test('fe-apple-callback-log', test_callback_log)

test_runtime = executable('test-fe-apple-runtime',
  sources: 'test-runtime.c',
  link_with: hexchat_apple_core,
  dependencies: [hexchat_common_dep, dependency('openssl')],
)

test('fe-apple-runtime', test_runtime)
