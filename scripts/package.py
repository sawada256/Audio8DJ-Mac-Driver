#!/usr/bin/env python3
"""Build two offline installers from exactly the same prebuilt payload."""
import hashlib
import plistlib
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
DIST = ROOT / 'dist'
VERSION = (ROOT / 'VERSION').read_text().strip()
if not re.fullmatch(r'\d+\.\d+\.\d+', VERSION):
    raise SystemExit('VERSION must be major.minor.patch')
NAME = f'Audio8DJ-Mac-Driver-{VERSION}'
STAGE = BUILD / 'release' / NAME
PAYLOAD = STAGE / 'payload'
IDENTIFIER = 'com.sawada.audio8dj.installer'


def run(*args):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT)


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, target, ignore=shutil.ignore_patterns(".DS_Store", "__MACOSX"))
    else:
        shutil.copy2(source, target)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    bundle = BUILD / 'Audio8DJ.driver'
    midi = BUILD / 'bin/a8dj-midid'
    info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
    if info['CFBundleShortVersionString'] != VERSION:
        raise SystemExit('VERSION and plugin/Info.plist do not match')
    for binary in (bundle / 'Contents/MacOS/Audio8DJ', midi):
        run('lipo', binary, '-verify_arch', 'arm64', 'x86_64')
        run('codesign', '--verify', '--strict', binary)
    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True)
    DIST.mkdir(exist_ok=True)
    copy(bundle, PAYLOAD / 'Library/Audio/Plug-Ins/HAL/Audio8DJ.driver')
    copy(midi, PAYLOAD / 'usr/local/bin/a8dj-midid')
    copy(ROOT / 'packaging/com.sawada.audio8dj.midid.plist',
         PAYLOAD / 'Library/LaunchAgents/com.sawada.audio8dj.midid.plist')
    support = PAYLOAD / 'Library/Application Support/Audio8DJ'
    copy(ROOT / 'packaging/manage.sh', support / 'manage.sh')
    (support / 'README.txt').write_text(
        'Audio 8 DJ Apple Silicon compatibility driver\n'
        f'Version {VERSION}; unsigned installer, ad-hoc signed binaries.\n'
        'Uninstall (interrupts system audio):\n'
        "sudo bash '/Library/Application Support/Audio8DJ/manage.sh' uninstall\n"
        'Backups and latency settings are preserved.\n')
    for name in ('install.command', 'uninstall.command'):
        copy(ROOT / 'packaging' / name, STAGE / name)
    copy(ROOT / 'scripts/set-latency.sh', STAGE / 'set-latency.command')
    copy(ROOT / 'tools/a8dj-watch.sh', STAGE / 'watch-status.command')
    for name in ('README.md', 'README.en.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md'):
        copy(ROOT / name, STAGE / name)
    copy(ROOT / 'docs', STAGE / 'docs')
    copy(ROOT / 'firmware/README.md', STAGE / 'firmware/README.md')
    # Zip readers preserve these Unix mode bits; bash invocation also works.
    for path in STAGE.glob('*.command'):
        path.chmod(0o755)
    for path in PAYLOAD.rglob('*'):
        if path.is_dir():
            path.chmod(0o755)
        elif path.name in ('Audio8DJ', 'a8dj-midid', 'manage.sh'):
            path.chmod(0o755)
        else:
            path.chmod(0o644)
    files = sorted(p for p in STAGE.rglob('*') if p.is_file() and p.name != '.DS_Store')
    (STAGE / 'SHA256SUMS').write_text(''.join(
        f'{sha(p)}  {p.relative_to(STAGE).as_posix()}\n' for p in files))

    workspace = BUILD / 'package'
    if workspace.exists():
        shutil.rmtree(workspace)
    workspace.mkdir()
    pkg_scripts = workspace / 'scripts'
    shutil.copytree(ROOT / 'packaging/scripts', pkg_scripts, ignore=shutil.ignore_patterns('.DS_Store'))
    copy(ROOT / 'packaging/manage.sh', pkg_scripts / 'manage.sh')
    for path in pkg_scripts.iterdir():
        path.chmod(0o755)
    components = workspace / 'components.plist'
    run('pkgbuild', '--analyze', '--root', PAYLOAD, components)
    data = plistlib.loads(components.read_bytes())
    for item in data:
        item['BundleIsRelocatable'] = False
        item['BundleIsVersionChecked'] = False
        item['BundleOverwriteAction'] = 'upgrade'
    components.write_bytes(plistlib.dumps(data))
    component = workspace / 'Audio8DJ-component.pkg'
    run('pkgbuild', '--root', PAYLOAD, '--component-plist', components,
        '--scripts', pkg_scripts, '--identifier', IDENTIFIER,
        '--version', VERSION, '--install-location', '/',
        '--ownership', 'recommended', '--filter', r'\.DS_Store$', component)
    resources = workspace / 'resources'
    resources.mkdir()
    (resources / 'ReadMe.html').write_text('''<!doctype html><html lang="ja"><meta charset="utf-8">
<h1>Audio 8 DJ Mac Driver</h1>
<p>Audio 8 DJ用の非公式オーディオ・MIDIドライバー。macOS 12以降。</p>
<p>DAWを終了してください。導入時にMac全体の音声が一時停止します。再ビルド不要です。</p>
<p>MIDIはログインユーザーのバックグラウンドエージェントを使用します。必要に応じてシステム設定のログイン項目で実行を許可してください。</p>
<p>Developer ID署名・Apple公証はありません。同梱ファームウェアはMIT対象外です。</p>
<h2>English</h2>
<p>Unofficial audio/MIDI driver for Audio 8 DJ, targeting macOS 12 or later.</p>
<p>Quit your DAW first; installation briefly interrupts system audio. No rebuilding required.</p>
<p>MIDI uses a per-user background agent. Allow background activity in System Settings if required.</p>
<p>No Developer ID signature or Apple notarization. Bundled firmware is excluded from MIT.</p>
<p>削除 / Uninstall:</p>
<pre>sudo bash '/Library/Application Support/Audio8DJ/manage.sh' uninstall</pre>
</html>''')
    # Include both the original-code license and the firmware exclusion in UI.
    (resources / 'License.txt').write_text((ROOT / 'LICENSE').read_text() + '\n\n' +
                                          (ROOT / 'THIRD_PARTY_NOTICES.md').read_text())
    distribution = workspace / 'Distribution.xml'
    distribution.write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
