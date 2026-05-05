# udpptt GNOME Shell indicator demo

This is a small GNOME Shell extension for desktop demos. It watches:

```text
/run/user/$UID/udpptt/state
```

When the file says `DOWN` or `PTT DOWN ...`, the extension shows a red `SPEAK` badge in the GNOME top panel. When the file says `UP`, the badge is hidden.

## Install locally

```sh
UUID=udpptt-indicator@resiliencetheatre.codeberg.org
mkdir -p ~/.local/share/gnome-shell/extensions
cp -r "$UUID" ~/.local/share/gnome-shell/extensions/

gnome-extensions enable "$UUID"
```

Log out and back in if GNOME does not see the extension immediately.

## Test without udpptt

```sh
mkdir -p "$XDG_RUNTIME_DIR/udpptt"
printf 'PTT DOWN txid=test time_ms=0\n' > "$XDG_RUNTIME_DIR/udpptt/state"
sleep 3
printf 'PTT UP txid=test time_ms=0\n' > "$XDG_RUNTIME_DIR/udpptt/state"
```

## Run with ptt_client

Start `ptt_client` with the optional state-file output:

```sh
./ptt_client 198.51.100.10 \
    --txid Alpha \
    --ptt-socket /tmp/udpptt.sock \
    --state-file "$XDG_RUNTIME_DIR/udpptt/state" \
    --encrypt --key 'shared room secret'
```

This option has no GNOME dependency. It only writes a small text file when the effective PTT state changes.
