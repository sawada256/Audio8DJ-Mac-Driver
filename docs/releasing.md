# リリース / Releasing

## 日本語

初期バージョンは `0.1.0`。バージョンは `major.minor.patch` 形式とし、0.x期間も互換性への影響をCHANGELOGで明記します。`VERSION` と `plugin/Info.plist` の `CFBundleShortVersionString` を一致させ、`CFBundleVersion` はリリースごとに増やします。初期版より前の変更履歴はありません。

1. `make release` と必要なサニタイザー検証を実行します。
2. USB抜き差し、再生・録音、DAW切り替え、全レート、MIDI、ログイン時起動、導入・削除を実機で確認し、検証範囲を記録します。
3. CHANGELOGを更新してコミットし、`v0.1.0` のような注釈付きタグを作成します。
4. GitHubへソースとタグをpushし、同じタグのReleaseに `dist/` のZIP、unsigned.pkg、SHA256SUMS.txtを添付します。Releasesのバイナリを用意してから一般利用者に案内してください。

`build/` と `dist/` は生成物としてGit管理しません。ソースのタグと配布物を対応させます。CIはビルド・検査・成果物保存のみで、自動公開しません。署名・公証を追加する場合はビルドパイプラインと利用手順を一緒に更新してください。

ファームウェアは独自コードのMITライセンスとは別扱いです。第三者表示を削除せず配布物に含めます。

## English

The initial version is `0.1.0`. Use `major.minor.patch` and document compatibility impact during 0.x development. Keep VERSION and CFBundleShortVersionString synchronized and increment CFBundleVersion for each release. CHANGELOG starts with this release.

Run `make release` and relevant sanitizers. Validate installation/removal, USB reconnect, recording/playback, DAW switching, rates, MIDI and login startup on hardware and record coverage. Update CHANGELOG, commit and create an annotated tag such as `v0.1.0`. Push source and tag to GitHub and attach the ZIP, unsigned.pkg and SHA256SUMS.txt from dist/ to its Release before directing users to download.

Generated build/ and dist/ are ignored by Git. Keep artifacts associated with their source tag. CI builds, tests and saves artifacts without publishing automatically. If adding Developer ID signing/notarization, update both the pipeline and installation guidance. Preserve third-party firmware notices in distributions.
