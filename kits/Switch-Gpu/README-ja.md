# SwitchGpu 0.10.0 実機テストキット

Nintendo Switch（Erista）の GM20B を Windows ARM64 で起動し、描画、割り込み、停止を確認するキットです。UEFI の GPU0（`ACPI\SWOA0F01`）が必要です。E3 の割り込み試験は **R447H 以降**で実行できます。

署名済みのキットを展開して `Install.cmd` を実行します。各 CMD は管理者権限を求めます。ファームウェアは自分で用意した linux-firmware の `nvidia/gm20b` 一式を、キット内の `firmware\nvidia\gm20b` または `C:\SwitchWOA\firmware\nvidia\gm20b` に置いてください。Resident と Cycle が ACR・PMU・FECS・GPCCS および GR の初期化データを取り込みます。取り込み済みならレジストリのコピーも使えます。

| 段階 | 実行するファイル | 確認する結果 |
| --- | --- | --- |
| E1 | `Resident.cmd` → `ResidentStatus.cmd` | 起動結果 0、resident 1。状態確認はデバイスの再起動なし。 |
| E2 | `Submit.cmd` | 常駐中にクリア 1,027 回が完了し、GPFIFO が 2 周する。 |
| E3 | `Irq.cmd` | 常駐中に nonstall 割り込みで 64 回完了する。 |
| F1 | `Copy.cmd` | 常駐チャネルの CE2 で 4〜64 KiB を16回コピーする。ページサイズの倍数にそろわない長さも含め、全バイトを照合する。割り込みを使えない場合は上限付きでポーリングする。 |
| E1 停止 | `ResidentStop.cmd` | 停止結果 0。GPU をリセットし、SMMU を復元してメモリをプールに返す。 |
| E4 | `Cycle.cmd` | 停止状態から起動・クリア8回・停止を5周。全領域が予約できた場合、各周で貸出・返却が3ずつ増え、停止後の使用中バイト数は0。 |
| E5 | `Status.cmd` | デバイス状態、結果、直近の Step、プール統計を表示。ResidentStatus は PTIMER が実時間1 msで進んだ量も記録する。 |

`AutoTest.cmd` はインストールと Windows 再起動を行い、サインイン後に Resident → Irq → Submit → Copy → ResidentStatus → ResidentStop → Cycle を一度だけ実行します。`AutoTestCancel.cmd` は予約と再起動を取り消し、`Remove.cmd` はドライバーを削除します。

F1 は既存の GR チャネルで B0B5 をサブチャネル4に割り当てます。GR arena 内の相対位置 `0x1DA000` にコマンドとセマフォ、`0x1E0000` と `0x1F0000` に入出力バッファを置きます。成功時は `F1 copy result 0x00000000` と `16/16` が出ます。完了を確認できなかった場合は `pending 1` を記録し、ResidentStop でリセットしてから再試行できます。

ログは `C:\SwitchWOA-Gpu.txt` に追記します。詳細が必要な場合は `VerboseOn.cmd` を実行し、次のデバイス起動後に試験してください。`VerboseOff.cmd` で次回起動から通常の記録に戻ります。`Gpu.ps1 -Mode Status -Detail` はレジストリに保存済みの詳細を表示します。

0.8.0 では E1〜E4 を実機で確認済みです。0.9.0 は旧試験モードを整理し、通常時のログ量を減らしました。参照コードの出典とライセンスは `NOTICE-GM20B-MMU.txt` に記載しています。
