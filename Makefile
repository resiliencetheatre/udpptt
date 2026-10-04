CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= /opt/udpptt

REAL_USER := $(if $(SUDO_USER),$(SUDO_USER),$(USER))
REAL_HOME := $(if $(SUDO_USER),$(shell getent passwd $(SUDO_USER) | cut -d: -f6),$(HOME))

USER_SYSTEMD_DIR ?= $(REAL_HOME)/.config/systemd/user
USER_CONFIG_DIR ?= $(REAL_HOME)/.config/udpptt
USER_EXTENSIONS_DIR ?= $(REAL_HOME)/.local/share/gnome-shell/extensions
SERVICE_NAME ?= udpptt.service
ENV_NAME ?= udpptt.env

GNOME_EXTENSION_UUID ?= udpptt-indicator@resiliencetheatre.codeberg.org
GNOME_EXTENSION_SRC ?= gnome-shell-extension/$(GNOME_EXTENSION_UUID)
GNOME_EXTENSION_DST ?= $(USER_EXTENSIONS_DIR)/$(GNOME_EXTENSION_UUID)

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0 2>/dev/null)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0 2>/dev/null)
SODIUM_CFLAGS := $(shell pkg-config --cflags libsodium 2>/dev/null)
SODIUM_LIBS   := $(shell pkg-config --libs libsodium 2>/dev/null)

OPUS_CFLAGS := $(shell pkg-config --cflags opus 2>/dev/null)
OPUS_LIBS := $(shell pkg-config --libs opus 2>/dev/null)

CLIENT_CFLAGS := $(GST_CFLAGS) $(SODIUM_CFLAGS) $(OPUS_CFLAGS)
CLIENT_LIBS   := $(GST_LIBS) $(SODIUM_LIBS) $(OPUS_LIBS)

TARGETS = ptt_wav_gate ptt_client ptt_server ptt_helper ptt_hid
DATAFILES = start.wav stop.wav

all: $(TARGETS)

ptt_client: ptt_client.c ptt_wav_io.h ptt_protocol.h ptt_jitter.h ptt_telemetry.h ptt_jitter.c ptt_telemetry.c
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ ptt_client.c ptt_jitter.c ptt_telemetry.c $(CLIENT_LIBS) -lm

ptt_wav_gate: ptt_wav_gate.c ptt_client.c ptt_wav_io.h ptt_protocol.h ptt_jitter.h ptt_telemetry.h ptt_jitter.c ptt_telemetry.c
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -o $@ ptt_wav_gate.c ptt_jitter.c ptt_telemetry.c $(CLIENT_LIBS) -lm

ptt_server: ptt_server.c ptt_protocol.h
	$(CC) $(CFLAGS) -o $@ $<

ptt_helper: ptt_helper.c
	$(CC) $(CFLAGS) -o $@ $<

ptt_hid: ptt_hid.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 ptt_client "$(DESTDIR)$(BINDIR)/ptt_client"
	install -m 0755 ptt_wav_gate "$(DESTDIR)$(BINDIR)/ptt_wav_gate"
	install -m 0755 ptt_server "$(DESTDIR)$(BINDIR)/ptt_server"
	install -m 0755 ptt_helper "$(DESTDIR)$(BINDIR)/ptt_helper"
	install -m 0755 ptt_hid "$(DESTDIR)$(BINDIR)/ptt_hid"

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
'ExecStart=$(BINDIR)/ptt_client $${SERVER_IP} --ptt-socket $${PTT_SOCKET} --altgr-ptt-delay-ms $${ALTGR_PTT_DELAY_MS} --txid $${CALL_SIGN} --encrypt --key $${WORD_OF_DAY}' \
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
'PTT_SOCKET=/tmp/udpptt.sock' \
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
	@echo "For HID PTT, run ptt_hid separately, for example:"
	@echo "  ptt_hid --socket /tmp/udpptt.sock --key volumeup"
	@echo
	@echo "If keyboard or HID PTT is used, make sure that user can read the needed /dev/input/event* device:"
	@echo "  sudo usermod -aG input $(REAL_USER)"

