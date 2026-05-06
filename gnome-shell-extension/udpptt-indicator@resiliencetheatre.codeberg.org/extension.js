/* SPDX-License-Identifier: GPL-3.0-or-later */

import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import St from 'gi://St';

import {Extension, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';

const POLL_INTERVAL_MS = 250;

function defaultStatePath() {
    const runtimeDir = GLib.get_user_runtime_dir();
    return GLib.build_filenamev([runtimeDir, 'udpptt', 'state']);
}

function readTextFile(path) {
    try {
        const [ok, bytes] = GLib.file_get_contents(path);
        if (!ok || !bytes)
            return null;

        return new TextDecoder('utf-8').decode(bytes);
    } catch (e) {
        return null;
    }
}

function parseKeyValueState(text) {
    const state = {
        tx: false,
        rx: false,
        txid: '',
        talker: '',
        firstLine: '',
    };

    if (!text)
        return state;

    const lines = text.split(/\r?\n/).map(line => line.trim()).filter(line => line.length > 0);
    if (lines.length === 0)
        return state;

    state.firstLine = lines[0];

    for (const line of lines) {
        const idx = line.indexOf('=');
        if (idx <= 0)
            continue;

        const key = line.slice(0, idx).trim().toLowerCase();
        const value = line.slice(idx + 1).trim();

        if (key === 'tx')
            state.tx = value === '1' || value.toLowerCase() === 'true' || value.toLowerCase() === 'down';
        else if (key === 'rx')
            state.rx = value === '1' || value.toLowerCase() === 'true' || value.toLowerCase() === 'down';
        else if (key === 'txid')
            state.txid = value;
        else if (key === 'talker')
            state.talker = value;
    }

    // Backward compatibility with old one-line state files:
    //   DOWN
    //   UP
    //   PTT DOWN txid=Alpha time_ms=123
    //   PTT UP txid=Alpha time_ms=123
    const first = state.firstLine.toUpperCase();
    if (first === 'DOWN' || first.startsWith('DOWN ') || first === 'PTT DOWN' || first.startsWith('PTT DOWN '))
        state.tx = true;
    else if (first === 'UP' || first.startsWith('UP ') || first === 'PTT UP' || first.startsWith('PTT UP '))
        state.tx = false;

    return state;
}

function displayForState(state) {
    // Local TX wins. If the user is transmitting and also receiving/suppressing
    // remote audio, the UI should still say SPEAK/TX.
    if (state.tx)
        return {visible: true, text: ' SPEAK ', styleClass: 'udpptt-tx-label'};

    if (state.rx) {
        const talker = state.talker ? state.talker : '?';
        return {visible: true, text: ` RX ${talker} `, styleClass: 'udpptt-rx-label'};
    }

    return {visible: false, text: '', styleClass: 'udpptt-idle-label'};
}

export default class UdppttIndicatorExtension extends Extension {
    enable() {
        this._statePath = defaultStatePath();
        this._lastVisible = false;
        this._lastText = null;
        this._lastStyleClass = null;

        this._indicator = new PanelMenu.Button(0.0, _('udpptt PTT'), false);
        this._label = new St.Label({
            text: ' SPEAK ',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'udpptt-tx-label',
        });

        this._indicator.add_child(this._label);
        this._indicator.hide();

        Main.panel.addToStatusArea('udpptt-ptt-indicator', this._indicator, 0, 'right');

        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, POLL_INTERVAL_MS, () => {
            this._refreshState();
            return GLib.SOURCE_CONTINUE;
        });

        this._refreshState();
    }

    disable() {
        if (this._pollId) {
            GLib.Source.remove(this._pollId);
            this._pollId = 0;
        }

        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }

        this._label = null;
    }

    _refreshState() {
        const text = readTextFile(this._statePath);
        const state = parseKeyValueState(text);
        const display = displayForState(state);

        if (this._label && display.text !== this._lastText) {
            this._label.set_text(display.text);
            this._lastText = display.text;
        }

        if (this._label && display.styleClass !== this._lastStyleClass) {
            if (this._lastStyleClass)
                this._label.remove_style_class_name(this._lastStyleClass);
            this._label.add_style_class_name(display.styleClass);
            this._lastStyleClass = display.styleClass;
        }

        if (this._indicator && display.visible !== this._lastVisible) {
            if (display.visible)
                this._indicator.show();
            else
                this._indicator.hide();
            this._lastVisible = display.visible;
        }
    }
}
