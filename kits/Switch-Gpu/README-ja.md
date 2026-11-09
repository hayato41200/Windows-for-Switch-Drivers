# GPU 段階 A: switchgpu(試験)

GM20B の GPU に Windows から電源とクロックを入れ、GPU が応答するか(ID レジスター `NV_PMC_BOOT_0` が読めるか)を確かめるテスト用ドライバーです。GPU で描くための最初の一歩で、画面の表示には関係しません。

- **UEFI R411 が要る。** DSDT の `GPU0`(`ACPI\SWOA0F01`)に付く。R411 は R408(画面の DISP)に GPU0 を足し、電源ボタン(PWRB)と MAX77620(0x3C)を共用(Shared)にした版。R410 では電源ボタンのドライバーが 0x3C を独り占めしていて、GPU のドライバーが開けなかった(STATUS_DEVICE_BUSY)。
- 入れただけでは、GPU に電源を入れない。起動のたびに、今の状態(電源チップ、クランプ、クロック、リセット)を**読むだけ**。
- `PowerTest.cmd` を実行したときだけ、1 回、GPU を起こして ID を読み、元どおりに止める。

## 中身

| ファイル | 内容 |
|---|---|
| `Install.cmd` | switchgpu を入れる |
| `Status.cmd` | GPU0 の状態と、switchgpu の記録 |
| `PowerTest.cmd` | GPU を 1 回起こして ID を読み、止める。終わったら Status と同じ表示 |
| `Remove.cmd` | switchgpu を外す |

どれも右クリックして「管理者として実行」。最後の `SUMMARY` を写真に撮る。記録は `C:\SwitchWOA-Gpu.txt` に追記される。

## 手順

1. UEFI **R411** で起動する。
2. `Install.cmd` → `SUMMARY` を写真に撮る(電源チップが読めているか、GPU のレールが最初は切れているか)。
3. `PowerTest.cmd` → `SUMMARY` を写真に撮る。
   - うまくいけば `NV_PMC_BOOT_0 = 0x12B000A1`(chipset 0x12B = GM20B)と `PTIMER ... running` が出る。
4. **Windows が固まったら**: 電源ボタンを長押しして切り、もう一度 R411 で起動して `Status.cmd` を実行する。`last step` で、どこで止まったかがわかる。テストは 1 回きりなので、次の起動で繰り返すことはない。

## GPU を起こす手順(L4T の nvgpu と同じ順)

1. VDD_GPU を 1.0 V にする(MAX77621 I2C5 0x1C の VOUT と VOUT_DVS = 0xBF)。イネーブルのピン(MAX77620 の GPIO6)を High にする。2 ms 待つ。
2. クロックを入れる: GPU(CLK_X 24)、pllg_ref(CLK_X 29)、pll_p_out5(204 MHz)、fuse。
3. GPU のリセットを立てる → クランプを外す(PMC_GPU_RG_CNTRL = 0)→ リセットを下ろす。
4. MC のフラッシュ(GPU のホットリセット)を入れて、外す。
5. ここで初めて GPU のレジスター(BAR0)を読む。クランプ、クロック、リセットが正しい状態のときだけ読む。
6. 逆の順に止め、レジスターと電源チップを元の値に戻す。

## 戻し方

- R408 で起動すれば GPU0 は無くなり、switchgpu は読み込まれない。
- `Remove.cmd` で外せる。

## 記録

`HKLM\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters`。設定(DWORD):
- `PowerTest` = 1: 次にドライバーが起動したとき、1 回だけ GPU を起こす(始める前に 0 に戻る)。`PowerTest.cmd` が使う。
- `KeepOn` = 1: テストのあと GPU を止めない(段階 B の準備用。ふだんは使わない)。
