# GPU 段階 A / B1 / B2a: switchgpu(試験)

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
| `ProbeTest.cmd` | 段階 B1。PowerTest と同じく GPU を 1 回起こし、そのあいだに、ファームウェアを使わない調べもの(GPU の中の部品の一覧、PMU ファルコンのレジスター、DMEM の書き込みと読み返し、priv ring の初期化)をしてから止める。GPU の上では何も動かさない |
| `DmaTest.cmd` | 段階 B2a。GPU を 1 回起こし、GPU の中の小さな CPU(PMU ファルコン)の DMA で、Windows のメモリーから 256 バイト読み、256 バイト書き戻せるかを確かめてから止める。ファームウェアは使わず、ファルコンの上では何も動かさない |
| `Remove.cmd` | switchgpu を外す |

どれも右クリックして「管理者として実行」。最後の `SUMMARY` を写真に撮る。記録は `C:\SwitchWOA-Gpu.txt` に追記される。

## 手順

1. UEFI **R411** で起動する。
2. `Install.cmd` → `SUMMARY` を写真に撮る(電源チップが読めているか、GPU のレールが最初は切れているか)。
3. `PowerTest.cmd` → `SUMMARY` を写真に撮る。
   - うまくいけば `NV_PMC_BOOT_0 = 0x12B000A1`(chipset 0x12B = GM20B)と `PTIMER ... running` が出る。
4. **Windows が固まったら**: 電源ボタンを長押しして切り、もう一度 R411 で起動して `Status.cmd` を実行する。`last step` で、どこで止まったかがわかる。テストは 1 回きりなので、次の起動で繰り返すことはない。

## 段階 B1(ProbeTest、0.2.0)

段階 A が通った本体で、`Install.cmd`(0.2.0 に入れ替え)→ `ProbeTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `B1 topology`: GPU の中の部品の一覧(PMU、FECS などの場所)。
- `B1 PMU falcon`: PMU のファルコン(ファームウェアの検証を動かす小さな CPU)の大きさと状態。`production` か `debug-fused` か。
- `B1 DMEM write/read: 4 of 4 words OK`: ファルコンのメモリーに書いて読み返せる。
- `B1 priv ring`: GPU の中のレジスターの通り道(GPC などに要る)を、nouveau と同じ手順で初期化した結果。

固まったら、PowerTest と同じく、長押し → R411 で起動 → `Status.cmd`。`last step` が 20〜23 なら B1 のどこか。

## 段階 B2a(DmaTest、0.3.7)

ファームウェアの検証役(ACR)は、DMA でメモリーからファームウェアを読む。その前に、GPU 用のページテーブル無しの「物理アドレス」で、PMU ファルコンの DMA が Windows のメモリーに届くかを確かめる。

`Install.cmd`(0.3.7 に入れ替え)→ `DmaTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `B2a SMMU`: GPU がメモリーの変換装置(SMMU)を通っているか。0.3.0 の実機では GPU だけが通っていた(ASID 0x80000000)。0.3.1 からは、Linux と同じく、テストのあいだだけ GPU を SMMU から外し(`GPU ASID during the test 0x00000000`)、終わったら戻す(`restored 0x80000000`)。0.3.1 の実機では、この書き込みが効かなかった(SMMU のレジスターは Windows から変えられないらしい)。0.3.2 からは、外せなくても読み込みの DMA は試す(読むだけなので害はない)。あわせてメモリーコントローラーのエラー記録(`B2a MC errors`)を読む。
- 0.3.3 の実機で、GPU が使う SMMU の ASID 0 の変換表は RAM の外(0x20608C000)を指していた。変換表を選ぶレジスターは書けるので、0.3.4 からはテストのあいだだけ、バッファのまわり 4 MB を「同じアドレスに変換する」表(ページディレクトリとページテーブル、各 4 KB)を作って ASID 0 に設定し、終わったら元の値に戻す(`B2a SMMU identity map`)。
- 0.3.4 の実機では、恒等の表を入れても DMA は終わらなかった。GPU のユニットのオン・オフ(PMC_ENABLE 0xC0012020)で、メモリーへの通り道(XBAR、L2、FB、HUB)がどれもオフだった。0.3.5 からは DMA の前に、nvgpu と同じくこの 4 つをリセットしてオンにする(`B2a memory path`)。
- 0.3.5 の実機では、その 4 つのビットは書いても変わらなかった(GM20B には無い)。0.3.6 からは、DMA の行き先の種類を 3 通り(sysmem 非コヒーレント、ローカル FB、sysmem コヒーレント)試し、試すたびに PMU ファルコンをリセットする。ファルコンのエラー(bit 5 = バスのエラー)と、GPU の MMU のエラー(PFIFO)も読む(`B2a try 1〜3`、`B2a PFIFO`)。
- 0.3.6 の実機では、3 通りとも DMA が始まらず、バスのエラーも無かった。0.3.7 からは、nouveau と同じく DMA の前にファルコンの外部インターフェース(ITFEN、0x048 の bit 0)をオンにする(`B2a falcon interface`)。
- 書き込み(DMEM → メモリー)は、読み込みが 64 語ぜんぶ合ったときだけする(行き先が正しいとわかってから書く)。
- `B2a DMA memory -> DMEM: 64 of 64 words OK`: Windows のメモリーから GPU に読み込めた。
- `B2a DMA DMEM -> memory: 64 of 64 words OK ... stray changes 0`: GPU から Windows のメモリーに、決めた場所だけ書けた。

固まったら、長押し → R411 で起動 → `Status.cmd`。`last step` が 30〜38 なら B2a のどこか(36〜38 = DMA の 1〜3 回目)。

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
- `PowerTest` = 1: 次にドライバーが起動したとき、1 回だけ GPU を起こす(始める前に 0 に戻る)。`PowerTest.cmd` が使う。2 なら B1 の調べものもする(`ProbeTest.cmd`)。3 なら B2a の DMA の確認をする(`DmaTest.cmd`)。
- `KeepOn` = 1: テストのあと GPU を止めない(段階 B の準備用。ふだんは使わない)。
