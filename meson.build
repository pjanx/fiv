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

win32 = host_machine.system() == 'windows'

# The likelihood of this being installed is nearly zero. Enable the wrap.
libjpegqs = dependency('libjpegqs', required : get_option('libjpegqs'),
	allow_fallback : true)

lcms2 = dependency('lcms2', required : get_option('lcms2'))
# Note that only libraw_r is thread-safe, but we'll just run it out-of process.
libraw = dependency('libraw', required : get_option('libraw'))
# This is a direct runtime dependency, but its usage may be disabled for images.
librsvg = dependency('librsvg-2.0', required : get_option('librsvg'))
xcursor = dependency('xcursor', required : get_option('xcursor'))
libheif = dependency('libheif', required : get_option('libheif'))
libtiff = dependency('libtiff-4', required : get_option('libtiff'))
# This is a direct dependency of GTK+, but its usage may be disabled for images.
gdkpixbuf = dependency('gdk-pixbuf-2.0', required : get_option('gdk-pixbuf'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('pixman-1'),

	dependency('libturbojpeg'),
	dependency('libwebp'),
	dependency('libwebpdemux'),
	dependency('libwebpdecoder', required : false),
	dependency('libwebpmux'),
	# Wuffs is included as a submodule.

	lcms2,
	libjpegqs,
	libraw,
	librsvg,
	xcursor,
	libheif,
	libtiff,

	cc.find_library('m', required : false),
]

# As of writing, no pkg-config file is produced, and the plugin is not installed
# by default. The library can be built statically, but it's a bit of a hassle.
have_lcms2_fast_float = false
if not get_option('lcms2fastfloat').disabled()
	lcms2ff = dependency('lcms2_fast_float', required : false)
	if not lcms2ff.found()
		lcms2ff = cc.find_library(
			'lcms2_fast_float', required : get_option('lcms2fastfloat'))
		if lcms2ff.found() and not cc.has_header('lcms2_fast_float.h')
			error('lcms2_fast_float.h not found')
		endif
	endif
	if lcms2ff.found()
		dependencies += lcms2ff
		have_lcms2_fast_float = true
	endif
endif

# As of writing, the API is unstable, and no pkg-config file is produced.
# Trying to wrap Cargo in Meson is a recipe for pain, so no version pinning.
have_resvg = false
if not get_option('resvg').disabled()
	resvg = dependency('resvg', required : false)
	if not resvg.found()
		resvg = cc.find_library('resvg', required : get_option('resvg'))
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
conf.set_quoted('PROJECT_VERSION', '@VCS_TAG@')
conf.set_quoted('PROJECT_NS', application_ns)
conf.set_quoted('PROJECT_DOCDIR', get_option('prefix') / docdir)
if win32
	conf.set_quoted('PROJECT_DOCDIR', docdir)
endif

conf.set('HAVE_JPEG_QS', libjpegqs.found())
conf.set('HAVE_LCMS2', lcms2.found())
conf.set('HAVE_LCMS2_FAST_FLOAT', have_lcms2_fast_float)
conf.set('HAVE_LIBRAW', libraw.found())
conf.set('HAVE_RESVG', have_resvg)
conf.set('HAVE_LIBRSVG', librsvg.found())
conf.set('HAVE_XCURSOR', xcursor.found())
conf.set('HAVE_LIBHEIF', libheif.found())
conf.set('HAVE_LIBTIFF', libtiff.found())
conf.set('HAVE_GDKPIXBUF', gdkpixbuf.found())

config = vcs_tag(
	command : ['git', 'describe', '--always', '--dirty=+'],
	input : configure_file(output : 'config.h.in', configuration : conf),
	output : 'config.h',
)

rc = []
if win32
	windows = import('windows')
	rsvg_convert = find_program('rsvg-convert')
	icotool = find_program('icotool')

	# Meson is brain-dead and retarded, so these PNGs cannot be installed,
	# only because they must all have the same name when installed.
	# The largest size is mainly for an appropriately sized Windows icon.
	icon_png_list = []
	foreach size : ['16', '32', '48', '256']
		icon_dimensions = size + 'x' + size
		icon_png_list += custom_target(icon_dimensions + ' icon',
			input : 'fiv.svg',
			output : 'fiv.' + icon_dimensions + '.png',
			command : [rsvg_convert, '--output', '@OUTPUT@',
				'--width', size, '--height', size, '@INPUT@'])
	endforeach

	icon_ico = custom_target(input : icon_png_list, output : 'fiv.ico',
		command : [icotool, '-c', '-o', '@OUTPUT@', '@INPUT@'])
	rc += windows.compile_resources('fiv.rc', depends : icon_ico)
endif

gnome = import('gnome')
gresources = gnome.compile_resources('resources',
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
	'fiv-io-model.c', 'xdg.c', tiff_tables, gresources, rc, config,
	install : true,
	dependencies : dependencies,
	win_subsystem : 'windows',
)
if gdkpixbuf.found()
	executable('io-benchmark', 'fiv-io-benchmark.c', 'fiv-io.c',
		'xdg.c', tiff_tables,
		build_by_default : false,
		dependencies : [dependencies, gdkpixbuf])
endif

desktops += 'fiv-jpegcrop.desktop'
jpegcrop = executable('fiv-jpegcrop', 'fiv-jpegcrop.c', rc, config,
	install : true,
	dependencies : [
		dependency('gtk+-3.0'),
		dependency('libturbojpeg'),
	],
	win_subsystem : 'windows',
)

if get_option('tools').enabled()
	# libjq 1.6 lacks a pkg-config file, and there is no release in sight.
	# libjq 1.6 is required.
	tools_dependencies = [
		cc.find_library('jq'), dependency('libpng'), dependency('libraw')]
	tools_c_args = cc.get_supported_arguments(
		'-Wno-unused-function', '-Wno-unused-parameter')
	foreach tool : ['info', 'pnginfo', 'rawinfo']
		executable(tool, 'tools/' + tool + '.c', tiff_tables,
			dependencies : tools_dependencies,
			c_args: tools_c_args)
	endforeach
endif

gsettings_schemas = ['fiv.gschema.xml']
foreach schema : gsettings_schemas
	install_data(schema,
		rename : [application_ns + schema],
		install_dir : get_option('datadir') / 'glib-2.0' / 'schemas')
endforeach

# For the purposes of development: make the program find its GSettings schemas.
gnome.compile_schemas(depend_files : files(gsettings_schemas))

# Validate various files, if there are tools around to do it.
xmls = ['fiv.svg', 'fiv.manifest', 'resources/resources.gresource.xml'] + \
	gsettings_schemas
xmls += run_command(find_program('sed', required : false, disabler : true),
	'-n', 's@.*>\([^<>]*[.]svg\)<.*@resources/\\1@p',
	configure_file(
		input : 'resources/resources.gresource.xml',
		output : 'resources.gresource.xml.stamp',
		copy : true,
	), capture : true, check : true).stdout().strip().split('\n')

xmlwf = find_program('xmlwf', required : false, disabler : true)
xmllint = find_program('xmllint', required : false, disabler : true)
foreach xml : xmls
	test('xmlwf ' + xml, xmlwf, args : files(xml))
	test('xmllint ' + xml, xmllint, args : ['--noout', files(xml)])
endforeach

dfv = find_program('desktop-file-validate', required : false, disabler : true)
foreach desktop : desktops
	test(desktop, dfv, args : files(desktop))
endforeach

# Finish the installation.
install_data('fiv.svg',
	install_dir : get_option('datadir') / 'icons/hicolor/scalable/apps')
install_subdir('docs',
	install_dir : docdir, strip_directory : true)

if not win32
	asciidoctor = find_program('asciidoctor', required : false)
	a2x = find_program('a2x', required : false)
	if not asciidoctor.found() and not a2x.found()
		warning('Neither asciidoctor nor a2x were found, ' +
			'falling back to a substandard manual page generator')
	endif
	foreach page : [meson.project_name()]
		man_capture = false
		if asciidoctor.found()
			command = [asciidoctor, '-b', 'manpage',
				'-a', 'release-version=' + meson.project_version(),
				'-o', '@OUTPUT@', '@INPUT@']
		elif a2x.found()
			command = [a2x, '--doctype', 'manpage', '--format', 'manpage',
				'-a', 'release-version=' + meson.project_version(),
				'-D', '@OUTDIR@', '@INPUT@']
		else
			command = ['env', 'LC_ALL=C',
				'asciidoc-release-version=' + meson.project_version(),
				'awk', '-f', files('submodules/liberty/tools/asciiman.awk'),
				'@INPUT@']
			man_capture = true
		endif
		custom_target('manpage for ' + page,
			input : 'docs' / page + '.adoc',
			output : page + '.1',
			capture : man_capture,
			command : command,
			install : true,
			install_dir : get_option('mandir') / 'man1')
	endforeach

	foreach desktop : desktops
		install_data(desktop,
			rename : application_ns + desktop,
			install_dir : get_option('datadir') / 'applications')
	endforeach

	# TODO(p): Consider moving this to /usr/share or /usr/lib.
	install_data('fiv-reverse-search',
		install_dir : get_option('bindir'))

	# As usual, handling generated files in Meson is a fucking pain.
	updatable_desktops = [application_ns + 'fiv.desktop']
	foreach name, uri : {
		'Google' : 'https://lens.google.com/uploadbyurl?url=',
		'Bing' : 'https://www.bing.com/images/searchbyimage?cbir=sbi&imgurl=',
		'Yandex' : 'https://yandex.com/images/search?rpt=imageview&url=',
		'TinEye' : 'https://tineye.com/search?url=',
		'SauceNAO' : 'https://saucenao.com/search.php?url=',
		'IQDB' : 'https://iqdb.org/?url=',
	}
		desktop = 'fiv-reverse-search-' + name.to_lower() + '.desktop'
		updatable_desktops += application_ns + desktop

		test(desktop, dfv, args : configure_file(
			input : 'fiv-reverse-search.desktop.in',
			output : application_ns + desktop,
			configuration : {'NAME' : name, 'URL' : uri},
			install : true,
			install_dir : get_option('datadir') / 'applications',
		))
	endforeach

	# With gdk-pixbuf, fiv.desktop depends on currently installed modules;
	# the package manager needs to be told to run this script as necessary.
	dynamic_desktops = gdkpixbuf.found()

	updater = configure_file(
		input : 'fiv-update-desktop-files.in',
		output : 'fiv-update-desktop-files',
		configuration : {
			'FIV' : get_option('prefix') / get_option('bindir') / exe.name(),
			'DESKTOPDIR' : get_option('prefix') /
				get_option('datadir') / 'applications',
			'DESKTOPS' : ' \\\n\t'.join(updatable_desktops),
		},
		install : dynamic_desktops,
		install_dir : get_option('bindir'))
	if not meson.is_cross_build()
		meson.add_install_script(updater, skip_if_destdir : dynamic_desktops)
	endif
elif meson.is_cross_build()
	msys2_root = meson.get_external_property('msys2_root')
	meson.add_install_script('msys2-cross-install.sh', msys2_root)

	# This is the minimum to run targets from msys2-cross-configure.sh builds.
	meson.add_devenv({
		'WINEPATH' : msys2_root / 'bin',
		'XDG_DATA_DIRS' : msys2_root / 'share',
	})
endif
