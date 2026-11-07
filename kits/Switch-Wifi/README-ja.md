# Wi-Fi 段階 2a：Windows から Wi-Fi チップが見えるかの試験

## 中身

| ファイル | 内容 |
|---|---|
| `switchwifi.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `..\..\drivers\SwitchWifi\src\switchwifi.c` |
| `switchwifi.inf` | `ACPI\SWOA0D01` |
| `Wifi.ps1` | Install、Status、Retry、Remove |
| `Install.cmd` など | `Wifi.ps1` を各モードで動かすだけのもの。右クリックして「管理者として実行」 |

## ドライバーの作り

- **なぜ ACPI デバイスか:** Tegra210 の PCIe は設定空間が ECAM ではないので、Windows の pci.sys では列挙できない。UEFI R405 がリンクを上げて BAR を割り当て、DSDT の `WLN0` としてドライバーに渡す。
- **リソース（この順番で受け取る）:**
  1. BAR0（0x13400000、32 KB）。先頭 4 KB がバックプレーンの窓。
  2. BAR2（0x13000000、4 MB）。チップの RAM。
  3. バス 1 の設定空間の窓（0x11FFF000、4 KB）。
  4. ルートポート 1 のレジスタ（0x01001000）。
  5. AFI（0x01003800）。
- **このバージョンでやること（読むだけ）:**
  - ルートポートのリンク状態を見る。
  - リンクが上がっていれば、エンドポイントの設定空間を読む。
  - ChipCommon のチップ ID を読み、EROM をたどってコアの一覧を作る。
- **安全策:** エンドポイントが応答しないと、SError で Windows が止まる。そこで、最初のアクセスの前に `Busy=1` を書いて保存し、最後に 0 に戻す。次に起動したときに `Busy=1` が残っていたら、エンドポイントには触らない（`Skipped=1`）。そのため、起動を繰り返して止まり続けることはない。`Retry.cmd` で `Busy` を消すと、もう一度試せる。
- **記録:** `Services\switchwifi\Parameters` に書く。`Step` は、止まったときにどこまで進んでいたかを示す。

## 手順

1. **Switch を UEFI R405 で起動する。** 起動画面の「R404 wifi: BCM4356 rev 3 found on PCIe.」が出ることを確認する。
2. **このフォルダーを Switch に移す。**
3. **`Install.cmd` を右クリックして「管理者として実行」する。** 最後に出る結果を写真に撮る。
4. **もし Windows が止まったら:** 次の起動ではドライバーがエンドポイントに触らないので、Windows は起動する。`Status.cmd` を管理者として実行し、`step=` の値を見せてほしい。
