#!/bin/bash
# Shared by the ZIP installer and the pkg scripts. Never disables Gatekeeper.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
SUPPORT='/Library/Application Support/Audio8DJ'
DRIVER='/Library/Audio/Plug-Ins/HAL/Audio8DJ.driver'
AGENT='/Library/LaunchAgents/com.sawada.audio8dj.midid.plist'
BINARY='/usr/local/bin/a8dj-midid'
LABEL='com.sawada.audio8dj.midid'

console_account() {
    CONSOLE_USER=$(stat -f '%Su' /dev/console)
    CONSOLE_UID=$(stat -f '%u' /dev/console)
    CONSOLE_HOME=''
    if [ "$CONSOLE_UID" -ge 501 ] && [ "$CONSOLE_USER" != loginwindow ]; then
        CONSOLE_HOME=$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory | sed 's/^NFSHomeDirectory: //')
    fi
}
stop_agent() {
    if [ -n "$CONSOLE_HOME" ]; then
        launchctl bootout "gui/$CONSOLE_UID/$LABEL" 2>/dev/null || true
    fi
}
prepare() {
    # Back up before replacing anything. Backups survive uninstall.
    local backup="$SUPPORT/Backups/$(date '+%Y%m%d-%H%M%S')-$$"
    mkdir -p "$backup"
    chmod 755 "$SUPPORT" "$SUPPORT/Backups"
    for path in "$DRIVER" "$AGENT" "$BINARY"; do
        if [ -e "$path" ]; then
            mkdir -p "$backup$(dirname "$path")"
            ditto "$path" "$backup$path"
        fi
    done
    if [ -n "$CONSOLE_HOME" ]; then
        local legacy="$CONSOLE_HOME/Library/LaunchAgents/$LABEL.plist"
        if [ -f "$legacy" ]; then
            ditto "$legacy" "$backup/legacy-user-agent.plist"
            rm -f "$legacy"
        fi
    fi
    stop_agent
    echo "バックアップ先: $backup"
}
activate() {
    chown -R root:wheel "$DRIVER"
    chmod -R go-w "$DRIVER"
    chown root:wheel "$AGENT" "$BINARY" "$SUPPORT/manage.sh"
    chmod 644 "$AGENT"
    chmod 755 "$BINARY" "$SUPPORT/manage.sh"
    killall coreaudiod 2>/dev/null || true
    if [ -n "$CONSOLE_HOME" ]; then
        stop_agent
        local registered=0 detail=''
        for attempt in {1..10}; do
            if detail=$(launchctl bootstrap "gui/$CONSOLE_UID" "$AGENT" 2>&1); then
                registered=1
                break
            fi
            sleep 0.5
        done
        if [ "$registered" -ne 1 ]; then
            printf '%s\n' "$detail" >&2
            echo 'MIDIの即時起動に失敗しました。ログアウトして再ログインしてください。 / MIDI could not start; log out and back in.' >&2
        fi
    fi
    echo 'インストール完了。音声は接続・ファーム準備後に表示されます。'
    echo 'MIDIエージェントはログイン時にも起動します。'
}
uninstall() {
    stop_agent
    # Remove only this driver's legacy per-user agent for the console user.
    if [ -n "$CONSOLE_HOME" ]; then
        rm -f "$CONSOLE_HOME/Library/LaunchAgents/$LABEL.plist"
    fi
    rm -rf "$DRIVER"
    rm -f "$AGENT" "$BINARY" "$SUPPORT/manage.sh" "$SUPPORT/README.txt"
    pkgutil --forget com.sawada.audio8dj.installer >/dev/null 2>&1 || true
    killall coreaudiod 2>/dev/null || true
    echo 'Audio 8 DJドライバーとMIDIエージェントを削除しました。'
    echo 'レイテンシ設定とBackupsフォルダは保持しました。他のログイン中ユーザーはログアウトしてください。'
}
case "${1:-}" in
    --dry-run)
        echo "対象: $DRIVER"
        echo "対象: $BINARY"
        echo "対象: $AGENT"
        echo "補助ファイルとバックアップ: $SUPPORT"
        echo '音声サービスを再起動し、ログインユーザーのMIDIエージェントを起動します。'
        exit 0 ;;
    prepare|activate|uninstall) ;;
    *) echo 'Usage: manage.sh prepare|activate|uninstall|--dry-run' >&2; exit 2 ;;
esac
if [ "$(id -u)" -ne 0 ]; then
    echo '管理者権限が必要です。' >&2
    exit 1
fi
console_account
"$1"
