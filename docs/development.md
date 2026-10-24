# 開発 / Development

## 構成 / Layout

| Path | 内容 / Contents |
|---|---|
| `plugin/` | CoreAudio HAL、USB音声、MIDI FIFO / HAL, USB audio, MIDI FIFO |
| `firmware/` | 埋め込み機器データ / Embedded device firmware |
| `tools/` | MIDI常駐エージェントと診断 / MIDI agent and diagnostics |
| `packaging/` | ZIP・pkgで共通の導入と削除 / Shared install/remove logic |
| `scripts/` | ビルド、配布物生成、レイテンシ / Build, package, latency |
| `tests/` | USB実機不要のテスト / Tests without USB hardware |
| `docs/` | 日本語・英語ガイド / Bilingual guides |
| `build/`, `dist/` | 生成物、Git対象外 / Generated, ignored by Git |

## ビルド / Build

macOSにXcode Command Line Toolsを導入（`xcode-select --install`）、Python 3を用意します。リポジトリ直下で `make release` を実行するとテスト、Universalビルド、ad-hoc署名、ZIP/pkg生成、パッケージ検査を行います。開発時は `make build` / `make test` を別々に実行できます。自動処理はドライバーをインストールしません。

On macOS install Xcode Command Line Tools (`xcode-select --install`) and Python 3. Run `make release` at the repository root for tests, Universal builds, ad-hoc signing, installers and package verification. `make build` and `make test` run separately. These commands do not install the driver.

```sh
sh tests/sanitize.sh address
sh tests/sanitize.sh thread
clang --analyze -Xanalyzer -analyzer-output=text -Iplugin   plugin/Audio8DJ.c plugin/usb_engine.c plugin/midi.c tools/a8dj-midid.c
```

## 実装 / Architecture

HALはUSB機器の接続を監視してCoreAudioへ公開します。必要に応じファームウェアを機器RAMへ転送します。USB転送のバッファはプラグインの生存期間中保持し、非同期完了前に再利用しません。音声リングは絶対フレーム番号・セッション世代・非待機アクセスで管理し、無効なデータは無音にします。レート変更はCoreAudioホストの構成変更手続きを使います。

The HAL monitors USB presence and publishes the audio device, loading firmware into device RAM when needed. USB buffers persist for the plug-in lifetime and must not be reused before asynchronous completion. Audio slots carry absolute frame and session tags with nonblocking access; invalid data reads as silence. Rate changes use the CoreAudio host's configuration-change procedure.

MIDIエージェントはユーザーセッションでCoreMIDIポートを作り、`/tmp/a8dj-midi-tohost` と `/tmp/a8dj-midi-todev` のFIFOでHALと通信します。リアルタイムUSB完了処理はファイルIOを行わずリングへ格納します。

The per-user MIDI agent creates CoreMIDI ports and exchanges bytes with the HAL through `/tmp/a8dj-midi-tohost` and `/tmp/a8dj-midi-todev`. Real-time USB completions enqueue data rather than perform filesystem IO.

## レイテンシ / Latency

`bash scripts/set-latency.sh safe`（標準）, `mid`, `low`, `fast`。設定は `/Users/Shared/audio8dj.conf` に保存され、Audio 8 DJ を使うアプリがすべて止まった時点でドライバーが読み直します（coreaudiod の再起動は不要。`--restart` で強制再起動）。`safe` から始め、USB負荷に応じて調整してください。DAWのバッファ設定は別項目です。

Use `bash scripts/set-latency.sh safe` (default), `mid`, `low` or `fast`. The profile is stored in `/Users/Shared/audio8dj.conf` and the driver re-reads it once no app is using the device (no coreaudiod restart; `--restart` forces one). Start with safe and adjust for USB load. The DAW's buffer size is a separate setting.

| プロファイル / Profile | 転送 / Transfers | セーフティ in/out (48 kHz) | 往復遅延 実測 / Measured round trip |
|---|---|---|---|
| safe | 2 ms × 4 | 192 / 480 | 約 16 ms (バッファ64, 計算値) |
| low | 1 ms × 4 | 96 / 240 | 8.2 ms (バッファ32) / 7.5 ms (16) |
| fast | 1 ms × 3, 入力余裕 0.5 ms | 72 / 192 | 6.7 ms (バッファ32) / 6.0 ms (16) |

