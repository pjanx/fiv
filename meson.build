# vim: noet ts=4 sts=4 sw=4:
project('fiv', 'c',
	default_options : ['c_std=gnu99', 'warning_level=2'],
	version : '0.1.0')
add_project_arguments(
	meson.get_compiler('c').get_supported_arguments('-Wno-cast-function-type'),
	language : 'c',
)

if get_option('buildtype').startswith('debug')
	flags = meson.get_compiler('c').get_supported_arguments(
		'-fsanitize=address,undefined')
	add_project_arguments(flags, language : ['c'])
	add_project_link_arguments(flags, language : ['c'])
endif

lcms2 = dependency('lcms2', required : get_option('lcms2'))
# Note that only libraw_r is thread-safe, but we'll just run it out-of process.
libraw = dependency('libraw', required : get_option('libraw'))
librsvg = dependency('librsvg-2.0', required : get_option('librsvg'))
xcursor = dependency('xcursor', required : get_option('xcursor'))
libheif = dependency('libheif', required : get_option('libheif'))
libtiff = dependency('libtiff-4', required : get_option('libtiff'))
gdkpixbuf = dependency('gdk-pixbuf-2.0', required : get_option('gdk-pixbuf'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('pixman-1'),

	dependency('libturbojpeg'),
	dependency('libjpeg', required : get_option('jpeg-qs')),
	dependency('libwebp'),
	dependency('libwebpdemux'),
	dependency('libwebpdecoder', required : false),
	dependency('libwebpmux'),
	# https://github.com/google/wuffs/issues/58
	dependency('spng', version : '>=0.7.0',
		default_options: 'default_library=static',
		# fallback : ['spng', 'spng_dep'],
	),

	lcms2,
	libraw,
	librsvg,
	xcursor,
	libheif,
	libtiff,
	gdkpixbuf,
	meson.get_compiler('c').find_library('m', required : false),
]

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
# TODO(p): Wrap it in a Meson subproject, try to enable SIMD.
conf.set('HAVE_JPEG_QS', get_option('jpeg-qs').enabled())
conf.set('HAVE_LCMS2', lcms2.found())
conf.set('HAVE_LIBRAW', libraw.found())
conf.set('HAVE_LIBRSVG', librsvg.found())
conf.set('HAVE_XCURSOR', xcursor.found())
conf.set('HAVE_LIBHEIF', libheif.found())
conf.set('HAVE_LIBTIFF', libtiff.found())
conf.set('HAVE_GDKPIXBUF', gdkpixbuf.found())
configure_file(
	output : 'config.h',
	configuration : conf,
)

gnome = import('gnome')
resources = gnome.compile_resources('resources',
	'resources/resources.gresource.xml',
	source_dir : 'resources',
	c_name : 'resources',
)

exe = executable('fiv', 'fiv.c', 'fiv-view.c', 'fiv-io.c',
	'fiv-browser.c', 'fiv-sidebar.c', 'fiv-thumbnail.c', 'xdg.c', resources,
	install : true,
	dependencies : [dependencies])

if gdkpixbuf.found()
	executable('io-benchmark', 'fiv-io-benchmark.c', 'fiv-io.c', 'xdg.c',
		build_by_default : false,
		dependencies : [dependencies, gdkpixbuf])
endif

# XXX: With gdk-pixbuf, this even dependens on currently installed modules.
if meson.is_cross_build()
	install_data('fiv.desktop',
		install_dir : get_option('datadir') + '/applications')
else
	# XXX: The exe path may not contain spaces--quoting is a bitch in Meson.
	desktop = custom_target('desktop',
		output : 'fiv.desktop',
		input : 'fiv.desktop',
		command : ['sh', '-c', '''awk '/^MimeType=/ { $0 = "MimeType=";
			while ((exe " --list-supported-media-types" | getline type) > 0)
				$0 = $0 type ";" } 1' "exe=$1" "$2"''', 'sh', exe, '@INPUT@'],
		capture : true,
		install : true,
		install_dir : get_option('datadir') + '/applications',
	)
endif

install_data('fiv-browse.desktop',
	install_dir : get_option('datadir') + '/applications')
install_data('fiv.svg',
	install_dir : get_option('datadir') + '/icons/hicolor/scalable/apps')
