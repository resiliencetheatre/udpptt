CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WS_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags wireshark)
WS_LIBS ?= $(shell $(PKG_CONFIG) --libs wireshark)
SODIUM_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libsodium)
SODIUM_LIBS ?= $(shell $(PKG_CONFIG) --libs libsodium)
SOURCES = packet-mdpx.c ../dpi/protocol.c ../dpi/keys.c
.PHONY: all check-deps test clean
all: mdpx.so
check-deps:
	@test -n '$(WS_CFLAGS)' || { echo 'Wireshark headers missing: install libwireshark-dev (see README.md)' >&2; exit 1; }
mdpx.so: $(SOURCES) ../dpi/mdpx.h Makefile | check-deps
	$(CC) $(CPPFLAGS) -I../dpi $(WS_CFLAGS) $(SODIUM_CFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden -shared $(LDFLAGS) -Wl,-z,defs -o $@ $(SOURCES) $(WS_LIBS) $(SODIUM_LIBS) $(LDLIBS)
test: mdpx.so
	python3 ../tests/test_dpi_wireshark.py
clean:
	rm -f mdpx.so
