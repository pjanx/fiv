#!/bin/sh -e
# Remove thumbnails with URIs pointing to at this moment non-existing files.
make pnginfo

pnginfo=$(pwd)/pnginfo cache_home=${XDG_CACHE_HOME:-$HOME/.cache}
for size in normal large x-large xx-large; do
	cd "$cache_home/thumbnails/$size" 2>/dev/null || continue
	find . -name '*.png' -print0 | PNGINFO_SKIP_TRAILING=1 xargs -0 "$pnginfo" \
		| jq -r '.info.texts."Thumb::URI"' | grep '^file:///' \
		| grep -v '^file:///run/media/[^/]*/NIKON/' \
		| perl -MURI -MURI::Escape -MDigest::MD5 -lne \
			'print Digest::MD5->new()->add($_)->hexdigest . ".png"
				if !stat(uri_unescape(URI->new($_)->path))' \
		| xargs rm
done
