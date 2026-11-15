"""Build the release archives from build/engine_fixes.dll, package/ and the source tree.

Usage (after build.cmd succeeds): python tools/package_release.py
Writes to dist/:
  EngineFixes-<version>.zip         a BLSE mod folder and the startup companion under Bin/,
                                    merged into the game folder
  EngineFixes-<version>-source.zip  the complete source (the GPL's Corresponding Source)
"""
import os
import re
import shutil
import subprocess
import zipfile

version = re.search(r'BLSE_PLUGIN_INFO\("engine_fixes", "([^"]+)"',
                    open(os.path.join('src', 'engine_fixes.cpp'), encoding='utf-8-sig').read()).group(1)
for name in ('mod.ini', 'README.txt', 'engine_fixes.ini', 'engine_fixes_intro.ini'):
    if version not in open(os.path.join('package', name), encoding='utf-8').read():
        raise SystemExit(f'package/{name} does not mention version {version}')
if f'intro companion {version}'.encode() not in open(os.path.join('build', 'engine_fixes_intro.vtm'), 'rb').read():
    raise SystemExit(f'build/engine_fixes_intro.vtm is not version {version}')


def crlf(src, dst):
    text = open(src, encoding='utf-8').read().replace('\r\n', '\n').replace('\n', '\r\n')
    open(dst, 'w', encoding='utf-8', newline='').write(text)


def zipdir(folder, archive):
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for parent, dirs, files in os.walk(folder):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(parent, f)
                z.write(path, os.path.relpath(path, folder).replace(os.sep, '/'))
    print(archive)


os.makedirs('dist', exist_ok=True)

# Install archive.
stage = os.path.join('dist', f'EngineFixes-{version}')
shutil.rmtree(stage, ignore_errors=True)
mod = os.path.join(stage, 'Bin', 'loader', 'blse', 'mods', 'engine_fixes')
os.makedirs(os.path.join(mod, 'blse', 'plugins'))
shutil.copy2(os.path.join('build', 'engine_fixes.dll'), os.path.join(mod, 'blse', 'plugins', 'engine_fixes.dll'))
for name in ('mod.ini', 'engine_fixes.ini', 'README.txt'):
    crlf(os.path.join('package', name), os.path.join(mod, name))
crlf('LICENSE', os.path.join(mod, 'LICENSE.txt'))
crlf('NOTICE', os.path.join(mod, 'NOTICE.txt'))
crlf(os.path.join('include', 'BLSE-LICENSE.txt'), os.path.join(mod, 'BLSE-LICENSE.txt'))
loader = os.path.join(stage, 'Bin', 'loader')
shutil.copy2(os.path.join('build', 'engine_fixes_intro.vtm'), os.path.join(loader, 'engine_fixes_intro.vtm'))
crlf(os.path.join('package', 'engine_fixes_intro.ini'), os.path.join(loader, 'engine_fixes_intro.ini'))
crlf(os.path.join('package', 'README.txt'), os.path.join(stage, 'Engine Fixes and Tweaks README.txt'))
zipdir(stage, os.path.join('dist', f'EngineFixes-{version}.zip'))

# Source archive: every file tracked by git.
source = os.path.join('dist', f'EngineFixes-{version}-source')
shutil.rmtree(source, ignore_errors=True)
tracked = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True).stdout.split('\n')
for path in filter(None, tracked):
    os.makedirs(os.path.join(source, os.path.dirname(path)), exist_ok=True)
    shutil.copy2(path, os.path.join(source, path))
zipdir(source, os.path.join('dist', f'EngineFixes-{version}-source.zip'))
