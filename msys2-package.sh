#!/bin/sh -e
export LC_ALL=C
cd "$MESON_BUILD_ROOT"
arch=$1 msi=$2 files=package-files.wxs destdir=$(pwd)/package
shift 2

# We're being passed host_machine.cpu(), which will be either x86 or x86_64.
[ "$arch" = "x86" ] || arch=x64

rm -rf "$destdir"
meson install --destdir "$destdir"

txt2rtf() {
	LC_ALL=C.UTF-8 iconv -f utf-8 -t ascii//translit "$@" | awk 'BEGIN {
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

# msitools have this filename hardcoded in UI files, and it's required.
txt2rtf "$MESON_SOURCE_ROOT/LICENSE" > License.rtf

find "$destdir" -type f \
	| wixl-heat --prefix "$destdir/" --directory-ref INSTALLDIR \
		--component-group CG.fiv --var var.SourceDir > "$files"

wixl --verbose --arch "$arch" -D SourceDir="$destdir" --ext ui \
	--output "$msi" "$@" "$files"
