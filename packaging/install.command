#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
cd "$(dirname "$0")"
case "${1:-}" in
    ''|--dry-run) ;;
    *) echo 'Usage: bash install.command [--dry-run]' >&2; exit 2 ;;
esac
if [ ! -f SHA256SUMS ] || [ ! -d payload ]; then
    echo '配布ZIPをすべて展開したフォルダで実行してください（スクリプトだけでは導入できません）。' >&2
    exit 1
fi
shasum -a 256 -c SHA256SUMS
for required in \
    'payload/Library/Audio/Plug-Ins/HAL/Audio8DJ.driver/Contents/MacOS/Audio8DJ' \
    'payload/usr/local/bin/a8dj-midid' \
    'payload/Library/LaunchAgents/com.sawada.audio8dj.midid.plist' \
    'payload/Library/Application Support/Audio8DJ/manage.sh'; do
    [ -f "$required" ] || { echo "不足: $required" >&2; exit 1; }
done
codesign --verify --strict 'payload/Library/Audio/Plug-Ins/HAL/Audio8DJ.driver'
codesign --verify --strict payload/usr/local/bin/a8dj-midid
if [ "${1:-}" = --dry-run ]; then
    bash 'payload/Library/Application Support/Audio8DJ/manage.sh' --dry-run
    exit 0
fi
major=$(sw_vers -productVersion | cut -d. -f1)
if [ "$major" -lt 12 ]; then
    echo 'macOS 12以降が必要です。' >&2; exit 1
fi
case "$(uname -m)" in arm64|x86_64) ;; *) echo '未対応のCPUです。' >&2; exit 1 ;; esac
echo 'Audio 8 DJの音声・MIDIをインストールします。Mac全体の音声が一時停止します。'
if [ "$(id -u)" -ne 0 ]; then
    exec sudo /bin/bash "$PWD/install.command"
fi
bash 'payload/Library/Application Support/Audio8DJ/manage.sh' prepare
for destination in \
    '/Library/Audio/Plug-Ins/HAL/Audio8DJ.driver' \
    '/Library/Application Support/Audio8DJ' \
    '/Library/LaunchAgents/com.sawada.audio8dj.midid.plist' \
    '/usr/local/bin/a8dj-midid'; do
    mkdir -p "$(dirname "$destination")"
    # Replace the complete bundle so obsolete files cannot survive an update.
    if [ "$destination" = /Library/Audio/Plug-Ins/HAL/Audio8DJ.driver ]; then
        rm -rf "$destination"
    fi
    ditto "payload$destination" "$destination"
done
bash '/Library/Application Support/Audio8DJ/manage.sh' activate
