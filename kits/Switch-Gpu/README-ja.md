# SwitchGpu 0.13.0 実機テストキット

Nintendo Switch（Erista）の GM20B を Windows ARM64 で起動し、描画、割り込み、停止を確認するキットです。UEFI の GPU0（`ACPI\SWOA0F01`）が必要です。E3 の割り込み試験は **R447H 以降**で実行できます。

署名済みのキットを展開して `Install.cmd` を実行します。各 CMD は管理者権限を求めます。ファームウェアは自分で用意した linux-firmware の `nvidia/gm20b` 一式を、キット内の `firmware\nvidia\gm20b` または `C:\SwitchWOA\firmware\nvidia\gm20b` に置いてください。Resident と Cycle が ACR・PMU・FECS・GPCCS および GR の初期化データを取り込みます。取り込み済みならレジストリのコピーも使えます。

| 段階 | 実行するファイル | 確認する結果 |
| --- | --- | --- |
| E1 | `Resident.cmd` → `ResidentStatus.cmd` | 起動結果 0、resident 1。状態確認はデバイスの再起動なし。 |
| E2 | `Submit.cmd` | 常駐中にクリア 1,027 回が完了し、GPFIFO が 2 周する。 |
| E3 | `Irq.cmd` | 常駐中に nonstall 割り込みで 64 回完了する。 |
| F1 | `Copy.cmd` | 常駐チャネルの CE2 で 4〜64 KiB を16回コピーする。ページサイズの倍数にそろわない長さも含め、全バイトを照合する。割り込みを使えない場合は上限付きでポーリングする。 |
| F2 | `Blit.cmd` | 常駐中に 1080p 全画面、ずらした 640×360、1行、1列を照合し、全画面20回の GPU/CPU の MB/s と待ち時間を表示する。 |
| F2b (0.11.1) | `Blit.cmd` | 常駐時の GPCPLL・CAR・VDD_GPU 設定値を読み出す。CPU のキャッシュあり／WC と、GPU の個別投入・一括投入・行単位投入・WC を比較する。 |
| F2c (0.12.0) | `Blit.cmd` | バイパスで基準を測り、153.6、307.2、460.8、614.4 MHz のうち CVB 電圧が 1000 mV 以下の点を試す。各段で矩形4種を照合し、全画面20回をキャッシュあり／WC の宛先で計測する。 |
| F3 | `InstallDisplay.cmd` → `F3On.cmd` → 再起動 | 既定は無効。R447H 以降で等倍・回転なしの TV とパネルだけを GPU へ渡す。`F3Status.cmd` で件数と平均時間を確認し、`F3Off.cmd` と再起動で無効に戻す。 |
| F4 (SwitchDisplay 1.8.0) | `InstallDisplay.cmd` → `F4On.cmd` → 再起動 | パネル・TV ごとにバックバッファへ描画し、垂直帰線で切り替える。`F3Status.cmd` に両出力のフリップ、最大待ち時間、GPU／CPU の差分コピー、フォールバック件数を表示する。 |
| G1 (0.13.0) | `F3On.cmd` → 再起動 → `F3Status.cmd` | 最後の GPU Present 成功から既定で約10秒アイドルになると GPU を停止。次の GPU 対象 Present は CPU で描画し、バックグラウンドで GPU を起動する。 |
| E1 停止 | `ResidentStop.cmd` | 停止結果 0。GPU をリセットし、SMMU を復元してメモリをプールに返す。 |
| E4 | `Cycle.cmd` | 停止状態から起動・クリア8回・停止を5周。全領域が予約できた場合、各周で貸出・返却が3ずつ増え、停止後の使用中バイト数は0。 |
| E5 | `Status.cmd` | デバイス状態、結果、直近の Step、プール統計を表示。ResidentStatus は PTIMER が実時間1 msで進んだ量も記録する。 |

F4 の `DoubleBuffer` は既定で 0 です。`F4Off.cmd` と再起動で従来の単一バッファに戻ります。パネルの標準画面は1枚 3,686,400 バイトで、4.5 MiB の予約領域には2枚入らないため、2枚目を 4 GiB 未満の物理連続 WC メモリに確保します。TV は現在のラスタと同じサイズの WC バッファを追加します。1080p では1枚 8,294,400 バイトです。確保に失敗した出力は単一バッファで動作します。

DC-A／DC-B の Window A を選び、assembly バンクに開始アドレスを書き、GENERAL／WIN_A の UPDATE、ACT_REQ を順に発行します。ACT_REQ の解除と active バンクの開始アドレスを最大約50 ms確認します。タイムアウト時は両バッファを最新画像にそろえてフリップを打ち切り、走査が残り得るバッファは解放しません。通常終了では元の走査アドレスを確認して解放します。

UEFI のパネル先頭アドレスが既知の予約範囲外なら、そのバッファの GPU 用 MDL は作りません。ドライバーが確保した2枚目は GPU からアクセスでき、従来のバッファを GPU で安全に扱えない向きや回転・拡大縮小では CPU を使います。F4 と F3 は個別に切り替えられます。

