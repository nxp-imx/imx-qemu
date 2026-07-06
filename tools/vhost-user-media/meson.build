executable('vhost-user-media', files(
  'main.c', 'vhost-user-media.c'),
  dependencies: [qemuutil, glib, gio, vhost_user],
  install: true,
  install_dir: get_option('libexecdir'))
