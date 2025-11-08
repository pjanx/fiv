#!/bin/sh -e
# macos-configure.sh: set up a Homebrew-based macOS build
#
# Meson has no special support for macOS application bundles whatsoever.
#
# gtk-mac-bundler doesn't do anything particularly miraculous,
# and it doesn't directly support Homebrew.
#
# It would be cleaner and more reproducible to set up a special HOMEBREW_PREFIX,
# though right now we're happy to build an app bundle at all.
#
# It would also allow us to make a custom Little CMS build that includes
# the fast float plugin, which is a bit of a big deal.

# TODO: exiftool (Perl is part of macOS, at least for now)
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_ASK=1 brew install
	coreutils meson pkgconf shared-mime-info adwaita-icon-theme \
	gtk+3 jpeg-xl libavif libheif libraw librsvg little-cms2 webp

sourcedir=$(grealpath "${2:-$(dirname "$0")}")
builddir=$(grealpath "${1:-builddir}")
appdir=$builddir/fiv.app
meson setup --buildtype=debugoptimized --prefix="$appdir" \
	--bindir=Contents/MacOS --libdir=Contents/Resources/lib \
	--datadir=Contents/Resources/share "$builddir" "$sourcedir"
