# vim: noet ts=4 sts=4 sw=4:
project('jpeg-qs', 'c')
add_project_arguments('-DWITH_LOG', language : 'c')

deps = [
	dependency('libjpeg'),
	meson.get_compiler('c').find_library('m', required : false),
]

if host_machine.cpu_family() == 'x86_64'
	jpegqs_avx512 = static_library('jpegqs-avx512', 'libjpegqs.c',
		c_args : ['-DSIMD_SELECT', '-DSIMD_NAME=avx512',
			'-mavx512f', '-mfma', '-DSIMD_AVX512'],
		dependencies : deps,
		implicit_include_directories : false)
	jpegqs_avx2 = static_library('jpegqs-avx2', 'libjpegqs.c',
		c_args : ['-DSIMD_SELECT', '-DSIMD_NAME=avx2',
			'-mavx2', '-mfma', '-DSIMD_AVX2'],
		dependencies : deps,
		implicit_include_directories : false)
	jpegqs_sse2 = static_library('jpegqs-sse2', 'libjpegqs.c',
		c_args : ['-DSIMD_SELECT', '-DSIMD_NAME=sse2', '-msse2', '-DSIMD_SSE2'],
		dependencies : deps,
		implicit_include_directories : false)
	jpegqs_base = static_library('jpegqs-base', 'libjpegqs.c',
		c_args : ['-DSIMD_SELECT', '-DSIMD_NAME=base', '-DSIMD_BASE'],
		dependencies : deps,
		implicit_include_directories : false)

	jpegqs_lib = static_library('jpegqs', 'libjpegqs.c',
		c_args : ['-DSIMD_SELECT'],
		dependencies : deps,
		link_with : [jpegqs_base, jpegqs_sse2, jpegqs_avx2, jpegqs_avx512],
		implicit_include_directories : false)
else
	jpegqs_lib = static_library('jpegqs', 'libjpegqs.c',
		c_args : ['-DNO_SIMD'],
		dependencies : deps,
		implicit_include_directories : false)
endif

jpegqs_dep = declare_dependency(
	link_with : jpegqs_lib,
	include_directories : include_directories('include'),
)
