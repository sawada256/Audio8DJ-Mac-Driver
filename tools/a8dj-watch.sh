#!/bin/bash
# Watch the driver's counters and print a line only when one of them moves.
#
#   ./a8dj-watch.sh
#
# Run it in a terminal, then reproduce the noise. Every counter that can
# explain a click is here, so if any of them ticks at the moment you hear one,
# that identifies the cause:
#
#   resync / submitFail / rearm   an isochronous transfer missed its bus frame.
#                                 Leaves a gap of a few ms in both directions.
#                                 Cure: a deeper queue (./set-latency.sh mid).
#   録音先読み                     CoreAudio read capture frames that had not
#                                 arrived. Cure: a larger input safety offset.
#   出力枯渇                       the engine sent frames CoreAudio had not
#                                 written. Cure: a larger output safety offset.
#   転送失敗 / µframe異常 / 端数    the bus or the device did not deliver. This
#                                 is the one that would point at power or
#                                 cabling rather than at the driver.
#   クロック補正                   normal. It ticks a couple of times a second
#                                 for as long as the stream runs.
#
# If you hear a click and nothing here moves, the glitch is not in the data
# path at all and the next place to look is analog.
set -u

STATUS=/tmp/a8dj-status.txt
prev=""

echo "監視中です。ノイズが聞こえたらその時刻を控えてください。Ctrl-C で終了。"
echo "(クロック補正だけが増えるのは正常です)"
echo

while :; do
	if [ -r "$STATUS" ]; then
		# Everything except the clock-correction count, which always moves.
		now=$(grep -E '^resync=|^USB:|^出力枯渇|^録音先読み' "$STATUS" \
		      | sed 's/クロック補正=[0-9]* 回//')
		if [ "$now" != "$prev" ]; then
			if [ -n "$prev" ]; then
				echo "--- $(date '+%H:%M:%S') 変化 ---"
				echo "$now"
				echo
			fi
			prev="$now"
		fi
	fi
	sleep 0.2
done
