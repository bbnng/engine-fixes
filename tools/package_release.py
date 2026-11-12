"""Build the release archive from build/engine_fixes.dll and package/.

Usage (after build.cmd succeeds): python tools/package_release.py
Writes dist/EngineFixes-<version>.zip: a BLSE mod folder under Bin/, merged into the game folder.
"""
import os
import re
import shutil
import zipfile

version = re.search(r'BLSE_PLUGIN_INFO\("engine_fixes", "([^"]+)"',
                    open(os.path.join('src', 'engine_fixes.cpp'), encoding='utf-8').read()).group(1)
for name in ('mod.ini', 'README.txt'):
    if version not in open(os.path.join('package', name), encoding='utf-8').read():
        raise SystemExit(f'package/{name} does not mention version {version}')


def crlf(src, dst):
    text = open(src, encoding='utf-8').read().replace('\r\n', '\n').replace('\n', '\r\n')
    open(dst, 'w', encoding='utf-8', newline='').write(text)


stage = os.path.join('dist', f'EngineFixes-{version}')
shutil.rmtree(stage, ignore_errors=True)
mod = os.path.join(stage, 'Bin', 'loader', 'blse', 'mods', 'engine_fixes')
os.makedirs(os.path.join(mod, 'blse', 'plugins'))
shutil.copy2(os.path.join('build', 'engine_fixes.dll'), os.path.join(mod, 'blse', 'plugins', 'engine_fixes.dll'))
for name in ('mod.ini', 'engine_fixes.ini', 'README.txt'):
    crlf(os.path.join('package', name), os.path.join(mod, name))
crlf(os.path.join('package', 'README.txt'), os.path.join(stage, 'Engine Fixes README.txt'))

archive = os.path.join('dist', f'EngineFixes-{version}.zip')
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for folder, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(folder, f)
            z.write(path, os.path.relpath(path, stage).replace(os.sep, '/'))
print(archive)