install-gnome-extension: all
	@if ! command -v gnome-shell >/dev/null 2>&1; then \
		echo "ERROR: gnome-shell was not found. This target is only for GNOME Shell desktop systems."; \
		echo "Install GNOME Shell or use 'make install' for non-GNOME / console targets."; \
		exit 1; \
	fi
	@if ! pgrep -u "$(REAL_USER)" -x gnome-shell >/dev/null 2>&1; then \
		echo "ERROR: no running gnome-shell process found for user '$(REAL_USER)'."; \
		echo "Log into a GNOME session as $(REAL_USER), then run this target again."; \
		echo "For non-GNOME systems, use 'make install' instead."; \
		exit 1; \
	fi
	@if [ ! -d "$(GNOME_EXTENSION_SRC)" ]; then \
		echo "ERROR: GNOME extension source directory not found:"; \
		echo "  $(GNOME_EXTENSION_SRC)"; \
		exit 1; \
	fi
	@if [ ! -f "$(GNOME_EXTENSION_SRC)/metadata.json" ] || [ ! -f "$(GNOME_EXTENSION_SRC)/extension.js" ]; then \
		echo "ERROR: GNOME extension source is missing metadata.json or extension.js:"; \
		echo "  $(GNOME_EXTENSION_SRC)"; \
		exit 1; \
	fi

	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m 0755 ptt_client "$(DESTDIR)$(BINDIR)/ptt_client"
	install -m 0755 ptt_wav_gate "$(DESTDIR)$(BINDIR)/ptt_wav_gate"
	install -m 0755 ptt_server "$(DESTDIR)$(BINDIR)/ptt_server"
	install -m 0755 ptt_helper "$(DESTDIR)$(BINDIR)/ptt_helper"
	install -m 0755 ptt_hid "$(DESTDIR)$(BINDIR)/ptt_hid"

	mkdir -p "$(DESTDIR)$(DATADIR)"
	for f in $(DATAFILES); do \
		if [ -f "$$f" ]; then \
			install -m 0644 "$$f" "$(DESTDIR)$(DATADIR)/$$f"; \
		fi; \
	done

	mkdir -p "$(USER_SYSTEMD_DIR)"
	mkdir -p "$(USER_CONFIG_DIR)"
	mkdir -p "$(USER_EXTENSIONS_DIR)"
	rm -rf "$(GNOME_EXTENSION_DST)"
	mkdir -p "$(GNOME_EXTENSION_DST)"
	cp -a "$(GNOME_EXTENSION_SRC)/." "$(GNOME_EXTENSION_DST)/"

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
'LoadCredentialEncrypted=word_of_day:%h/.config/udpptt/word_of_day.cred' \
'ExecStartPre=/usr/bin/mkdir -p %t/udpptt' \
'ExecStart=/bin/sh -c '\''exec $(BINDIR)/ptt_client "$${SERVER_IP}" --altgr-ptt-delay-ms "$${ALTGR_PTT_DELAY_MS}" --txid "$${CALL_SIGN}" --state-file "%t/udpptt/state" --rx-state-timeout-ms "$${RX_STATE_TIMEOUT_MS:-1000}" --encrypt --key "$$(cat "$${CREDENTIALS_DIRECTORY}/word_of_day")"'\''' \
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
'ALTGR_PTT_DELAY_MS=2000' \
'PTT_SOCKET=/tmp/udpptt.sock' \
'RX_STATE_TIMEOUT_MS=1000' \
> "$(USER_CONFIG_DIR)/$(ENV_NAME)"; \
	else \
		grep -q '^RX_STATE_TIMEOUT_MS=' "$(USER_CONFIG_DIR)/$(ENV_NAME)" || printf '%s\n' 'RX_STATE_TIMEOUT_MS=1000' >> "$(USER_CONFIG_DIR)/$(ENV_NAME)"; \
	fi

	if [ -n "$(SUDO_USER)" ]; then \
		chown -R "$(REAL_USER):$(REAL_USER)" "$(REAL_HOME)/.config/systemd" "$(REAL_HOME)/.config/udpptt" "$(REAL_HOME)/.local/share/gnome-shell"; \
	fi

	@echo
	@echo "Installed binaries to: $(DESTDIR)$(BINDIR)"
	@echo "Installed tone files to: $(DESTDIR)$(DATADIR)"
	@echo "Installed GNOME Shell extension to: $(GNOME_EXTENSION_DST)"
	@echo "Installed GNOME-aware user service to: $(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Installed/updated env file: $(USER_CONFIG_DIR)/$(ENV_NAME)"
	@echo "Target user: $(REAL_USER)"
	@echo
	@echo "IMPORTANT: log out of the GNOME session and log back in so GNOME Shell reloads the extension."
	@echo
	@echo "After logging back in, run:"
	@echo "  gnome-extensions enable $(GNOME_EXTENSION_UUID)"
	@echo "  systemctl --user daemon-reload"
	@echo "  systemctl --user enable --now $(SERVICE_NAME)"
	@echo
	@echo "This service expects a TPM/systemd credential at:"
	@echo "  $(USER_CONFIG_DIR)/word_of_day.cred"
	@echo
	@echo "If keyboard or HID PTT is used, make sure that user can read the needed /dev/input/event* device:"
	@echo "  sudo usermod -aG input $(REAL_USER)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/ptt_client"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_wav_gate"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_server"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_helper"
	rm -f "$(DESTDIR)$(BINDIR)/ptt_hid"
	rm -f "$(DESTDIR)$(DATADIR)/start.wav"
	rm -f "$(DESTDIR)$(DATADIR)/stop.wav"
	rmdir "$(DESTDIR)$(DATADIR)" 2>/dev/null || true
	rm -f "$(USER_SYSTEMD_DIR)/$(SERVICE_NAME)"
	@echo "Removed binaries, tone files, and user service."
	@echo "Env file left in place: $(USER_CONFIG_DIR)/$(ENV_NAME)"

