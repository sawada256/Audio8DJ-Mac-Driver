# 互換性 / Compatibility

## 日本語

対象機器はAudio 8 DJ（USB VID `17cc` / PID `1978`）です。ほかのNative Instruments機器の対応を意味しません。macOS 12以降をビルド対象とし、Universalバイナリにarm64とx86_64を含めます。Intel実機の動作確認は行っていません。

初期版の確認範囲: Apple Silicon / macOS 26.3での実機再生、CoreAudioの8入力・8出力列挙、44.1 kHzでの短時間の無音ストリーム開始・停止。自動テストは音声変換、ライフサイクル、MIDI解析・転送キュー、競合・境界条件、パッケージ整合性を対象にしています。これらは全4レートでの長時間録音、全DAW、スリープ復帰、別Macでのダウンロード後インストール、実機MIDIの全メッセージの保証ではありません。

既知の制約:

- メニューバーのサウンド一覧に出ず、システム設定からは選択できる場合があります。原因は未確定です。
- MIDI単独稼働は対応していません。音声ストリーム稼働中にUSB MIDIが動作します。
- MIDI仮想ポートの表示はUSB接続と連動しません。
- 複数ユーザー同時利用は対象外です。MIDIキューは有限で、過負荷時の完全な無欠落は保証しません。
- Developer ID署名・Apple公証はありません。

## English

This driver targets Audio 8 DJ (USB VID `17cc`, PID `1978`), not other Native Instruments models. The deployment target is macOS 12; Universal builds contain arm64 and x86_64. Intel hardware has not been validated.

Initial validation includes hardware playback on Apple Silicon / macOS 26.3, enumeration of 8 inputs/8 outputs, and short silent stream start/stop at 44.1 kHz. Automated tests cover encoding, lifecycle, MIDI parsing/queues, concurrency, bounds and package consistency. This does not validate long recording sessions at all four rates, every DAW, sleep/wake, downloaded installation on another Mac, or every hardware MIDI message.

Known limitations: the menu-bar Sound list can omit the device while System Settings can select it (cause unresolved); USB MIDI needs an active audio stream; virtual MIDI ports do not track hardware presence; simultaneous multi-user operation is unsupported; finite MIDI queues can overflow; releases are not Developer ID signed or notarized.
