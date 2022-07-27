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
application_ns = 'name.janouch.'

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
conf.set_quoted('PROJECT_NS', application_ns)
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

tiff_tables = custom_target('tiff-tables.h',
	output : 'tiff-tables.h',
	input : 'tiff-tables.db',
	command : ['tiff-tables.awk', '@INPUT@'],
	capture : true,
)

desktops = ['fiv.desktop', 'fiv-browse.desktop']
exe = executable('fiv', 'fiv.c', 'fiv-view.c', 'fiv-io.c', 'fiv-context-menu.c',
	'fiv-browser.c', 'fiv-sidebar.c', 'fiv-thumbnail.c', 'fiv-collection.c',
	'xdg.c', resources,
	install : true,
	dependencies : dependencies)
if gdkpixbuf.found()
	executable('io-benchmark', 'fiv-io-benchmark.c', 'fiv-io.c', 'xdg.c',
		build_by_default : false,
		dependencies : [dependencies, gdkpixbuf])
endif

desktops += 'fiv-jpegcrop.desktop'
jpegcrop = executable('fiv-jpegcrop', 'fiv-jpegcrop.c',
	install : true,
	dependencies : [
		dependency('gtk+-3.0'),
		dependency('libturbojpeg'),
	])

if get_option('tools').enabled()
	# libjq 1.6 lacks a pkg-config file, and there is no release in sight.
	# libjq 1.6 is required.
	tools_dependencies = [cc.find_library('libjq'), dependency('libpng')]
	tools_c_args = cc.get_supported_arguments(
		'-Wno-unused-function', '-Wno-unused-parameter')
	foreach tool : ['pnginfo', 'jpeginfo', 'tiffinfo', 'webpinfo', 'bmffinfo']
		executable(tool, 'tools/' + tool + '.c', tiff_tables,
			dependencies : tools_dependencies,
			c_args: tools_c_args)
	endforeach
endif

gsettings_schemas = ['fiv.gschema.xml']
install_data(gsettings_schemas,
	rename : [application_ns + gsettings_schemas[0]],
	install_dir : get_option('datadir') / 'glib-2.0' / 'schemas')

# For the purposes of development: make the program find its GSettings schemas.
gnome.compile_schemas(depend_files : files(gsettings_schemas))

if host_machine.system() != 'windows'
	foreach desktop : desktops
		install_data(desktop,
			rename : application_ns + desktop,
			install_dir : get_option('datadir') / 'applications')
	endforeach

	# With gdk-pixbuf, fiv.desktop depends on currently installed modules;
	# the package manager needs to be told to run this script as necessary.
	dynamic_desktops = gdkpixbuf.found()

	updater = configure_file(
		input : 'fiv-update-desktop-files.in',
		output : 'fiv-update-desktop-files',
		configuration : {
			'EXE' : get_option('prefix') / get_option('bindir') / exe.name(),
			'DESKTOP' : get_option('prefix') / get_option('datadir') \
				/ 'applications' / application_ns + 'fiv.desktop',
		},
		install : dynamic_desktops,
		install_dir : get_option('bindir'))
	if not meson.is_cross_build()
		meson.add_install_script(updater, skip_if_destdir : dynamic_desktops)
	endif
endif

install_data('fiv.svg',
	install_dir : get_option('datadir') / 'icons/hicolor/scalable/apps')
install_subdir('docs', install_dir : docdir, strip_directory : true)
