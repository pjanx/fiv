SHELL = /bin/sh
# libjq 1.6 lacks a pkg-config file, and there is no release in sight.
# libjq 1.6 is required.
CFLAGS = -g -O2 -Wall -Wextra `pkg-config --cflags $(deps)`
LDLIBS = -ljq `pkg-config --libs $(deps)`

deps = libpng
targets = pnginfo jpeginfo tiffinfo webpinfo

all: $(targets)
$(targets): info.h
clean:
	rm -f $(targets)

.PHONY: all clean
