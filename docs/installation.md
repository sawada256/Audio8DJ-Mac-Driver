# インストール・更新・削除 / Installation, updates and removal

[日本語README](../README.md) · [English README](../README.en.md)

## 日本語

### 準備

macOS 12以降のMacと管理者パスワードが必要です。DAWと音声アプリを終了してください。導入時にはMac全体の音声サービスが再起動します。普段の設定を控え、スピーカーの音量を下げて初回確認を行ってください。

### pkgで導入

Releasesの `*-unsigned.pkg` をダブルクリックし、インストーラーに従います。これは音声ドライバーとMIDIエージェントをまとめて導入します。ネット接続・再ビルドは導入処理に不要です。

「開発元を確認できない」等で開けない場合は、一度開く操作をした後「システム設定 → プライバシーとセキュリティ」を確認してください。提供元を確認できたファイルについて、macOSが「このまま開く」を提示した場合にその操作を使えます。OSの版や管理ポリシーによって操作できない場合があります。セキュリティ機能の一括無効化は導入手順に含めていません。[Appleの説明](https://support.apple.com/guide/mac-help/mh40616/mac)

### ZIPで導入

ReleasesのZIPを**全体**展開します。`install.command`、`payload/`、`SHA256SUMS`を同じフォルダのまま保持してください。`install.command`をダブルクリックします。開けない場合はターミナルで `bash `（末尾に半角スペース）を入力し、そのファイルをウインドウへドラッグしてReturnを押します。

```sh
bash "/展開したフォルダ/install.command"
```

必要な箇所で管理者パスワードを求めます。入力中に文字が表示されないのは通常の動作です。ファイルの欠落・改変があると導入前に停止します。導入せず検査する場合は末尾に `--dry-run` を付けます。チェックサムはファイルの整合性検査で、発行者認証の代わりではありません。

### 接続と選択

Audio 8 DJを接続し数秒待ち、「システム設定 → サウンド → 出力」から選びます。「Audio MIDI設定」では8入力・8出力とレートを確認できます。DAWでもAudio 8 DJを選択し、入出力バスを設定してください。MIDIや録音の許可は [permissions.md](permissions.md) を参照してください。

### 更新

新しいReleaseを同じ方法で上書きします。古いドライバー等は `/Library/Application Support/Audio8DJ/Backups/` に保存されます。以前の同一識別子のユーザー用MIDI登録がある場合、現在のコンソールユーザー分をバックアップして共通LaunchAgentに統一します。複数ユーザーが同時ログイン中の場合、ほかのユーザーはログアウトしてください。

### 削除

配布ZIPの `uninstall.command` を実行するか、ターミナルで以下を実行します。

```sh
sudo bash '/Library/Application Support/Audio8DJ/manage.sh' uninstall
```

音声ドライバー、共通MIDI登録、MIDI実行ファイル、管理スクリプトを削除し、音声サービスを再起動します。バックアップとレイテンシ設定は保持します。通常は再起動不要ですが、MIDI登録が反映されなければログアウト・ログインしてください。

## English

### Prepare and install

Use macOS 12 or later and an administrator account/password. Save your work and quit audio applications: installation restarts system audio. Lower speaker volume for the first check.

Download the `*-unsigned.pkg` from Releases and follow Installer, or extract the **entire** release ZIP and open `install.command`. Keep `payload/` and `SHA256SUMS` beside it. You can also type `bash ` in Terminal, drag the command file into the window, and press Return. The script requests administrator authentication when needed; password entry is not echoed.

The prebuilt installers include audio, MIDI and embedded firmware. They do not need internet access, Python, Xcode or compilation. A source ZIP downloaded through GitHub's Code menu needs building first.

If macOS blocks an unidentified developer, review System Settings → Privacy & Security after attempting to open the file. For a file whose source you trust, use Open Anyway if macOS offers it. Availability depends on the OS and management policy. This project does not require globally disabling security protections. [Apple instructions](https://support.apple.com/guide/mac-help/mh40616/mac)

The ZIP installer checks included hashes and signatures before installing. `bash /path/to/install.command --dry-run` validates without installing. Hashes detect corruption; they do not authenticate the publisher.

### Select, update and remove

Connect the interface, wait a few seconds, and select Audio 8 DJ in System Settings → Sound → Output. Audio MIDI Setup shows channels and sample rate. Configure your DAW's input/output buses separately. See [permissions](permissions.md) for background MIDI and recording access.

Install a newer release the same way. Existing components are backed up under `/Library/Application Support/Audio8DJ/Backups/`. A matching per-user MIDI registration for the current console user is backed up and replaced by the shared LaunchAgent registration. Other concurrently logged-in users should log out.

Run `uninstall.command` from the ZIP, or the `sudo bash ... uninstall` command above. Removal deletes the installed audio/MIDI components and restarts system audio, preserving backups and latency settings. A restart is normally unnecessary; log out and back in if MIDI registration does not refresh.
