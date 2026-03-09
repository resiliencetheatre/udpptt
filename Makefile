CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null)

CLIENT_CFLAGS := $(GST_CFLAGS) $(SODIUM_CFLAGS)
CLIENT_LIBS   := $(GST_LIBS) $(SODIUM_LIBS)

TARGETS = ptt_client ptt_server

all: $(TARGETS)

ptt_client: ptt_client.c
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ $< $(CLIENT_LIBS)

ptt_server: ptt_server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
