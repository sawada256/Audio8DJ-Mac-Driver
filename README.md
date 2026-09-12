# Audio 8 DJ Mac Driver

[English](README.en.md) · [インストール](docs/installation.md) · [困ったとき](docs/troubleshooting.md)

Native Instruments **Audio 8 DJ** をApple Silicon Macで使うための非公式オーディオ・MIDIドライバーです。メーカーの公式製品ではありません。

## ダウンロードと導入

GitHubのこのリポジトリの **Releases** から `Audio8DJ-Mac-Driver-0.1.0-unsigned.pkg` または `Audio8DJ-Mac-Driver-0.1.0.zip` を取得してください。公開者はこれらのファイルをReleaseに添付します。

1. DAWを終了し、音声を使う作業を保存します。
2. `.pkg` を開いてインストールします。ZIPの場合は全体を展開して `install.command` を実行します。
3. Audio 8 DJを接続し、数秒待ちます。「システム設定 → サウンド → 出力」またはDAWで **Audio 8 DJ** を選択します。
4. MIDIを使う場合は、[MIDIとmacOSの許可設定](docs/permissions.md)を確認します。

**配布物からの導入に再ビルド・Xcode・Pythonは不要です。** GitHubの「Code → Download ZIP」はソース用で、ビルド済みインストーラーとは異なります。

Developer ID署名・Apple公証はありません。ダウンロード後のmacOSの確認操作が必要になる場合があります。[詳しい手順](docs/installation.md)をご覧ください。

## 対応機能

- 8入力・8出力、44.1 / 48 / 88.2 / 96 kHz。
- CoreAudio向けFloat32ストリーム、USB側24bit音声。
- USB接続に応じたオーディオ装置の追加・除去。
- MIDI IN / OUT（ログインユーザーの常駐エージェントを使用）。
- 必要な機器ファームウェアを内蔵。接続時に機器のRAMへ転送します。

## 動作条件と制約

macOS 12以降をビルド対象とし、arm64 / x86_64のUniversalバイナリを生成します。主対象はApple Siliconです。全Mac・全macOS・全DAWでの互換性を保証するものではありません。Intel実機、別Macへのダウンロード経由の導入、スリープ復帰などの検証範囲は [互換性](docs/compatibility.md) に記載しています。

メニューバーのサウンド一覧に出ない場合でも、システム設定のサウンド画面で選択して再生できる場合があります。MIDIの実際のUSB送受信には音声ストリームの稼働が必要です。MIDI仮想ポートは未接続でも表示されます。

## ソースからビルド

Xcode Command Line Tools、Python 3、macOSが必要です。

```sh
make build    # build/ にドライバーと実行ファイル
make test     # USB実機を操作しないテスト
make release  # dist/ にZIP・pkg・チェックサムを生成し検査
```

このリポジトリ内のファイルだけでビルドできます。通常のソース取得にはGitサブモジュール・Git LFSは不要です。

## 文書

- [導入・更新・削除 / Installation](docs/installation.md)
- [MIDI常駐・バックグラウンド・録音許可 / Permissions](docs/permissions.md)
- [トラブルシューティング / Troubleshooting](docs/troubleshooting.md)
- [動作条件・既知の制約 / Compatibility](docs/compatibility.md)
- [構成・開発 / Development](docs/development.md)
- [リリース作成 / Releasing](docs/releasing.md)
- [変更履歴 / Changelog](CHANGELOG.md)

## ライセンス

独自コードと文書は [MIT](LICENSE)。同梱ファームウェアはMITの対象外で、権利は各権利者に帰属します。[第三者に関する表示](THIRD_PARTY_NOTICES.md)と[ファームウェア説明](firmware/README.md)をご確認ください。
