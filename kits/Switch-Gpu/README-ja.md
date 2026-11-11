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
| `FwTest.cmd` | 段階 B3a。自分の linux-firmware の ACR のファームウェアを読み、署名を当てた像をメモリーに作り、ACR のブートローダーを PMU の IMEM と DMEM に直接(PIO)書いて読み返す。ファルコンは起動しない |
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

## 段階 B2a(DmaTest、0.3.17)

ファームウェアの検証役(ACR)は、DMA でメモリーからファームウェアを読む。その前に、GPU 用のページテーブル無しの「物理アドレス」で、PMU ファルコンの DMA が Windows のメモリーに届くかを確かめる。

`Install.cmd`(0.3.17 に入れ替え)→ `DmaTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `B2a SMMU`: GPU がメモリーの変換装置(SMMU)を通っているか。0.3.0 の実機では GPU だけが通っていた(ASID 0x80000000)。0.3.1 からは、Linux と同じく、テストのあいだだけ GPU を SMMU から外し(`GPU ASID during the test 0x00000000`)、終わったら戻す(`restored 0x80000000`)。0.3.1 の実機では、この書き込みが効かなかった(SMMU のレジスターは Windows から変えられないらしい)。0.3.2 からは、外せなくても読み込みの DMA は試す(読むだけなので害はない)。あわせてメモリーコントローラーのエラー記録(`B2a MC errors`)を読む。
- 0.3.3 の実機で、GPU が使う SMMU の ASID 0 の変換表は RAM の外(0x20608C000)を指していた。変換表を選ぶレジスターは書けるので、0.3.4 からはテストのあいだだけ、バッファのまわり 4 MB を「同じアドレスに変換する」表(ページディレクトリとページテーブル、各 4 KB)を作って ASID 0 に設定し、終わったら元の値に戻す(`B2a SMMU identity map`)。
- 0.3.4 の実機では、恒等の表を入れても DMA は終わらなかった。GPU のユニットのオン・オフ(PMC_ENABLE 0xC0012020)で、メモリーへの通り道(XBAR、L2、FB、HUB)がどれもオフだった。0.3.5 からは DMA の前に、nvgpu と同じくこの 4 つをリセットしてオンにする(`B2a memory path`)。
- 0.3.5 の実機では、その 4 つのビットは書いても変わらなかった(GM20B には無い)。0.3.6 からは、DMA の行き先の種類を 3 通り(sysmem 非コヒーレント、ローカル FB、sysmem コヒーレント)試し、試すたびに PMU ファルコンをリセットする。ファルコンのエラー(bit 5 = バスのエラー)と、GPU の MMU のエラー(PFIFO)も読む(`B2a try 1〜3`、`B2a PFIFO`)。
- 0.3.6 の実機では、3 通りとも DMA が始まらず、バスのエラーも無かった。0.3.7 ではDMA前にITFENをオンにした。0.3.8ではnouveauの順に、ITFENをbindより先にオンにしてから、PMUにinstance blockをbindし、statusの完了を確認する(`B2a PMU instance bind`)。PMUをリセットする再試行ではbindもやり直す。
- bind result が成功以外ならDMA試行はスキップされる。`B2a PMU bind handshake` のack/completion stateとpoll数で止まった位置を確認する。SMMU恒等マップを使う場合、instance blockとpage directoryも同じ4 MB内に必要。
- 0.3.8 の実機では、bind は成功したが DMA は始まらなかった。0.3.9 では読むだけの調べを 2 つ足す: (1) MC の割り込みを消してから DMA なしで 10 ms 待ち、0x200 が勝手に戻るか(`B2a MC int after 10 ms idle`)。(2) DMA の前後で MC のホットリセットの状態の bit 2 を読み、GPU の要求が MC まで出ているか(`B2a MC hot reset status`)。
- 0.3.9 の実機では、0x200 は DMA をしないと出なかった(GPU が原因)。bit 2 は DMA の前から 0 だったので、DMA との関係はわからなかった。0.3.10 では、電源を入れる途中(リセット解除の直後、フラッシュの後、BAR0 を読んだ後)と B2a の始めにも bit 2 を読み、どこで 0 になるかを見る(`MC hot reset bit 2 over the run`)。リセット解除やフラッシュの直後に 0 になるなら、bit 2 はフラッシュ完了の印にすぎない。電源を入れる途中の MC の割り込みも読む(`MC int while powering up`)。読むだけで、書き込みは増やさない。
- 0.3.10 の実機では、bit 2 はリセット解除の直後に 0 になった(フラッシュ完了の印にすぎず、DMA の調べには使えない)。0x200 は電源を入れる途中でも出ていたが、記録を消していなかったので古い記録かもしれない。0.3.11 では、電源を入れる初めに MC の割り込みの記録を消してから読み直す(`MC int while powering up`)。B2a では、bind の直後、DMA の各回の前と後で読み、読むたびに消す(`B2a MC int step by step`)。2 回目と 3 回目の「前」には、ファルコンのリセットと bind のやり直しが含まれる。
- 0.3.11 の実機では、電源を入れる途中の 0x200 は起動時の古い記録だった。B2a では 0x200 は「bind の直後」に 1 回だけ出て、DMA の 3 回の試しやリセット・bind のやり直しでは出なかった。0.3.12 では、その区間をバッファ確保、Step 35、Step 31(DMA の準備)、最初の bind に分け、それぞれの前で読んで消す。`B2a MC int step by step` の各値は、その名前の手順のあいだに出た割り込み。
- 0.3.12 の実機では、0x200 はバッファ確保の区間(GPU には触れず、SMMU の表の差し替えだけ)で出た。GPU のどの手順も MC の割り込みを出さなかったので、DMA は GPU の中で止まっているらしい。0.3.13 では、nouveau が GM20B で最初のファルコン DMA より前に設定する GPU 側のメモリーの通り道(FB の sysmem flush ページ、MMU 制御とデバッグバッファ、L2、PBUS の割り込みとフォールト)を、DMA の前後で読むだけ(`B2a GPU memory path`)。
- 0.3.13 の実機では、L2(LTC)のレジスターが PRI エラー(0xBADF3000)を返し、L2 の数(0x12006C)が 0 だった。B2a は GPU 内部の連絡網(priv ring)を初期化していなかった(B1 の Step 23 だけがしていた)。0.3.14 では、B1 と同じ初期化を B2a の始め(Step 29)にもして、すぐ後に通り道を読み直してから DMA を試す(`B2a priv ring`、`B2a GPU memory path right after the priv ring init`)。
- 0.3.14 の実機では、priv ring の初期化で L2 に届くようになった(L2 が 2 つ)が、DMA は始まらなかった。0.3.15 では、nouveau の GM20B と同じ FB の初期設定をする: sysmem flush ページ(0x100C10)と MMU のデバッグ用バッファ(0x100CC8 / 0x100CCC)に、確保したページ(SMMU の恒等マップの範囲内)の住所を入れ、0x100CC4 の大きさの欄を 0 にする。テストの後で元の値に戻し、ページは GPU のリセット後に解放する(`B2a FB init`)。
- 0.3.15 の実機では、FB の設定は効いたが DMA は始まらなかった。nvgpu と比べると、nvgpu は PMU より前に PFIFO をリセットして有効にし、FB のフラッシュもしている。0.3.16 では、priv ring の後に PFIFO を有効にして割り込みを消し(`B2a PFIFO enable`)、FB の設定の後に FB のフラッシュ(0x070000)をする(`B2a FB flush`)。PFIFO が有効になるので、`B2a PFIFO` の MMU のエラー記録が読めるようになるはず。テストの後で PFIFO はオフに戻す。
- 0.3.16 の実機で、DMA の後に GPU の MMU のエラーが初めて見えた(intr0 0x18000000、fault id 0x80000000)。0.3.17 では読むだけで、1 回目の前と各回の後に MMU のエラーの状態を読み(`B2a PFIFO MMU fault timeline`)、fault id で立っている装置と PMU(0x17)のエラーの記録を読んで種類・住所・読み書きを表示する(`B2a MMU fault record`)。
- 書き込み(DMEM → メモリー)は、読み込みが 64 語ぜんぶ合ったときだけする(行き先が正しいとわかってから書く)。
- `B2a DMA memory -> DMEM: 64 of 64 words OK`: Windows のメモリーから GPU に読み込めた。
- `B2a DMA DMEM -> memory: 64 of 64 words OK ... stray changes 0`: GPU から Windows のメモリーに、決めた場所だけ書けた。

固まったら、長押し → R411 で起動 → `Status.cmd`。`last step` が 30〜38 なら B2a のどこか(36〜38 = DMA の 1〜3 回目)。

## 段階 B3a / B3b(FwTest、0.4.2)

B2a の結論: 署名のない PMU から物理アドレスで DMA すると、GPU の MMU が権限違反(PRIV_VIOLATION)で止める。nvgpu と nouveau の本番の手順は、署名つきの ACR のブートローダーを PIO で入れて起動し、そのローダーが GPU の仮想アドレスで DMA する。B3a はその前半(ファームウェアの読み込みと PIO での書き込み)だけをする。

準備: 自分の linux-firmware の `nvidia/gm20b` フォルダーを、キットの中に `firmware\nvidia\gm20b` として写す(`acr\bl.bin` 832 バイトと `acr\ucode_load.bin` 18592 バイトを使う)。0.4.1 からは、次の段階のために `pmu\desc.bin`、`pmu\image.bin`、`pmu\sig.bin`、`gr\fecs_bl.bin`、`gr\fecs_inst.bin`、`gr\fecs_data.bin`、`gr\fecs_sig.bin` もレジストリに写す(ドライバーは届いたかと大きさを確かめるだけで、まだ使わない。`B3a other firmware in the registry`)。キットにはファームウェアを入れていない。キットの中に無いときは、`C:\SwitchWOA\firmware\nvidia\gm20b`、`C:\firmware\nvidia\gm20b`、`C:\lib\firmware\nvidia\gm20b` の順に探す。どこにも無いときは、前の FwTest がレジストリに残したものを使う(`firmware ... (registry)` と出る)。キットの中に無いときは、`C:\SwitchWOAirmware
vidia\gm20b`、`C:irmware
vidia\gm20b`、`C:\libirmware
vidia\gm20b` の順に探す。どこにも無いときは、前の FwTest がレジストリに残したものを使う(`firmware ... (registry)` と出る)。

`Install.cmd`(0.4.2 に入れ替え)→ `FwTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `firmware acr\bl.bin: ... SHA256 ...`: キットがレジストリに写したファイル。
- `B3a firmware` / `B3a load header`: ヘッダーの値。ドライバーはどの値もファイルの大きさの内側にあるかを確かめてから使う。
- `B3a signed image`: ヒューズ(production / debug)に合う署名を、像の決まった場所に当てた。
- `B3a PMU PIO: ... N of N words read back OK`: ブートローダーが IMEM の上の端に書けた。
- `B3a BL descriptor at DMEM 0: 19 of 19 words read back OK ... (not started)`
- `B3a result 0x00000000 (OK)`

ローダーが使う DMA のアドレス(`code_dma_base` と `data_dma_base`)は、PMU 用の仮想アドレス空間ができるまで 0 のまま。起動もしない。固まったら `Status.cmd`。`last step` が 40〜45 なら B3a のどこか。

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
