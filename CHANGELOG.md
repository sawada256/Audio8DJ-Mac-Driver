# Changelog / 変更履歴

## [Unreleased]

- MIDI IN のタイムスタンプが常に 0 だった問題を修正。Cubase でリアルタイム録音すると全ノートが録音開始位置に集まり、押したままのノートが録音停止までの長いノートになっていました。受信時刻（ホスト時刻）を付けて送ります。
- MIDI IN timestamps were always 0, so Cubase placed every recorded note at the start of the take (held notes became long notes up to the stop). Packets now carry the host time they were received.
- 出力レイテンシの報告で USB キューの lead を二重に数えていた問題を修正。機器側レイテンシは設定キー `hw_latency_in_us` / `hw_latency_out_us` で報告します。
- IN パケットの欠落・再スケジュール時に公開クロックが実時間から遅れ、ドリフト補正が機器の出力 FIFO を枯渇させていた問題を修正。欠落区間は時間を進めて無音にします。OUT 側も再スケジュール分を読み飛ばして整列を保ちます。
- クロックを USB バスフレーム時刻と DLL（遅延ロックループ）で平滑化し、コールバックの揺らぎが zero timestamp に乗らないようにしました。
- 録音で未到着フレームを読んだ際に直前のサンプルを繰り返していた問題を修正（無音を返す）。
- 再生中のサンプルレート変更を拒否せず、USB を新レートで再起動するようにしました。
- 出力の float→24bit 変換を 2^23 基準の丸めに変更し、24bit 音源をビット単位で再現するようにしました。サンプル変換は1回だけ行います。
- 診断: resync を IN/OUT 別に集計し、直近1秒のクロック最大ずれ・完了遅延・最小余裕を状態ファイルに表示。発生時刻つきのイベントログ `/tmp/a8dj-events.log` を追加。
- 診断の修正: 録音の最小余裕が -LONG_MAX に張り付かず実際の不足フレーム数を示すように。CoreAudio が出力を書いていない間（入力のみ使用など）は出力枯渇に数えず「出力未使用」として別集計し、停止・再開をイベントログに記録。
- 高速プロファイル `fast`（1 ms × 3 転送、入力余裕 0.5 ms）を追加。ループバック実測で往復 6.7 ms（バッファ32）/ 6.0 ms（16）。
- 設定ファイルの変更を、デバイス未使用時にドライバーが読み直し、構成変更として CoreAudio に反映。coreaudiod の再起動が不要に。キューは 3 ms 未満にしない（1 ms × 2 は実測で成立しない）。
- ループバック測定ツール `a8dj-loopback`（往復遅延・音切れ・周波数特性）を追加。
- 余裕を調整する設定キー `in_margin_us` / `out_margin_us` を追加。`set-latency.sh` は preset 以外の設定を保持します。

- Output latency no longer counts the USB queue lead twice; device latency is reported from `hw_latency_in_us` / `hw_latency_out_us`.
- Lost or rescheduled IN packets now advance the clock (as silence) instead of shifting it, which had made drift correction starve the device's output FIFO; OUT skips the same gap.
- Clock is filtered with bus-frame time and a delay-locked loop, keeping callback jitter out of zero timestamps.
- Late capture reads return silence instead of repeating the newest sample.
- Sample-rate changes while running restart USB at the new rate instead of failing.
- Float to 24-bit output uses 2^23 scaling with rounding (bit-exact for 24-bit sources).
- Diagnostics: per-direction resync counts, last-second clock error / completion delay / margins in the status file, and a timestamped event log at `/tmp/a8dj-events.log`.
- Diagnostics fixes: capture margin reports the actual shortfall instead of saturating at -LONG_MAX; output with no CoreAudio writer (input-only use) is counted as idle, not as starvation, with idle/resume events logged.
- New `fast` profile (1 ms × 3, 0.5 ms input slack): 6.7 ms round trip at a 32-frame buffer, 6.0 ms at 16, measured on a loopback.
- The driver re-reads the config while the device is idle and applies it through a configuration change; no coreaudiod restart. The queue never drops below 3 ms (1 ms × 2 measured unschedulable).
- New loopback measurement tool `a8dj-loopback` (latency, dropouts, frequency response).
- New `in_margin_us` / `out_margin_us` keys; `set-latency.sh` keeps non-preset settings.

## [0.1.0] - 2026-09-13

初期リリース。Audio 8 DJの8入力・8出力、USB接続監視、MIDIブリッジ、内蔵ファームウェア、Universalビルド、ZIP/pkgインストーラー、日本語・英語の利用ガイドを提供します。

Initial release: 8-channel input/output, USB presence monitoring, MIDI bridge, embedded firmware, Universal builds, ZIP/pkg installers, and Japanese/English guides.

既知の制約 / Known limitations: [compatibility](docs/compatibility.md).
