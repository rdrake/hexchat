hexchat_gtk_sources = [
  'ascii.c',
  'banlist.c',
  'chanlist.c',
  'chanview.c',
  'custom-list.c',
  'dccgui.c',
  'editlist.c',
  'fe-gtk.c',
  'fkeys.c',
  'gtkutil.c',
  'ignoregui.c',
  'joind.c',
  'menu.c',
  'maingui.c',
  'notifygui.c',
  'palette.c',
  'pixmaps.c',
  'plugin-tray.c',
  'plugin-notification.c',
  'rawlog.c',
  'servlistgui.c',
  'setup.c',
  'textgui.c',
  'urlgrab.c',
  'userlistgui.c',
  'xtext.c',
  'xtext-render.c',
  'xtext-emoji.c',
  'hex-input-edit.c',
  'hex-emoji-chooser.c'
]

# GTK4 dependency — target Mint 22 / Ubuntu 24.04 baseline
gtk_dep = dependency('gtk4', version: '>= 4.14.0')

hexchat_gtk_deps = [
  hexchat_common_dep,
  libgmodule_dep, # used by libsexy
  gtk_dep
]

# GTK4: Need explicit X11 dependency for direct Xlib calls (XFlush, XGrabServer, etc.)
x11_dep = dependency('x11', required: false)
if x11_dep.found()
  hexchat_gtk_deps += x11_dep
endif

hexchat_gtk_cflags = []

hexchat_gtk_ldflags = []

# Tray library include directory (Windows only for now)
tray_inc = []

if host_machine.system() == 'windows'
  hexchat_gtk_sources += 'notifications/notification-windows.c'

  # System tray support using LizardByte/tray library
  hexchat_gtk_sources += '../../external/tray/src/tray_windows.c'
  tray_inc = include_directories('../../external/tray/src')

  # TODO: mingw doesn't have these headers or libs
  # add_languages('cpp')
  # shared_module('hcnotifications-winrt',
  #  sources: 'notifications/notification-winrt.cpp'
  #)

else
  hexchat_gtk_sources += 'notifications/notification-freedesktop.c'
endif


if get_option('plugin')
  hexchat_gtk_sources += 'plugingui.c'
endif

resources = gnome.compile_resources('resources',
  '../../data/hexchat.gresource.xml',
  source_dir: '../../data', # TODO: Fix upstream
  c_name: 'hexchat',
  extra_args: ['--manual-register']
)

if host_machine.system() == 'windows'
  hexchat_gtk_ldflags += '-Wl,-e,mainCRTStartup'
endif

executable('hexchat',
  sources:  resources + hexchat_gtk_sources,
  dependencies: hexchat_gtk_deps,
  include_directories: tray_inc,
  c_args: hexchat_gtk_cflags,
  link_args: hexchat_gtk_ldflags,
  pie: true,
  install: true,
  win_subsystem: 'windows',
)
