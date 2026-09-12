# 機器ファームウェア / Device firmware

`audio8dj_firmware.h` はAudio 8 DJへ送るファームウェアのチャンク配列です。ビルド時にドライバーへ埋め込まれ、必要に応じて機器RAMへロードされます。利用者による別ファイルの配置は不要です。機器のEEPROMを書き換えるインストーラーではありません。

`audio8dj_firmware.h` contains device firmware chunks embedded in the driver and loaded into Audio 8 DJ RAM when needed. Users do not need to install a separate firmware file. The installer does not rewrite device EEPROM.

SHA-256 (header): `f36aec623a120d72643dffe631f19519db8e036d7aae8c1b20c88b50a663bc46`

メーカー由来データはMIT対象外です。 / Manufacturer-derived firmware is excluded from MIT. See [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).
