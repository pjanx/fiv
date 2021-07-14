project('fastiv', 'c', default_options : ['c_std=gnu99'], version : '0.1.0')

conf = configuration_data()
conf.set_quoted('PROJECT_NAME', meson.project_name())
conf.set_quoted('PROJECT_VERSION', meson.project_version())
configure_file(
	output : 'config.h',
	configuration : conf,
)

dependencies = [
	dependency('gtk+-3.0'),
	dependency('libturbojpeg'),
]

# TODO(p): we need to create and install a .desktop file
executable('fastiv', 'fastiv.c', 'fastiv-view.c',
	install : true,
	dependencies : [dependencies])
