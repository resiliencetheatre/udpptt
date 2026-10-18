#!/usr/bin/env python3
"""Install the local AltGr desktop service; never start it automatically."""
import grp
import os
from pathlib import Path
import pwd
import re
import subprocess
import tempfile


def run(args, privileged=False, user=False):
    if privileged and os.geteuid() != 0:
        args = ['sudo', '--'] + args
    if user and os.geteuid() == 0:
        args = ['runuser', '-u', account.pw_name, '--'] + args
    subprocess.run(args, check=True)


def write(path, text, mode):
    # Atomic replacement also avoids following an existing destination symlink.
    fd, tmp = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(text)
            os.fchmod(stream.fileno(), mode)
            if os.geteuid() == 0:
                os.fchown(stream.fileno(), account.pw_uid, account.pw_gid)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def quote(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    global account
    account = pwd.getpwnam(os.environ.get('SUDO_USER') or pwd.getpwuid(os.getuid()).pw_name)
    if account.pw_uid == 0:
        raise ValueError('Run make install as your desktop user (sudo make install is also supported).')
    values = {key: os.environ.get(key, '') for key in ('SERVER_IP', 'CALL_SIGN', 'WORD_OF_DAY', 'DPI_ID')}
    for key, value in values.items():
        if not value or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError(f'{key} must be supplied as a nonempty single-line Make variable.')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,63}', values['DPI_ID']):
        raise ValueError('DPI_ID must be 1..63 characters: A-Z, a-z, 0-9, dot, underscore or hyphen.')
    if os.environ.get('DESTDIR'):
        raise ValueError('make install configures a live desktop; DESTDIR is not supported.')
    paths = {key: Path(os.environ[key]) for key in ('BINDIR', 'DATADIR', 'USER_SYSTEMD_DIR', 'USER_CONFIG_DIR')}
    for path in paths.values():
        if not path.is_absolute() or not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(path)):
            raise ValueError('Installation directories must be absolute paths without spaces or systemd metacharacters.')
    env_name, service_name = os.environ['ENV_NAME'], os.environ['SERVICE_NAME']
    for name in (env_name, service_name):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', name):
            raise ValueError('Service and environment filenames must be plain filenames.')
    config, units = paths['USER_CONFIG_DIR'], paths['USER_SYSTEMD_DIR']
    secret, public = config / 'dpi.secret', config / 'dpi.public'
    for key in (secret, public):
        if key.is_symlink():
            raise ValueError(f'Refusing symlink key: {key}')
    if secret.exists() != public.exists():
        raise ValueError('Incomplete DPI key pair; restore the missing key before reinstalling. Existing identity was preserved.')
    for key in (secret, public):
        if key.exists() and not re.fullmatch(rb'[0-9a-f]{64}\n', key.read_bytes()):
            raise ValueError(f'Invalid DPI key file: {key}')
    input_group = grp.getgrnam('input')
    added = input_group.gr_gid not in os.getgrouplist(account.pw_name, account.pw_gid)
    for directory in (config, units):
        # Create parents as the desktop user, including when invoked through sudo.
        run(['mkdir', '-p', str(directory)], user=True)
    for directory in (paths['BINDIR'], paths['DATADIR']):
        run(['install', '-d', str(directory)], privileged=True)
    for binary in ('ptt_dpi_keygen', 'ptt_client', 'ptt_wav_gate', 'ptt_server', 'ptt_helper', 'ptt_hid'):
        run(['install', '-m', '0755', binary, str(paths['BINDIR'] / binary)], privileged=True)
    for tone in ('start.wav', 'stop.wav'):
        run(['install', '-m', '0644', tone, str(paths['DATADIR'] / tone)], privileged=True)
    if not secret.exists():
        run([str(Path('ptt_dpi_keygen').resolve()), '--secret', str(secret), '--public', str(public)], user=True)
    run(['chmod', '0600', str(secret)], user=True)
    if added:
        run(['usermod', '-aG', 'input', account.pw_name], privileged=True)
    env = Path('systemd/udpptt.env').read_text()
    for key, value in values.items():
        env = re.sub(r'^' + key + r'=.*$', lambda m: key + '=' + quote(value), env, flags=re.M)
    service = Path('systemd/udpptt.service').read_text()
    service = service.replace('/opt/udpptt', str(paths['DATADIR'])).replace('/usr/local/bin', str(paths['BINDIR']))
    service = service.replace('%h/.config/udpptt/udpptt.env', str(config / env_name))
    service = service.replace('--dpi-id ${DPI_ID}', '--dpi-id ${DPI_ID} --dpi-secret ' + str(secret))
    write(config / env_name, env, 0o600)
    write(units / service_name, service, 0o644)
    runtime = f'/run/user/{account.pw_uid}'
    run(['env', f'XDG_RUNTIME_DIR={runtime}', f'DBUS_SESSION_BUS_ADDRESS=unix:path={runtime}/bus',
         'systemctl', '--user', 'daemon-reload'], user=True)
    print(f'Installed AltGr PTT for {account.pw_name}: {units / service_name}')
    print(f'Configuration: {config / env_name} (mode 0600); DPI keys: {secret}, {public}')
    print('AltGr delay: 1000 ms; audio preamble: ABCDE. Other PTT modes require manual configuration.')
    print('Input group: ' + ('user added.' if added else 'already a member.'))
    print('Reboot before using AltGr PTT so input-group membership takes effect.')
    print(f'Then start manually: systemctl --user start {service_name.removesuffix(".service")}')
    print(f'View logs: journalctl --user -u {service_name} -f')
    print('The service has not been started or enabled; an already-running service needs a restart to load changes.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Installation failed: {error}')
