"""Exercise the live installer without modifying system files or memberships."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('installer', 'scripts/install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    settings = dict(SERVER_IP='198.51.100.10', CALL_SIGN='desktop',
                    WORD_OF_DAY='shared "room" \\ $literal', DPI_ID='desk',
                    BINDIR=str(root / 'bin'), DATADIR=str(root / 'data'),
                    USER_CONFIG_DIR=str(root / 'config'), USER_SYSTEMD_DIR=str(root / 'units'),
                    ENV_NAME='udpptt.env', SERVICE_NAME='udpptt.service', DESTDIR='')
    account = SimpleNamespace(pw_name='desktop', pw_uid=os.getuid() or 1000, pw_gid=os.getgid())
    calls = []

    def run(args, **kwargs):
        calls.append(args)
        if args[0] in ('mkdir', 'chmod') or args[0].endswith('/ptt_dpi_keygen'):
            subprocess.run(args, check=True, stdout=subprocess.DEVNULL)

    with patch.dict(os.environ, settings), patch.object(installer.pwd, 'getpwnam', return_value=account), \
         patch.object(installer.grp, 'getgrnam', return_value=SimpleNamespace(gr_gid=9999)), \
         patch.object(installer.os, 'getgrouplist', return_value=[]), patch.object(installer, 'run', side_effect=run):
        installer.main()
        config = root / 'config'
        first = (config / 'dpi.secret').read_bytes()
        assert len(first) == 65
        assert (config / 'udpptt.env').stat().st_mode & 0o777 == 0o600
        assert 'ALTGR_PTT_DELAY_MS=1000' in (config / 'udpptt.env').read_text()
        assert 'WORD_OF_DAY=' + installer.quote(settings['WORD_OF_DAY']) in (config / 'udpptt.env').read_text()
        unit = (root / 'units/udpptt.service').read_text()
        assert 'RuntimeDirectory=udpptt' in unit and str(config / 'dpi.secret') in unit
        assert ['usermod', '-aG', 'input', 'desktop'] in calls
        assert any(c[-3:] == ['systemctl', '--user', 'daemon-reload'] for c in calls)
        assert not any('start' in c for c in calls)
        calls.clear()
        with patch.object(installer.os, 'getgrouplist', return_value=[9999]):
            installer.main()
        assert (config / 'dpi.secret').read_bytes() == first
        assert not any(c[0] == 'usermod' or c[0].endswith('/ptt_dpi_keygen') for c in calls)
        (config / 'dpi.public').unlink()
        calls.clear()
        try:
            installer.main()
            raise AssertionError('accepted incomplete pair')
        except ValueError:
            assert not calls
        with patch.dict(os.environ, {'DPI_ID': 'invalid label'}):
            try:
                installer.main()
                raise AssertionError('accepted invalid label')
            except ValueError:
                assert not calls
print('Installer checks passed')
