#!/bin/bash
# Switches the Audio 8 DJ driver between its latency profiles.
#
#   ./set-latency.sh safe | mid | low | show
#
# The profile is read when coreaudiod loads the plug-in, so switching restarts
# it. Any audio playing at the time will be interrupted.
set -e

CONF=/Users/Shared/audio8dj.conf

usage() {
    cat <<'EOF'
使い方 / Usage: bash set-latency.sh [safe|mid|low|show]

safe : 標準・安定性優先 / default, larger USB queue
mid  : 中間 / intermediate queue
low  : 低レイテンシ / smaller queue
show : 設定と状態を表示 / show configuration and status

設定変更はMac全体の音声を一時停止します。
Changing the profile restarts system audio and interrupts all audio apps.
DAWのバッファ設定は別に調整します。音切れがあればsafeを使用してください。
DAW buffer size is separate. Use safe if you encounter dropouts.

EOF
}

# Replace only the preset line; margins and measured hardware latencies
# (in_margin_us, hw_latency_out_us, ...) stay as they were.
write_preset() {
    local tmp
    tmp=$(mktemp)
    if [ -f "$CONF" ]; then
        grep -v -E '^[[:space:]]*(preset|uframes|nbuf)[[:space:]]*=' "$CONF" > "$tmp" || true
    fi
    echo "preset=$1" >> "$tmp"
    cat "$tmp" > "$CONF"
    rm -f "$tmp"
}

case "${1:-}" in
safe|mid|low) write_preset "$1" ;;
show)
    if [ -f "$CONF" ]; then
        echo "設定ファイル $CONF:"
        cat "$CONF"
    else
        echo "設定ファイルなし → 既定の safe で動作します"
    fi
    echo
    echo "実行中の状態 (/tmp/a8dj-status.txt):"
    [ -f /tmp/a8dj-status.txt ] && cat /tmp/a8dj-status.txt || echo "  (未起動)"
    echo
    echo "イベントログ末尾 (/tmp/a8dj-events.log):"
    [ -f /tmp/a8dj-events.log ] && tail -n 20 /tmp/a8dj-events.log || echo "  (なし)"
    exit 0
    ;;
*) usage; exit 1 ;;
esac

chmod 666 "$CONF" 2>/dev/null || true
echo "$CONF に $1 を書きました。"
echo "coreaudiod を再起動します (sudo が必要です)..."
sudo killall coreaudiod
echo "完了。デバイスを選び直してから再生してください。"
