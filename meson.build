# vim: noet ts=4 sts=4 sw=4:
project('fiv', 'c',
	default_options : ['c_std=gnu99', 'warning_level=2'],
	version : '0.1.0')

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments('-Wno-cast-function-type'), language : 'c')

# This, annoyingly, enables the leak sanitizer by default,
# which requires some tuning to not stand in the way.
# Use -Db_sanitize=address,undefined and adjust LSAN_OPTIONS yourself.
#if get_option('buildtype').startswith('debug')
#	flags = cc.get_supported_arguments('-fsanitize=address,undefined')
#	add_project_arguments(flags, language : ['c'])
#	add_project_link_arguments(flags, language : ['c'])
#endif

# The likelihood of this being installed is nearly zero. Enable the wrap.
libjpegqs = dependency('libjpegqs', required : get_option('libjpegqs'),
	allow_fallback : true)

lcms2 = dependency('lcms2', required : get_option('lcms2'))
# Note that only libraw_r is thread-safe, but we'll just run it out-of process.
libraw = dependency('libraw', required : get_option('libraw'))
librsvg = dependency('librsvg-2.0', required : get_option('librsvg'))
xcursor = dependency('xcursor', required : get_option('xcursor'))
libheif = dependency('libheif', required : get_option('libheif'))
libtiff = dependency('libtiff-4', required : get_option('libtiff'))
# This is a direct dependency of GTK+, but its usage may be disabled.
gdkpixbuf = dependency('gdk-pixbuf-2.0', required : get_option('gdk-pixbuf'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('pixman-1'),

	# Wuffs is included as a submodule.
	dependency('libturbojpeg'),
	dependency('libwebp'),
	dependency('libwebpdemux'),
	dependency('libwebpdecoder', required : false),
	dependency('libwebpmux'),

	lcms2,
	libjpegqs,
	libraw,
	librsvg,
	xcursor,
	libheif,
	libtiff,

	cc.find_library('m', required : false),
]

# As of writing, the API is unstable, and no pkg-config file is produced.
# Trying to wrap Cargo in Meson is a recipe for pain, so no version pinning.
have_resvg = false
if not get_option('resvg').disabled()
	resvg = dependency('resvg', required : false)
	if not resvg.found()
		resvg = cc.find_library('libresvg', required : get_option('resvg'))
		if resvg.found() and not cc.has_header('resvg.h')
			error('resvg.h not found')
		endif
	endif
	if resvg.found()
		dependencies += resvg
		have_resvg = true
	endif
endif

# XXX: https://github.com/mesonbuild/meson/issues/825
docdir = get_option('datadir') / 'doc' / meson.project_name()

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
conf.set_quoted('PROJECT_DOCDIR', get_option('prefix') / docdir)
conf.set('HAVE_JPEG_QS', libjpegqs.found())
conf.set('HAVE_LCMS2', lcms2.found())
conf.set('HAVE_LIBRAW', libraw.found())
conf.set('HAVE_RESVG', have_resvg)
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

jpegcrop = executable('fiv-jpegcrop', 'fiv-jpegcrop.c',
	install : true,
	dependencies : [
		dependency('gtk+-3.0'),
		dependency('libturbojpeg'),
	])
install_data('fiv-jpegcrop.desktop',
	install_dir : get_option('datadir') / 'applications')

if get_option('tools').enabled()
	# libjq 1.6 lacks a pkg-config file, and there is no release in sight.
	# libjq 1.6 is required.
	tools_dependencies = [cc.find_library('libjq'), dependency('libpng')]
	tools_c_args = cc.get_supported_arguments(
		'-Wno-unused-function', '-Wno-unused-parameter')
	foreach tool : ['pnginfo', 'jpeginfo', 'tiffinfo', 'webpinfo', 'bmffinfo']
		executable(tool, 'tools/' + tool + '.c',
			dependencies : tools_dependencies,
			c_args: tools_c_args)
	endforeach
endif

# XXX: With gdk-pixbuf, this even depends on currently installed modules.
if meson.is_cross_build()
	install_data('fiv.desktop',
		install_dir : get_option('datadir') / 'applications')
else
	# XXX: The exe path may not contain spaces--quoting is a bitch in Meson.
	desktop = custom_target('desktop',
		output : 'fiv.desktop',
		input : 'fiv.desktop',
		command : ['sh', '-c', '''awk '/^MimeType=/ { $0 = "MimeType=";
			while (((exe " --list-supported-media-types") | getline type) > 0)
				$0 = $0 type ";" } 1' "exe=$1" "$2"''', 'sh', exe, '@INPUT@'],
		capture : true,
		install : true,
		install_dir : get_option('datadir') / 'applications',
	)
endif

install_data('fiv-browse.desktop',
	install_dir : get_option('datadir') / 'applications')
install_data('fiv.svg',
	install_dir : get_option('datadir') / 'icons/hicolor/scalable/apps')
install_subdir('docs', install_dir : docdir, strip_directory : true)
