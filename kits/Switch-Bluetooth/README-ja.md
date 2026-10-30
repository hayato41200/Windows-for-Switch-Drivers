# Bluetooth：Nintendo Switch の Bluetooth ドライバー（第1段：通信の確認）

第1段は、Bluetooth チップと話せるかを確かめるだけです。まだ Windows の「Bluetooth」には出ません。

## 中身

| ファイル | 内容 |
|---|---|
| `switchbt.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `drivers\SwitchBluetooth\src\switchbt.c` |
| `switchbt.inf` | `ACPI\SWOA0B01` |
| `Bluetooth.ps1` | Install、Status、Remove |

## ドライバーの作り

- **使うリソース（DSDT の `BTH0`、UEFI R390 以降）:**
  - UART-D（0x70006300）。Wi-Fi と一体の BCM4356A3 の Bluetooth が、ここにつながっている（H4）。
  - 割り込み GSI 122（第1段では使わない）。
- **前提:** UEFI R390 が、UART-D のピンとクロックを設定し、チップの電源（PH4）を入れる。
- **動き:** 起動したときに 115200 bps で次を送り、答えを `Services\switchbt\Parameters` に書く。
  - HCI_Reset
  - Read_Local_Version
  - Read_BD_ADDR
  - Read_Local_Name

## 手順

1. **Switch を UEFI R390 で起動する。** ペイロードは `D:\Codex\coreboot-R390.rom`。
   - UEFI の画面に、次の行が出るか見る。
     - `R390 Bluetooth: HCI ok, ... maker 000f ...`（000f = Broadcom）
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Bluetooth`。
3. **インストールする。** 管理者の PowerShell で実行する。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Bluetooth\Bluetooth.ps1 -Mode Install
   ```
   - `RESULT: the Bluetooth chip answers.` と、チップの名前（`BCM4356A3` など）が出れば成功。
   - 最後の行の `MS_BTHX_BTHMINI=True` は、次の段階で使う Windows の部品があるかどうか。

結果は `C:\SwitchWOA-Bluetooth.txt` にも残ります。
