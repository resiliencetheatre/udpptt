# udpptt GNOME Shell indicator demo

This is a small GNOME Shell extension for desktop demos. It watches:

```text
/run/user/$UID/udpptt/state
```

It supports both the older one-line TX-only state file and the newer multi-line TX/RX state file.

## Display behavior

- `tx=1` or `PTT DOWN ...` shows a red `SPEAK` badge.
- `rx=1` shows a blue `RX <talker>` badge.
- TX wins over RX if both are active.
- Idle state hides the badge.

Expected new state-file format:

```text
PTT UP txid=Alpha time_ms=1777965000000
tx=0
rx=1
txid=Alpha
talker=Bravo
time_ms=1777965000000
rx_timeout_ms=1000
rx_last_audio_ms=1777964999500
```

The first line is kept for compatibility with older extension versions.

## Install locally

```sh
UUID=udpptt-indicator@resiliencetheatre.codeberg.org
mkdir -p ~/.local/share/gnome-shell/extensions
rm -rf ~/.local/share/gnome-shell/extensions/"$UUID"
cp -r "$UUID" ~/.local/share/gnome-shell/extensions/

gnome-extensions enable "$UUID"
```

Log out and back in if GNOME does not see the extension immediately.

If the extension was already installed, restart it:

```sh
gnome-extensions disable udpptt-indicator@resiliencetheatre.codeberg.org
gnome-extensions enable udpptt-indicator@resiliencetheatre.codeberg.org
```

On GNOME Wayland, logging out and back in is the most reliable way to reload changed extension code.

## Test without udpptt

```sh
mkdir -p "$XDG_RUNTIME_DIR/udpptt"

cat > "$XDG_RUNTIME_DIR/udpptt/state" <<'EOF_STATE'
PTT DOWN txid=test time_ms=0
tx=1
rx=0
txid=test
talker=
time_ms=0
rx_timeout_ms=1000
rx_last_audio_ms=0
EOF_STATE

sleep 3

cat > "$XDG_RUNTIME_DIR/udpptt/state" <<'EOF_STATE'
PTT UP txid=test time_ms=0
tx=0
rx=1
txid=test
talker=Bravo
time_ms=0
rx_timeout_ms=1000
rx_last_audio_ms=0
EOF_STATE

sleep 3

cat > "$XDG_RUNTIME_DIR/udpptt/state" <<'EOF_STATE'
PTT UP txid=test time_ms=0
tx=0
rx=0
txid=test
talker=
time_ms=0
rx_timeout_ms=1000
rx_last_audio_ms=0
EOF_STATE
```

## Run with ptt_client

Start `ptt_client` with the optional state-file output:

```sh
./ptt_client 198.51.100.10 \
    --txid Alpha \
    --ptt-socket /tmp/udpptt.sock \
    --state-file "$XDG_RUNTIME_DIR/udpptt/state" \
    --rx-state-timeout-ms 1000 \
    --encrypt --key 'shared room secret'
```

For a user systemd service, use `%t` instead of `$XDG_RUNTIME_DIR`:

```ini
ExecStartPre=/usr/bin/mkdir -p %t/udpptt
ExecStart=/usr/local/bin/ptt_client ${SERVER_IP} --altgr-ptt-delay-ms ${ALTGR_PTT_DELAY_MS} --txid ${CALL_SIGN} --state-file %t/udpptt/state --rx-state-timeout-ms 1000 --encrypt --key ${WORD_OF_DAY}
```

This option has no GNOME dependency. It only writes a small text file with current TX/RX state.
