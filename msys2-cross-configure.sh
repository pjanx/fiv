#!/bin/sh -e
# msys2-cross-configure.sh: set up an MSYS2-based cross-compiled Meson build.
# Dependencies: AWK, sed, sha256sum, cURL, bsdtar,
# wine64, Meson, mingw-w64-binutils, mingw-w64-gcc, pkg-config
repository=https://repo.msys2.org/mingw/mingw64/

status() {
	echo "$(tput bold)-- $*$(tput sgr0)"
}

dbsync() {
	status Fetching repository DB
	[ -f db.tsv ] || curl -# "$repository/mingw64.db" | bsdtar -xOf- | awk '
		function flush() { print f["%NAME%"] f["%FILENAME%"] f["%DEPENDS%"] }
		NR > 1 && $0 == "%FILENAME%" { flush(); for (i in f) delete f[i] }
		!/^[^%]/ { field = $0; next } { f[field] = f[field] $0 "\t" }
		field == "%SHA256SUM%" { path = "*packages/" f["%FILENAME%"]
			sub(/\t$/, "", path); print $0, path > "db.sums" } END { flush() }
	' > db.tsv
}

fetch() {
	status Resolving "$@"
	mkdir -p packages
	awk -F'\t' 'function get(name,    i, a) {
		if (visited[name]++ || !(name in filenames)) return
		print filenames[name]; split(deps[name], a); for (i in a) get(a[i])
	} BEGIN { while ((getline < "db.tsv") > 0) {
		filenames[$1] = $2; deps[$1] = ""; for (i = 3; i <= NF; i++) {
			gsub(/[<=>].*/, "", $i); deps[$1] = deps[$1] $i FS }
	} for (i = 0; i < ARGC; i++) get(ARGV[i]) }' "$@" | while IFS= read -r name
	do
		status Fetching "$name"
		[ -f "packages/$name" ] || curl -#o "packages/$name" "$repository/$name"
	done

	version=$(curl -# https://exiftool.org/ver.txt)
	name=exiftool-$version.tar.gz remotename=Image-ExifTool-$version.tar.gz
	status Fetching "$remotename"
	[ -f "$name" ] || curl -#o "$name" "https://exiftool.org/$remotename"
	ln -sf "$name" exiftool.tar.gz
}

verify() {
	status Verifying checksums
	sha256sum --ignore-missing --quiet -c db.sums
}

extract() {
	status Extracting packages
	for subdir in *
	do [ -d "$subdir" -a "$subdir" != packages ] && rm -rf -- "$subdir"
	done
	for i in packages/*
	do bsdtar -xf "$i" --strip-components 1 mingw64
	done

	bsdtar -xf exiftool.tar.gz
	mv Image-ExifTool-*/exiftool bin
	mv Image-ExifTool-*/lib/* lib/perl5/site_perl
	rm -rf Image-ExifTool-*
}

configure() {
	# Don't let GLib programs inherit wrong paths from the environment.
	export XDG_DATA_DIRS=$msys2_root/share

	status Configuring packages
	wine64 bin/update-mime-database.exe share/mime
	wine64 bin/glib-compile-schemas.exe share/glib-2.0/schemas
	wine64 bin/gdk-pixbuf-query-loaders.exe \
		> lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
}

setup() {
	status Setting up Meson
	cat >"$toolchain" <<-EOF
	[binaries]
	c = 'x86_64-w64-mingw32-gcc'
	cpp = 'x86_64-w64-mingw32-g++'
	ar = 'x86_64-w64-mingw32-gcc-ar'
	ranlib = 'x86_64-w64-mingw32-gcc-ranlib'
	strip = 'x86_64-w64-mingw32-strip'
	windres = 'x86_64-w64-mingw32-windres'
	pkgconfig = 'pkg-config'

	[properties]
	sys_root = '$builddir'
	msys2_root = '$msys2_root'
	pkg_config_libdir = '$msys2_root/share/pkgconfig:$msys2_root/lib/pkgconfig'
	needs_exe_wrapper = true

	[host_machine]
	system = 'windows'
	cpu_family = 'x86_64'
	cpu = 'x86_64'
	endian = 'little'
	EOF

	meson setup --buildtype=debugoptimized --prefix="$packagedir" \
		--bindir . --libdir . --cross-file="$toolchain" "$builddir" "$sourcedir"
}

sourcedir=$(realpath "${2:-$(dirname "$0")}")
builddir=$(realpath "${1:-builddir}")
packagedir=$builddir/package
toolchain=$builddir/msys2-cross-toolchain.meson

# This directory name matches the prefix in .pc files, so we don't need to
# modify them (pkgconf has --prefix-variable, but Meson can't pass that option).
msys2_root=$builddir/mingw64

mkdir -p "$msys2_root"
cd "$msys2_root"
dbsync
fetch mingw-w64-x86_64-gtk3 mingw-w64-x86_64-lcms2 \
	mingw-w64-x86_64-libraw mingw-w64-x86_64-libheif \
	mingw-w64-x86_64-perl mingw-w64-x86_64-perl-win32-api \
	mingw-w64-x86_64-libwinpthread-git # Because we don't do "provides"?
verify
extract
configure
setup
