project('fastiv', 'c', default_options : ['c_std=gnu99'], version : '0.1.0')

# TODO(p): Use libraw_r later, when we start parallelizing/preloading.
libraw = dependency('libraw', required : get_option('libraw'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('libturbojpeg'),
	dependency('libpng', version : '>=1.5.4'),
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
	'fastiv-browser.c',
	install : true,
	dependencies : [dependencies])

# TODO(p): See fastiv_io_open(), consider optionally integrating this.
gdk_pixbuf = dependency('gdk-pixbuf-2.0', required : false)
if gdk_pixbuf.found()
	executable('io-benchmark', 'fastiv-io-benchmark.c', 'fastiv-io.c',
		build_by_default : false,
		dependencies : [dependencies, gdk_pixbuf])
endif

install_data('fastiv.desktop',
	install_dir : get_option('datadir') + '/applications')
install_data('fastiv.svg',
	install_dir : get_option('datadir') + '/icons/hicolor/scalable/apps')
