CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

USER_SYSTEMD_DIR ?= $(HOME)/.config/systemd/user
USER_CONFIG_DIR ?= $(HOME)/.config/udpptt
SERVICE_NAME ?= udpptt.service
ENV_NAME ?= udpptt.env

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

install: all
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 ptt_client "$(DESTDIR)$(BINDIR)/ptt_client"
	install -m 0755 ptt_server "$(DESTDIR)$(BINDIR)/ptt_server"

	mkdir -p "$(USER_SYSTEMD_DIR)"
	mkdir -p "$(USER_CONFIG_DIR)"

	printf '%s\n' \
'[Unit]' \
'Description=udpptt client' \
'After=network-online.target' \
'Wants=network-online.target' \
'' \
'[Service]' \
'Type=simple' \
'EnvironmentFile=%h/.config/udpptt/udpptt.env' \
'ExecStart=$(BINDIR)/ptt_client $${SERVER_IP} --altgr-ptt-delay-ms $${ALTGR_PTT_DELAY_MS} --txid $${CALL_SIGN} --encrypt --key $${WORD_OF_DAY}' \
'Restart=always' \
'RestartSec=3' \
'' \
'[Install]' \
'WantedBy=default.target' \
> "$(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"

	if [ ! -f "$(USER_CONFIG_DIR)/$(ENV_NAME)" ]; then \
		printf '%s\n' \
'SERVER_IP=198.51.100.10' \
'CALL_SIGN=Alpha' \
'WORD_OF_DAY=shared-room-secret' \
'ALTGR_PTT_DELAY_MS=2000' \
> "$(USER_CONFIG_DIR)/$(ENV_NAME)"; \
	fi

	@echo
	@echo "Installed binaries to: $(DESTDIR)$(BINDIR)"
	@echo "Installed user service to: $(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Installed env file to: $(USER_CONFIG_DIR)/$(ENV_NAME)"
	@echo
	@echo "Then run:"
	@echo "  systemctl --user daemon-reload"
	@echo "  systemctl --user enable --now $(SERVICE_NAME)"
	@echo
	@echo "If keyboard PTT is used, make sure your user can read /dev/input/event*."
	@echo "Usually this means adding the user to the input group:"
	@echo "  sudo usermod -aG input $$USER"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/ptt_client"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_server"
	rm -f "$(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Removed binaries and user service."
	@echo "Env file left in place: $(USER_CONFIG_DIR)/$(ENV_NAME)"

clean:
	rm -f $(TARGETS)

.PHONY: all install uninstall clean
