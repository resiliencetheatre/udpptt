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

export default class UdppttIndicatorExtension extends Extension {
    enable() {
        this._statePath = defaultStatePath();
        this._active = false;

        this._indicator = new PanelMenu.Button(0.0, _('udpptt PTT'), false);
        this._label = new St.Label({
            text: ' SPEAK ',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'udpptt-ptt-label',
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
        const line = text ? text.trim().toUpperCase() : 'UP';

        // Supported state-file formats:
        //   DOWN
        //   UP
        //   PTT DOWN txid=Alpha time_ms=123
        //   PTT UP txid=Alpha time_ms=123
        const active = line === 'DOWN' || line.startsWith('DOWN ') ||
            line === 'PTT DOWN' || line.startsWith('PTT DOWN ');

        if (active === this._active)
            return;

        this._active = active;

        if (this._indicator) {
            if (active)
                this._indicator.show();
            else
                this._indicator.hide();
        }
    }
}
