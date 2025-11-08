#!/bin/sh -e
# macos-svg2icns.sh: convert an SVG to the macOS .icns format
if [ $# -ne 2 ]
then
	echo >&2 "Usage: $0 INPUT.svg OUTPUT.icns"
	exit 2
fi

svg=$1 icns=$2 tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

iconset="$tmpdir/$(basename "$icns" .icns).iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512
do
	size2x=$((size * 2))
	rsvg-convert --output="$iconset/icon_${size}x${size}.png" \
		--width=$size --height=$size "$svg"
	rsvg-convert --output="$iconset/icon_${size}x${size}@2x.png" \
		--width=$size2x --height=$size2x "$svg"
done
iconutil -c icns -o "$icns" "$iconset"
