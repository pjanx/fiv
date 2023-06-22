#!/bin/sh -e
export LC_ALL=C.UTF-8
cd "$MESON_BUILD_ROOT"

txt2rtf() {
	iconv -f utf-8 -t ascii//translit "$@" | awk 'BEGIN {
		print "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Tahoma;}}"
		print "\\f0\\fs24{\\pard\\sa240"
	} {
		gsub(/\\/, "\\\\"); gsub(/{/, "\\{"); gsub(/}/, "\\}")
		if (!$0) { print "\\par}{\\pard\\sa240"; prefix = "" }
		else { print prefix $0; prefix = " " }
	} END {
		print "\\par}}"
	}'
}

# We're being passed host_machine.cpu(), which will be either x86 or x86_64.
[ "$1" = "x86" ] && arch=x86 || arch=x64

wxs=fiv.wxs files=fiv-files.wxs msi=fiv.msi

txt2rtf "$MESON_SOURCE_ROOT/LICENSE" > License.rtf
find "$MESON_INSTALL_DESTDIR_PREFIX" -type f \
	| wixl-heat --prefix "$MESON_INSTALL_DESTDIR_PREFIX/" \
		--directory-ref INSTALLDIR --component-group CG.fiv \
		--var var.SourceDir > "$files"

wixl --verbose --arch "$arch" -D SourceDir="$MESON_INSTALL_DESTDIR_PREFIX" \
	--ext ui --output "$msi" "$wxs" "$files"
