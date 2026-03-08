CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0)

all: ptt_client ptt_server

ptt_client: ptt_client.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ $< $(GST_LIBS)

ptt_server: ptt_server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ptt_client ptt_server