`AutoTest.cmd` はインストールと Windows 再起動を行い、サインイン後に Resident → Irq → Submit → Copy → Blit → ResidentStatus → ResidentStop → Cycle を一度だけ実行します。表示ドライバーの導入と F3 の有効化は手動です。`AutoTestCancel.cmd` は予約と再起動を取り消し、`Remove.cmd` は GPU ドライバーを削除します。

G1 は `AutoResident=1` のときだけ有効です。`switchgpu\Parameters` の DWORD `IdlePowerOffMs` の既定値は 10000 で、0 に設定して再起動すると従来どおり常駐します。`F3On.cmd` は既存値を保持します。GPU 停止後に起動を要求できるのは、GPU に送られる Present です。回転や小さな更新など、表示側が CPU に直接回す処理は対象外です。起動中も CPU 描画を続けます。

自動再起動が2回連続で失敗すると、その Windows 起動中の自動起動を止めます。`ResidentStop.cmd` による手動停止後も自動復帰を止めます。`F3Status.cmd` の G1 行で停止・起動件数、直近の起動時間、失敗数を確認できます。低クロックの中間状態は設けていません。

F2/F3 の GPU VA は `0x20000000` と `0x21000000` から各16 MiBです。GR プールを `0x248000` バイトに拡張し、末尾に GPU の4 KiBページ用テーブル256 KiBと Tegra SMMU のテーブル8枚（計32 KiB）を置きます。マップと解除のたびに SMMU の PTC/TLB と GR PDB の TLB を無効化します。

`F2b bench` は各試験の所要時間と転送量を表示します。a は全画面20回を別々に投入、b は全画面20回を1回で投入、c は1080行を1回で投入して全画面1回分を転送します。WC の GPU 計測は a と同じ投入方法です。計測にはコマンド生成と完了待ちを含み、マップ・照合時間は除きます。一括コマンドは既存 GR arena の `0x0B0000` から64 KiBを使い、待ちは最大2秒を目安に打ち切ります。stage が0以外なら、1が準備・矩形照合、2〜4がGPU a/b/c、5がCPUキャッシュあり、6がGPU WC（割り当てを含む）、7がCPU WCです。

`F2b clock` は基板の 38.4 MHz 水晶と CAR の PLL 参照分周を使った公称値です。バイパス時は周波数を未確定として表示します。VDD_GPU は MAX77621 の VOUT コードから換算した設定値を表示します。TRIM は有効な測定窓が既にあり、カウントが安定していれば、その窓の測定値を表示します。クロックや電圧の設定は変更しません。

`F2c step` は周波数、CVB 電圧、M/N/PL、ロック・ランプのポーリング回数、TRIM のアイドル時の測定値、照合件数と帯域を記録します。CVB は Nouveau の非 NA、speedo ID 0 の表を使い、GPU speedo ヒューズの世代別換算のうち要求電圧が最も高い値を採用します。VOUT 読み返しが 1000 mV でないときや、参照クロックが 38.4 MHz と確認できないときは段をスキップします。ロックやコピーの失敗で打ち切り、PLL を無効化してバイパスへ戻します。DMA 停止やバイパスへの復帰を確認できなければ GPU をリセットします。

既定の常駐起動はバイパスです。必要なときだけ `HKLM\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters\GpcKhz` に DWORD で 76800 kHz 刻みの表の値（最大 614400 kHz）を設定し、次回の常駐起動で適用できます。0 はバイパスです。設定結果は `F2cResidentClockResult` に記録します。Blit の終了後は設定値に関わらずバイパスで動きます。電圧は従来の常駐起動設定のままです。

F3 は試験用 switchdisplay 1.7.1 の `GpuPresent` と switchgpu の `AutoResident` を使います。表示ドライバーは同期の内部 IOCTL `0x22E01C` で MDL・ピッチ・最大64矩形を渡します。回転・拡大縮小は従来どおり CPU が処理します。コピーが完了しなければ GPU を緊急リセットし、DMA 停止を確認できた場合に CPU へ戻します。リセットと DMA 停止を確認できなければ、そのバッファを再利用せず再起動まで保持します。

F1 は既存の GR チャネルで B0B5 をサブチャネル4に割り当てます。GR arena 内の相対位置 `0x1DA000` にコマンドとセマフォ、`0x1E0000` と `0x1F0000` に入出力バッファを置きます。成功時は `F1 copy result 0x00000000` と `16/16` が出ます。完了を確認できなかった場合は `pending 1` を記録し、ResidentStop でリセットしてから再試行できます。

ログは `C:\SwitchWOA-Gpu.txt` に追記します。詳細が必要な場合は `VerboseOn.cmd` を実行し、次のデバイス起動後に試験してください。`VerboseOff.cmd` で次回起動から通常の記録に戻ります。`Gpu.ps1 -Mode Status -Detail` はレジストリに保存済みの詳細を表示します。

0.8.0 では E1〜E4 を実機で確認済みです。0.9.0 は旧試験モードを整理し、通常時のログ量を減らしました。参照コードの出典とライセンスは `NOTICE-GM20B-MMU.txt` に記載しています。
