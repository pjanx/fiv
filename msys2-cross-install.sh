#!/bin/sh -e
export LC_ALL=C
cd "$MESON_INSTALL_DESTDIR_PREFIX"
msys2_root=$1

# Copy libraries we depend on, and a few files required by GLib/GTK+.
cp -p "$msys2_root"/bin/exiftool.exe .
cp -p "$msys2_root"/bin/*.dll .
# The console helper is only useful for debug builds.
cp -p "$msys2_root"/bin/gspawn-*-helper*.exe .
cp -pR "$msys2_root"/etc/ .

mkdir -p lib
cp -pR "$msys2_root"/lib/gdk-pixbuf-2.0/ lib
mkdir -p share/glib-2.0/schemas
cp -pR "$msys2_root"/share/glib-2.0/schemas/*.Settings.* share/glib-2.0/schemas
mkdir -p share
cp -pR "$msys2_root"/share/mime/ share
mkdir -p share/icons
cp -pR "$msys2_root"/share/icons/Adwaita/ share/icons
mkdir -p share/icons/hicolor
cp -p "$msys2_root"/share/icons/hicolor/index.theme share/icons/hicolor

# Remove unreferenced libraries.
find lib -name '*.a' -exec rm -- {} +
awk 'function whitelist(binary) {
	if (seen[binary]++)
		return

	delete orphans[binary]
	while (("strings -a \"" binary "\" 2>/dev/null" | getline string) > 0)
		if (match(string, /[-.+_a-zA-Z0-9]+[.][Dd][Ll][Ll]$/))
			whitelist("./" substr(string, RSTART, RLENGTH))
} BEGIN {
	while (("find . -type f -path \"./*.[Dd][Ll][Ll]\"" | getline) > 0)
		orphans[$0]++
	while (("find . -type f -path \"./*.[Ee][Xx][Ee]\"" | getline) > 0)
		whitelist($0)
	while (("find ./lib/gdk-pixbuf-2.0 -type f " \
		"-path \"./*.[Dd][Ll][Ll]\"" | getline) > 0)
		whitelist($0)
	for (library in orphans)
		print library
}' | xargs rm --

# Removes unused icons from the Adwaita theme. It could be even more aggressive,
# since it keeps around lots of sizes and all the GTK+ stock icons.
find share/icons/Adwaita -type f | awk 'BEGIN {
	while (("grep -aho \"[a-z][a-z-]*\" *.dll *.exe" | getline) > 0)
		good[$0] = 1
} /[.](png|svg|cur|ani)$/ {
	# Cut out the basename without extensions.
	match($0, /[^\/]+$/)
	base = substr($0, RSTART)
	sub(/[.].+$/, "", base)

	# Try matching while cutting off suffixes.
	# Disregarding the not-much-used GTK_ICON_LOOKUP_GENERIC_FALLBACK.
	while (!(keep = good[base]) &&
		sub(/-(ltr|rtl|symbolic)$/, "", base)) {}
	if (!keep)
		print
}' | xargs rm --

wine64 "$msys2_root"/bin/glib-compile-schemas.exe share/glib-2.0/schemas

# This may speed up program start-up a little bit.
wine64 "$msys2_root"/bin/gtk-update-icon-cache-3.0.exe share/icons/Adwaita
