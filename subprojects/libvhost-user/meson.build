project('libvhost-user', 'c',
	license: 'GPL-2.0-or-later',
	default_options: ['warning_level=1', 'c_std=gnu99'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments('-Wsign-compare',
						 '-Wdeclaration-after-statement',
						 '-Wstrict-aliasing'),
		      native: false, language: 'c')

#if get_option('xen').enabled()
#  add_project_arguments(['-DCONFIG_XEN_LIBVHOST_USER'], native: false, language: 'c')
#endif

threads = dependency('threads')
glib = dependency('glib-2.0')

if get_option('xen').enabled()
  xencontrol = dependency('xencontrol', required: true)
  xenforeign = dependency('xenforeignmemory', required: true)
  xengnt = dependency('xengnttab', required: true)

  vhost_user = static_library('vhost-user',
			      files('libvhost-user.c', 'libvhost-user-xen.c'),
			      dependencies: [threads, xencontrol, xenforeign, xengnt, glib],
			      c_args: ['-D_GNU_SOURCE', '-DCONFIG_XEN_LIBVHOST_USER'])

else
  vhost_user = static_library('vhost-user',
			      files('libvhost-user.c'),
			      dependencies: threads,
			      c_args: '-D_GNU_SOURCE')

endif

executable('link-test', files('link-test.c'),
	   link_whole: vhost_user)

vhost_user_glib = static_library('vhost-user-glib',
				 files('libvhost-user-glib.c'),
				 link_with: vhost_user,
				 dependencies: glib)

vhost_user_dep = declare_dependency(link_with: vhost_user_glib,
				    dependencies: glib,
				    include_directories: include_directories('.'))
