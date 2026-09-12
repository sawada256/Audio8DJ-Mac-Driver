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

`bash scripts/set-latency.sh safe`（標準）, `mid`, `low`。設定は `/Users/Shared/audio8dj.conf` に保存され、反映時に音声が中断します。`safe` から始め、USB負荷に応じて調整してください。DAWのバッファ設定は別項目です。

Use `bash scripts/set-latency.sh safe` (default), `mid`, or `low`. The profile is stored in `/Users/Shared/audio8dj.conf`; applying it interrupts audio. Start with safe and adjust for USB load. The DAW's buffer size is a separate setting.
