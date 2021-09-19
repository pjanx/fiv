project('fastiv', 'c', default_options : ['c_std=gnu99'], version : '0.1.0')

# TODO(p): Use libraw_r later, when we start parallelizing/preloading.
libraw = dependency('libraw', required : get_option('libraw'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('libturbojpeg'),
	libraw,
	meson.get_compiler('c').find_library('m', required : false),
]

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
conf.set('HAVE_LIBRAW', libraw.found())
configure_file(
	output : 'config.h',
	configuration : conf,
)

executable('fastiv', 'fastiv.c', 'fastiv-view.c', 'fastiv-io.c',
	install : true,
	dependencies : [dependencies])

install_data('fastiv.desktop',
	install_dir : get_option('datadir') + '/applications')
install_data('fastiv.svg',
	install_dir : get_option('datadir') + '/icons/hicolor/scalable/apps')
