#!/bin/bash
# Switches the Audio 8 DJ driver between its latency profiles.
#
#   ./set-latency.sh safe | mid | low | fast | show [--restart]
#
# The driver re-reads the profile whenever no app is using the device, so a
# change applies as soon as playback and recording stop -- no coreaudiod
# restart. --restart forces one (needed only for drivers older than that).
set -e

CONF=/Users/Shared/audio8dj.conf

usage() {
    cat <<'EOF'
使い方 / Usage: bash set-latency.sh [safe|mid|low|fast|show] [--restart]

safe : 標準・安定性優先 (2 ms × 4 転送) / default, larger USB queue
mid  : 中間 (2 ms × 3) / intermediate queue
low  : 低レイテンシ (1 ms × 4) / smaller queue
fast : 最小レイテンシ (1 ms × 3, 入力余裕 0.5 ms) / smallest measured-stable
show : 設定と状態を表示 / show configuration and status

設定はデバイスを使うアプリがすべて止まった時点で反映されます。
The profile applies once every app has stopped using the device.
--restart を付けると coreaudiod を再起動して即座に反映します（全アプリの音声が止まります）。
DAWのバッファ設定は別に調整します。音切れがあれば low か safe に戻してください。
DAW buffer size is separate. Go back to low or safe if you hear dropouts.

fast の実測 (48 kHz, 出力7/8→入力7/8 ループバック): バッファ32で往復 6.7 ms、
バッファ16で 6.0 ms、5分間 音切れなし。負荷の高い Mac では low の方が安全です。
EOF
}

# Replace the profile lines only; measured hardware latencies
# (hw_latency_in_us / hw_latency_out_us) and comments stay as they were.
write_profile() {
    local tmp
    tmp=$(mktemp)
    if [ -f "$CONF" ]; then
        grep -v -E '^[[:space:]]*(preset|uframes|nbuf|in_margin_us|out_margin_us)[[:space:]]*=|^# profile:' \
            "$CONF" > "$tmp" || true
    fi
    echo "# profile: $1" >> "$tmp"
    case "$1" in
    # Spelled out rather than preset=fast so drivers that predate the preset
    # name still apply it.
    fast) printf 'uframes=8\nnbuf=3\nin_margin_us=500\n' >> "$tmp" ;;
    *)    echo "preset=$1" >> "$tmp" ;;
    esac
    cat "$tmp" > "$CONF"
    rm -f "$tmp"
}

case "${1:-}" in
safe|mid|low|fast) write_profile "$1" ;;
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
if [ "${2:-}" = --restart ]; then
    echo "coreaudiod を再起動します (sudo が必要です)..."
    sudo killall coreaudiod
    echo "完了。デバイスを選び直してから再生してください。"
else
    echo "Audio 8 DJ を使っているアプリをすべて止めると反映されます（coreaudiod の再起動は不要）。"
fi
