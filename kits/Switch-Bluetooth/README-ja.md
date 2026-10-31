# Bluetooth：Nintendo Switch の Bluetooth ドライバー（ベータ、試験中）

チップを、Windows に最初から入っている Bluetooth の部品（BthMini）につなぎます。うまくいけば、設定に「Bluetooth」が出て、マウスやキーボードとペアリングできます。

## 中身

| ファイル | 内容 |
|---|---|
| `switchbt.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `drivers\SwitchBluetooth\src\switchbt.c` |
| `switchbt.inf` | `ACPI\SWOA0B01` |
| `Bluetooth.ps1` | Install、Status、Remove |

## ドライバーの作り

- **使うリソース（DSDT の `BTH0`、UEFI R390 以降）:**
  - UART-D（0x70006300）。Wi-Fi と一体の BCM4356A3 の Bluetooth が、ここにつながっている（H4）。
  - 割り込み GSI 122。
- **前提:** UEFI R390 が、UART-D のピンとクロックを設定し、チップの電源（PH4）を入れる。
- **起動したとき:** 115200 bps で次を送り、答えを `Services\switchbt\Parameters` に書く。
  - HCI_Reset、Read_Local_Version
  - Write_BD_ADDR：チップのアドレスは仮の `AA:AA:AA:AA:AA:AA` なので、Switch ごとに1つ作って（98:B6:E9 + ランダムな3バイト）、レジストリの `AddressSuffix` に覚えておく。毎回同じアドレスを書く。
  - Read_BD_ADDR、Read_Local_Name
- **そのあと:** 子デバイス `SWOA\BTH_UART_H4`（互換 ID `MS_BTHX_BTHMINI`）を作る。Windows の `bth.inf` が、そこに BthMini と Bluetooth のスタックを入れる。
  - 送受信は割り込みで行う。速度は 115200 bps のまま（マウスやキーボードには十分。音声は第3段）。
  - ファームウェアのパッチは、まだ当てない（チップの ROM のまま）。

## 起動が止まったとき（1.0.0.5 以降）

- ドライバーは、起動のたびに `BootPending` をディスクに書き、60 秒動いたら消す。
- 前の起動で消えていなければ（Windows が途中で止まった）、その起動ではドライバーは何もしない（`SafeMode=1`）。そのまま Windows は起動できる。
- `Stage`（1 追加、2 キュー、3 ハードウェア、4 チップの確認、5 動作中、6 子デバイス追加。1.0.0.6 から子デバイスは起動の20秒後）と、やり取りの数も、その場でディスクに書く。止まった起動で、どこまで進んだかが分かる。
- もう一度動かすときは `-Mode Retry`（または `-Mode Install`）を実行して、再起動する。

## 手順

1. **Switch を UEFI R390 で起動する。**
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\Bluetooth`（第1段のフォルダーは上書きしてよい）。
3. **インストールする。** 管理者の PowerShell で実行する。第1段が入っていても、そのまま上書きでよい。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Bluetooth\Bluetooth.ps1 -Mode Install
   ```
   - `RESULT: the Bluetooth chip answers.` と、`BdAddr=98:B6:E9:...` が出る。
   - `radio: SWOA\BTH_UART_H4 ... problem=0` と `RESULT 2` が出れば、Windows の Bluetooth が動いている。
4. **設定 → Bluetooth とデバイス** を開いて、Bluetooth のスイッチが出るか、機器を探せるか試す。

うまくいかないときは、`-Mode Status` の結果（`C:\SwitchWOA-Bluetooth.txt`）を見せてください。
