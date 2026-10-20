"""Build explicit release archives, excluding logs, backups and old firmware."""
import hashlib
import json
import shutil
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'release'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def archive(name, files):
    path = OUT/name
    with ZipFile(path, 'w', ZIP_DEFLATED, compresslevel=9) as z:
        for target, source in sorted(files.items()):
            if not source.is_file():
                raise FileNotFoundError(source)
            z.write(source, target)
    return path

def main():
    OUT.mkdir(exist_ok=True)
    common = {p: ROOT/p for p in ('LICENSE', 'THIRD_PARTY.md', 'README.md')}
    for folder in ('LICENSES', 'docs'):
        common.update({p.relative_to(ROOT).as_posix(): p for p in (ROOT/folder).glob('*') if p.is_file()})
    studio = ROOT/'dist/studio'
    app = dict(common)
    for name in ('WarlockStudio.exe', 'hidapi.dll', 'Qt5Core.dll', 'Qt5Gui.dll',
                 'Qt5Widgets.dll', 'usb_rates.exe', 'libEGL.dll', 'libGLESv2.dll'):
        app[name] = studio/name
    for folder in ('platforms', 'imageformats', 'styles'):
        app.update({p.relative_to(studio).as_posix(): p for p in (studio/folder).glob('*.dll')})
    firmware = dict(common)
    firmware['hidapi.dll'] = studio/'hidapi.dll'
    for p in (ROOT/'firmware/aw60').glob('*.py'):
        firmware[p.relative_to(ROOT).as_posix()] = p
    for name in ('manifest-rev9.json', '919_v104-aw60-rev9.bin', 'stream.c', 'offsets.h', 'patch.ld'):
        firmware['firmware/aw60/'+name] = ROOT/'firmware/aw60'/name
    for name in ('Protocol.h', 'PresetFormat.h', 'WarlockLayout.h', 'firmware/stock/README.md', 'firmware/requirements-dev.txt'):
        firmware[name] = ROOT/name
    image = ROOT/'firmware/aw60/919_v104-aw60-rev9.bin'
    manifest = json.loads((image.parent/'manifest-rev9.json').read_text())
    if digest(image) != manifest['patched_sha256']:
        raise ValueError('Firmware differs from validated manifest')
    assets = [archive('WarlockStudio-windows-x64.zip', app), archive('AW60-rev9-firmware.zip', firmware)]
    shutil.copy2(image, OUT/image.name)
    sums = ''.join(digest(p)+'  '+p.name+'\n' for p in assets)
    sums += digest(image)+'  919_v104-aw60-rev9.bin\n'
    (OUT/'SHA256SUMS.txt').write_text(sums, encoding='ascii')
    for p in assets:
        print(p.name, p.stat().st_size, digest(p))

if __name__ == '__main__':
    main()
