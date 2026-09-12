# MIDI・macOSの許可 / MIDI and macOS permissions

## 日本語

### MIDIの常駐エージェント

音声はHALプラグイン、MIDI仮想ポートはログインユーザーの `a8dj-midid` が提供します。インストーラーは `/Library/LaunchAgents/com.sawada.audio8dj.midid.plist` を登録します。rootの常駐デーモンではありません。ログイン時に起動し、終了するとlaunchdが再起動します。ネットワーク通信は行いません。

macOSがバックグラウンド項目の通知を出した場合は、「システム設定 → 一般 → ログイン項目と機能拡張」のバックグラウンド活動／バックグラウンドでの実行許可を確認してください。OSによって項目名が異なります。項目が表示される場合は、詳細の実行ファイル `/usr/local/bin/a8dj-midid` を照合してください。未署名のため表示名が必ずAudio 8 DJになるとは限りません。無効にするとMIDIの自動起動に影響します。[Appleの設定説明](https://support.apple.com/guide/mac-help/mtusr003/mac)

音声だけ使う場合も標準インストーラーはMIDIを含みます。自動起動を止める場合は、通常ユーザーのターミナルで以下を実行します（音声ドライバーは残ります）。

```sh
launchctl disable "gui/$(id -u)/com.sawada.audio8dj.midid"
launchctl bootout "gui/$(id -u)/com.sawada.audio8dj.midid"
```

再開する場合:

```sh
launchctl enable "gui/$(id -u)/com.sawada.audio8dj.midid"
launchctl bootstrap "gui/$(id -u)" /Library/LaunchAgents/com.sawada.audio8dj.midid.plist
```

すでに登録済みの場合はbootstrapがエラーになることがあります。状態確認:

```sh
launchctl print "gui/$(id -u)/com.sawada.audio8dj.midid"
```

MIDIスタジオの `Audio 8 DJ MIDI IN` は機器→Mac、`Audio 8 DJ MIDI OUT` はMac→機器です。ポート表示だけではUSB接続や送受信成功を意味しません。USB MIDI転送にはDAW等で音声ストリームを動かす必要があります。

### 録音とその他の許可

録音するアプリには「プライバシーとセキュリティ → マイク」の許可が必要になる場合があります。許可する対象は使用中のDAWです。[Appleの説明](https://support.apple.com/guide/mac-help/mchla1b1e1fe/mac)

このドライバーはカーネル拡張やDriverKitシステム拡張ではありません。復旧モードでの起動セキュリティ変更、SIP無効化、フルディスクアクセスを通常の導入手順として要求しません。ファイルの入手に伴う実行確認については [インストール手順](installation.md) を参照してください。

## English

The HAL plug-in handles audio; `a8dj-midid` supplies virtual MIDI ports in the logged-in user's session. The installer registers `/Library/LaunchAgents/com.sawada.audio8dj.midid.plist`. It is a per-user LaunchAgent, not a root daemon. launchd starts it at login and restarts it after exit. It does not use the network.

If macOS reports a background item, check System Settings → General → Login Items & Extensions → App Background Activity / Allow in the Background. Labels vary by OS. If an item appears, inspect its executable path: `/usr/local/bin/a8dj-midid`. An unsigned item's displayed name may differ from Audio 8 DJ. Disabling it can prevent automatic MIDI startup. [Apple settings guide](https://support.apple.com/guide/mac-help/mtusr003/mac)

The commands above disable and stop MIDI, enable and register it again, or inspect its state. Run them as your login user, not with sudo. Disabling MIDI leaves the audio driver installed. A duplicate bootstrap can fail if the job is already registered.

MIDI IN carries device-to-Mac data; MIDI OUT carries Mac-to-device data. Virtual ports remain visible without hardware. Actual USB MIDI communication requires an active audio stream, for example from a DAW.

Recording applications may require Microphone access under Privacy & Security. Grant access to the DAW you use. [Apple microphone guide](https://support.apple.com/guide/mac-help/mchla1b1e1fe/mac)

This is a HAL plug-in, not a kernel or DriverKit extension. Normal installation does not require reduced boot security, disabling SIP, or Full Disk Access. Download-related execution prompts are covered in [installation](installation.md).
