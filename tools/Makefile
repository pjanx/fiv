SHELL = /bin/sh
# libjq 1.6 lacks a pkg-config file, and there is no release in sight.
CFLAGS = -g -O2 -Wall -Wextra `pkg-config --cflags $(deps)`
# libjq 1.6 is required.
LDLIBS = -ljq `pkg-config --libs $(deps)`

deps = libpng
targets = pnginfo jpeginfo

all: $(targets)
clean:
	rm -f $(targets)

.PHONY: all clean
