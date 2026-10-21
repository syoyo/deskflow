#!/usr/bin/env python3
"""Copy the current Flatpak/native configuration into an isolated test profile."""
from pathlib import Path
import configparser
import shutil

project = Path(__file__).resolve().parent
destination = project / 'run/config/Deskflow'
if (destination / 'Deskflow.conf').exists():
    raise SystemExit(f'Test profile already exists: {destination}; leaving it unchanged.')
home = Path.home()
sources = [home / '.var/app/org.deskflow.deskflow/config/Deskflow', home / '.config/Deskflow']
source = next((p for p in sources if (p / 'Deskflow.conf').exists()), None)
if source is None:
    raise SystemExit('No existing Deskflow configuration found.')
destination.mkdir(parents=True, mode=0o700)
destination.parent.chmod(0o700)
shutil.copytree(source, destination, dirs_exist_ok=True)
settings = configparser.ConfigParser(interpolation=None, strict=False)
settings.optionxform = str
settings.read(destination / 'Deskflow.conf')
for section in ('gui', 'log', 'server'):
    if not settings.has_section(section):
        settings.add_section(section)
settings['gui']['startCoreWithGui'] = 'false'
settings['log']['toFile'] = 'true'
settings['log']['file'] = str(project / 'run/deskflow-xclip.log')
settings['log']['level'] = 'DEBUG'
settings['server']['externalConfigFile'] = str(destination / 'deskflow-server.conf')
# Avoid touching certificate paths outside this private profile.
if settings.has_section('security'):
    old_certificate = settings['security'].get('certificate', '')
    certificate = Path(old_certificate) if old_certificate else None
    if certificate and certificate.is_file():
        target = destination / 'tls/test-profile.pem'
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(certificate, target)
        target.chmod(0o600)
        settings['security']['certificate'] = str(target)
with (destination / 'Deskflow.conf').open('w') as file:
    settings.write(file, space_around_delimiters=False)
(destination / 'Deskflow.conf').chmod(0o600)
print(f'Copied {source} to {destination}. Server autostart disabled; click Start when ready.')
