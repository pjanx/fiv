#!/bin/sh -e
export LC_ALL=C
cd "$MESON_INSTALL_DESTDIR_PREFIX"

# Input: Half-baked application bundle linked against Homebrew.
# Output: Portable application bundle.
source=/opt/homebrew
bindir=Contents/MacOS
libdir=Contents/Resources/lib
datadir=Contents/Resources/share

mkdir -p "$datadir"/glib-2.0/schemas
cp -p "$source"/share/glib-2.0/schemas/org.gtk.Settings.* \
	"$datadir"/glib-2.0/schemas
mkdir -p "$datadir"/icons
cp -pRL "$source"/share/icons/Adwaita "$datadir/"icons
mkdir -p "$datadir"/icons/hicolor
cp -p "$source"/share/icons/hicolor/index.theme "$datadir"/icons/hicolor
mkdir -p "$datadir/mime"
# GIO doesn't use the database on macOS, this subset is for us.
find "$source"/share/mime/ -maxdepth 1 -type f -exec cp -p {} "$datadir"/mime \;

# Copy binaries we directly or indirectly depend on.
#
# Homebrew is a bit chaotic in that some libraries are linked against locations
# in /opt/homebrew/Cellar, and some against /opt/homebrew/opt symlinks.
# We'll process things in such a way that it does not matter.
#
# As a side note, libraries in /usr/lib are now actually being served from
# a shared cache by the dynamic linker and aren't visible on the filesystem.
# There is an alternative to "otool -L" which can see them but it isn't
# particularly nicer to parse: "dyld_info -dependents/-linked_dylibs".
rm -rf "$libdir"
mkdir -p "$libdir"

pixbufdir=$libdir/gdk-pixbuf-2.0
loadersdir=$pixbufdir/loaders
cp -RL "$source"/lib/gdk-pixbuf-2.0/* "$pixbufdir"

# Fix a piece of crap loader that needs to be special.
svg=$loadersdir/libpixbufloader_svg.so
rm -f "$loadersdir"/libpixbufloader_svg.dylib
otool -L "$svg" | grep -o '@rpath/[^ ]*' | while IFS= read -r bad
do install_name_tool -change "$bad" "$source/lib/$(basename "$bad")" "$svg"
done

GDK_PIXBUF_MODULEDIR=$loadersdir gdk-pixbuf-query-loaders \
	| sed "s,$libdir,@rpath," > "$pixbufdir/loaders.cache"

gtkdir=$libdir/gtk-3.0
printbackendsdir=$gtkdir/printbackends
cp -RL "$source"/lib/gtk-3.0/* "$gtkdir"

# TODO: Figure out how to make gtk-query-immodules-3.0 pick up exactly
# what it needs to.  So far I'm not sure if this is at all even useful.
rm -rf "$gtkdir"/immodules*

find "$bindir" "$loadersdir" "$printbackendsdir" -type f -maxdepth 1 | awk '
	function collect(binary,    command, line) {
		if (seen[binary]++)
			return

		command = "otool -L \"" binary "\""
		while ((command | getline line) > 0)
			if (match(line, /^\t\/opt\/.+ \(/))
				collect(substr(line, RSTART + 1, RLENGTH - 3))
		close(command)
	} {
		collect($0)
		delete seen[$0]
	} END {
		for (library in seen)
			print library
	}
' | while IFS= read -r binary
do test -f "$libdir/$(basename "$binary")" || cp "$binary" "$libdir"
done

# Now redirect all binaries to internal linking.
# A good overview of how this works is "man dyld" and:
# https://itwenty.me/posts/01-understanding-rpath/
rewrite() {
	otool -L "$1" | sed -n 's,^\t\(.*\) (.*,\1,p' | grep '^/opt/' \
		| while IFS= read -r lib
	do install_name_tool -change "$lib" "@rpath/$(basename "$lib")" "$1"
	done
}

find "$bindir" -type f -maxdepth 1 | while IFS= read -r binary
do
	install_name_tool -add_rpath @executable_path/../Resources/lib "$binary"
	rewrite "$binary"
done

find "$libdir" -type f \( -name '*.so' -o -name '*.dylib' \) \
	| while IFS= read -r binary
do
	chmod 644 "$binary"
	install_name_tool -id "@rpath/${binary#$libdir/}" "$binary"
	rewrite "$binary"

	# Discard pointless @loader_path/../lib and absolute Homebrew paths.
	otool -l "$binary" | awk '
		$1 == "cmd" { command = $2 }
		command == "LC_RPATH" && $1 == "path" { print $2 }
	' | xargs -R 1 -I % install_name_tool -delete_rpath % "$binary"

	# Replace freshly invalidated code signatures with ad-hoc ones.
	codesign --force --sign - "$binary"
done

glib-compile-schemas "$datadir"/glib-2.0/schemas

# This may speed up program start-up a little bit.
gtk-update-icon-cache "$datadir"/icons/Adwaita
