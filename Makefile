CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= /opt/udpptt

REAL_USER := $(if $(SUDO_USER),$(SUDO_USER),$(USER))
REAL_HOME := $(if $(SUDO_USER),$(shell getent passwd $(SUDO_USER) | cut -d: -f6),$(HOME))

USER_SYSTEMD_DIR ?= $(REAL_HOME)/.config/systemd/user
USER_CONFIG_DIR ?= $(REAL_HOME)/.config/udpptt
SERVICE_NAME ?= udpptt.service
ENV_NAME ?= udpptt.env

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null)

CLIENT_CFLAGS := $(GST_CFLAGS) $(SODIUM_CFLAGS)
CLIENT_LIBS   := $(GST_LIBS) $(SODIUM_LIBS)

TARGETS = ptt_client ptt_server
DATAFILES = start.wav stop.wav

all: $(TARGETS)

ptt_client: ptt_client.c
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ $< $(CLIENT_LIBS)

ptt_server: ptt_server.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 ptt_client "$(DESTDIR)$(BINDIR)/ptt_client"
	install -m 0755 ptt_server "$(DESTDIR)$(BINDIR)/ptt_server"

	mkdir -p "$(DESTDIR)$(DATADIR)"
	for f in $(DATAFILES); do \
		if [ -f "$$f" ]; then \
			install -m 0644 "$$f" "$(DESTDIR)$(DATADIR)/$$f"; \
		fi; \
	done

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
'WorkingDirectory=$(DATADIR)' \
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

	if [ -n "$(SUDO_USER)" ]; then \
		chown -R "$(REAL_USER):$(REAL_USER)" "$(REAL_HOME)/.config/systemd" "$(REAL_HOME)/.config/udpptt"; \
	fi

	@echo
	@echo "Installed binaries to: $(DESTDIR)$(BINDIR)"
	@echo "Installed tone files to: $(DESTDIR)$(DATADIR)"
	@echo "Installed user service to: $(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Installed env file to: $(USER_CONFIG_DIR)/$(ENV_NAME)"
	@echo "Target user: $(REAL_USER)"
	@echo
	@echo "Then run as that user:"
	@echo "  systemctl --user daemon-reload"
	@echo "  systemctl --user enable --now $(SERVICE_NAME)"
	@echo
	@echo "If keyboard PTT is used, make sure that user can read /dev/input/event*:"
	@echo "  sudo usermod -aG input $(REAL_USER)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/ptt_client"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_server"
	rm -f "$(DESTDIR)$(DATADIR)/start.wav"
	rm -f "$(DESTDIR)$(DATADIR)/stop.wav"
	rmdir "$(DESTDIR)$(DATADIR)" 2>/dev/null || true
	rm -f "$(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Removed binaries, tone files, and user service."
	@echo "Env file left in place: $(USER_CONFIG_DIR)/$(ENV_NAME)"

clean:
	rm -f $(TARGETS)

.PHONY: all install uninstall clean
