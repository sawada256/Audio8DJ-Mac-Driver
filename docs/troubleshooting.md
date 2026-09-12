# 困ったとき / Troubleshooting

| 症状 / Symptom | 確認と対処 / Check and action |
|---|---|
| メニューバーに出ない / Missing from menu-bar Sound | システム設定 → サウンド → 出力で選択してください。ここで再生できる場合、装置自体は登録されています。 / Select it in System Settings → Sound → Output. Successful playback there means the audio device is registered. |
| どこにも出ない / Missing everywhere | USBの接続を確認して数秒待ち、Audio MIDI設定を開き直します。別のケーブルやハブなし接続も切り分けに使えます。 / Check USB, wait a few seconds, reopen Audio MIDI Setup; try another cable or a direct connection. |
| USBランプが点滅 / USB LED blinking | 点滅だけではファームウェア失敗と判断できません。音声の開始と実際の出力を確認してください。 / Blinking alone does not establish firmware failure; check audio start and playback. |
| 再生できるが録音できない / Playback works, recording fails | DAWの入力バス、チャンネル、マイク許可を確認。 / Check DAW input buses, channels and microphone permission. |
| MIDIポートが出ない / No MIDI ports | ログイン項目の許可とLaunchAgentの状態を確認。ログアウト・ログインで再登録を試します。 / Check background permission and LaunchAgent status; log out and back in. |
| MIDIポートはあるが届かない / MIDI ports exist but no data | USB接続、IN/OUTの配線、DAWで音声ストリームが動いているかを確認。 / Check USB, DIN MIDI direction and an active audio stream. |
| Bootstrap failed: 5 | インストーラーは短時間再試行します。残る場合は `launchctl print` とバックグラウンド許可を確認し、ログアウト・ログイン。スクリプト全体をrootのユーザーセッションとして起動しないでください。 / Installer retries briefly. If persistent, inspect the job and permissions; log out/in. Do not bootstrap into root's user session. |
| 音切れ / Dropouts | DAWのバッファを増やし、`set-latency.command safe` を使用。変更は音声サービスを再起動します。 / Increase DAW buffer size; use `set-latency.command safe`. Changing the profile restarts system audio. |
| undrained-usb-restart-required | DAWを終了し、下記の音声サービス再起動を実行。未完了のUSBバッファを再利用しない保護です。 / Quit audio apps and restart system audio below; this prevents reuse of USB buffers still owned by pending transfers. |

## 診断 / Diagnostics

```sh
cat /tmp/a8dj-status.txt
launchctl print "gui/$(id -u)/com.sawada.audio8dj.midid"
tail -40 /tmp/a8dj-midid.log
```

配布ZIPの `watch-status.command` は状態表示用です。停止中の値は最後のストリームの記録の場合があり、ファイルがあるだけでは現在の接続を証明しません。 / The ZIP's `watch-status.command` displays counters. Stopped counters can describe the last session; file existence alone does not prove current connectivity.

音声サービス再起動（全アプリの音声を中断） / Restart system audio (interrupts all audio apps):

```sh
sudo killall coreaudiod
```

Issueにはリリース版、Macのチップ、macOS版、DAW名・版、レート、バッファサイズ、接続方法、再現操作、上記ログの必要箇所を添えてください。個人情報は除いてください。 / In an issue, include release version, Mac chip, macOS/DAW versions, rate, buffer size, connection topology, reproduction steps and relevant log excerpts. Remove personal information.