uninstall-gnome-extension:
	rm -rf "$(GNOME_EXTENSION_DST)"
	@echo "Removed GNOME Shell extension: $(GNOME_EXTENSION_DST)"
	@echo "Log out and back into GNOME, or disable it with:"
	@echo "  gnome-extensions disable $(GNOME_EXTENSION_UUID)"

clean:
	rm -f $(TARGETS) tests/test_telemetry tests/test_telemetry_sanitize tools/telemetry_lab tests/test_recovery tests/test_client tests/test_recovery_sanitize tests/test_client_sanitize tests/ptt_server_sanitize tests/ptt_wav_gate_sanitize

.PHONY: all install install-gnome-extension uninstall uninstall-gnome-extension clean

TEST_CFLAGS = $(CFLAGS) -Werror -I.
tests/test_recovery: tests/test_recovery.c ptt_jitter.c ptt_telemetry.c ptt_jitter.h ptt_telemetry.h ptt_protocol.h
	$(CC) $(TEST_CFLAGS) $(OPUS_CFLAGS) -o $@ tests/test_recovery.c ptt_jitter.c ptt_telemetry.c $(OPUS_LIBS) -lm

tests/test_client: tests/test_client.c ptt_client.c ptt_wav_io.h ptt_jitter.c ptt_telemetry.c ptt_jitter.h ptt_telemetry.h ptt_protocol.h
	$(CC) $(TEST_CFLAGS) $(CLIENT_CFLAGS) -o $@ tests/test_client.c ptt_jitter.c ptt_telemetry.c $(CLIENT_LIBS) -lm

tests/test_telemetry: tests/test_telemetry.c ptt_telemetry.c ptt_telemetry.h
	$(CC) $(TEST_CFLAGS) $(OPUS_CFLAGS) -o $@ tests/test_telemetry.c ptt_telemetry.c $(OPUS_LIBS) -lm

test: all tests/test_recovery tests/test_client tests/test_telemetry
	./tests/test_telemetry
	./tests/test_recovery
	./tests/test_client
	python3 tests/test_server.py
	python3 tests/test_wav_gate.py

.PHONY: test

SANITIZE_FLAGS = -O1 -g -Wall -Wextra -Werror -pthread -I. -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize:
	$(CC) $(SANITIZE_FLAGS) $(OPUS_CFLAGS) -o tests/test_telemetry_sanitize tests/test_telemetry.c ptt_telemetry.c $(OPUS_LIBS) -lm
	./tests/test_telemetry_sanitize
	$(CC) $(SANITIZE_FLAGS) $(OPUS_CFLAGS) -o tests/test_recovery_sanitize tests/test_recovery.c ptt_jitter.c ptt_telemetry.c $(OPUS_LIBS) -lm
	$(CC) $(SANITIZE_FLAGS) $(CLIENT_CFLAGS) -o tests/test_client_sanitize tests/test_client.c ptt_jitter.c ptt_telemetry.c $(CLIENT_LIBS) -lm
	$(CC) $(SANITIZE_FLAGS) -o tests/ptt_server_sanitize ptt_server.c
	$(CC) $(SANITIZE_FLAGS) $(CLIENT_CFLAGS) -o tests/ptt_wav_gate_sanitize ptt_wav_gate.c ptt_jitter.c ptt_telemetry.c $(CLIENT_LIBS) -lm
	./tests/test_recovery_sanitize
	ASAN_OPTIONS=detect_leaks=0 ./tests/test_client_sanitize
	PTT_SERVER=./tests/ptt_server_sanitize python3 tests/test_server.py
	ASAN_OPTIONS=detect_leaks=0 PTT_WAV_GATE=./tests/ptt_wav_gate_sanitize PTT_SERVER=./tests/ptt_server_sanitize python3 tests/test_wav_gate.py

.PHONY: sanitize

tools/telemetry_lab: tools/telemetry_lab.c ptt_telemetry.c ptt_telemetry.h
	$(CC) $(TEST_CFLAGS) $(OPUS_CFLAGS) -o $@ tools/telemetry_lab.c ptt_telemetry.c $(OPUS_LIBS) -lm