実測は出力 7/8（ヘッドホン端子）→入力 7/8 のループバック。fast は5分間、44.1 / 48 / 96 kHz で音切れなし（96 kHz・バッファ32で 5.8 ms）。1 ms × 2 はスケジュールが毎回間に合わず成立しないため、ドライバーはキューを 3 ms 以上に保ちます。入力余裕 0.25 ms では最小余裕が1フレームまで減り、0 では録音の先読み不足が起きました。

Measured through an out 7/8 (headphone jack) to in 7/8 loopback. fast ran 5 minutes, and at 44.1 / 48 / 96 kHz, without a dropout (5.8 ms at 96 kHz, buffer 32). 1 ms × 2 never schedules in time, so the driver keeps at least 3 ms queued. 0.25 ms of input slack left one frame to spare; 0 starved.

### ループバック測定 / Loopback measurement

`build/bin/a8dj-loopback --out 7 --in 7 --buffer 32 --seconds 30 [--rate 96000]` は出力ペアから試験信号を出し、入力ペアで受けて、往復遅延（ハードウェア経路とアプリから見た値）、1 ms 単位の音切れ、周波数特性（1 kHz 基準・コヒーレンス付き）を表示します。ケーブルで出力と入力をつないでから実行してください。

`build/bin/a8dj-loopback` plays a test signal on an output pair, records an input pair, and reports round-trip latency (hardware path and as seen by an app), 1 ms dropout detection and frequency response relative to 1 kHz with coherence. Connect the output to the input with a cable first.

### 詳細設定 / Fine tuning

`/Users/Shared/audio8dj.conf` には次のキーも書けます（値はマイクロ秒、`set-latency.sh` で preset を切り替えても保持されます）。反映には coreaudiod の再起動が必要です。

| キー / Key | 既定 / Default | 内容 / Meaning |
|---|---|---|
| `in_margin_us` | 1転送分 / one transfer | 入力セーフティオフセットの余裕（1転送分に加算）/ Input slack on top of one transfer |
| `out_margin_us` | 0 | 出力セーフティオフセットの余裕（lead + 1転送分に加算）/ Output slack on top of lead + one transfer |
| `hw_latency_in_us` | 0 | アプリへ報告する入力側の機器レイテンシ（ADC等）/ Input converter/device latency reported to apps |
| `hw_latency_out_us` | 0 | 同、出力側（DAC・機器内FIFO）/ Same, output side |

`hw_latency_*` はループバック実測で決めます。キューの lead はセーフティオフセットに含まれるため、ここには入れません。状態ファイル `/tmp/a8dj-status.txt` の「録音先読み」「出力枯渇」「クロック: 最大ずれ」が余裕を詰める際の指標です。

Set `hw_latency_*` from a loopback measurement; the queue lead is already part of the safety offset and must not be added here. While reducing margins, watch the capture-starve, output-starve and clock-error lines in `/tmp/a8dj-status.txt`.

### 診断ログ / Diagnostics log

ストリーミング中は `/tmp/a8dj-events.log` に追記します（1 MB を超えると最初から）。resync（IN/OUT 別、予定フレームをどれだけ過ぎていたか、欠落時間）、rearm、クロックの 500 µs 超のずれ、フィルタ再始動を発生時刻つきで記録し、何か起きた秒には1秒集計（録音先読み・出力枯渇・最小余裕・欠落補完・クロック最大ずれ・完了遅延最大）を書きます。`t=` は再生開始からの秒数です。状態ファイルには直近1秒の値と resync の内訳が出ます。CoreAudio が出力を書いていない間（入力だけ使うアプリなど）は「出力未使用」として数え、出力枯渇には含めません。1秒未満で再開した「出力再開」は、書き込み側の遅れによる実際の音切れの可能性があります。

While streaming, `/tmp/a8dj-events.log` records resyncs (per direction, how far past the planned frame, gap length), rearms, clock deviations over 500 µs and filter restarts, plus a one-second summary for any second in which something happened. `t=` is seconds since streaming started. The status file shows the last second's values and the resync split. `bash scripts/set-latency.sh show` prints both.

