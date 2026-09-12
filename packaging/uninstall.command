#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
cd "$(dirname "$0")"
manager='/Library/Application Support/Audio8DJ/manage.sh'
if [ ! -f "$manager" ]; then
    manager="$PWD/payload/Library/Application Support/Audio8DJ/manage.sh"
fi
if [ ! -f "$manager" ]; then
    echo 'アンインストーラーがありません。配布ZIPを展開して実行してください。' >&2; exit 1
fi
case "${1:-}" in
    --dry-run) exec /bin/bash "$manager" --dry-run ;;
    '') ;;
    *) echo 'Usage: bash uninstall.command [--dry-run]' >&2; exit 2 ;;
esac
if [ "$(id -u)" -ne 0 ]; then
    exec sudo /bin/bash "$PWD/uninstall.command"
fi
bash "$manager" uninstall
