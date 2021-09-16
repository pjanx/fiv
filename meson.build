project('fastiv', 'c', default_options : ['c_std=gnu99'], version : '0.1.0')

# TODO(p): consider whether libraw_r would be appropriate
libraw = dependency('libraw', required : get_option('libraw'))
dependencies = [
	dependency('gtk+-3.0'),
	dependency('libturbojpeg'),
	libraw,
]

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
conf.set('HAVE_LIBRAW', libraw.found())
configure_file(
	output : 'config.h',
	configuration : conf,
)

# TODO(p): we need to create and install a .desktop file
executable('fastiv', 'fastiv.c', 'fastiv-view.c',
	install : true,
	dependencies : [dependencies])