<title>Audio 8 DJ Apple Silicon 互換ドライバー {VERSION}</title>
<options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
<domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
<volume-check><allowed-os-versions><os-version min="12.0"/></allowed-os-versions></volume-check>
<readme file="ReadMe.html" mime-type="text/html"/>
<license file="License.txt" mime-type="text/plain"/>
<choices-outline><line choice="default"/></choices-outline>
<choice id="default" title="Audio 8 DJ audio and MIDI"><pkg-ref id="{IDENTIFIER}"/></choice>
<pkg-ref id="{IDENTIFIER}" version="{VERSION}" auth="Root">Audio8DJ-component.pkg</pkg-ref>
</installer-gui-script>''')
    pkg = DIST / f'{NAME}-unsigned.pkg'
    run('productbuild', '--distribution', distribution, '--resources', resources,
        '--package-path', workspace, pkg)
    archive = DIST / f'{NAME}.zip'
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
        for path in files + [STAGE / 'SHA256SUMS']:
            output.write(path, path.relative_to(STAGE.parent).as_posix())
    (DIST / 'SHA256SUMS.txt').write_text(''.join(
        f'{sha(p)}  {p.name}\n' for p in (pkg, archive)))
    print(f'Release artifacts: {DIST}')


if __name__ == '__main__':
    main()
