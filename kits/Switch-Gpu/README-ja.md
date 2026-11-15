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
| `AcrTest.cmd` | 段階 B3c。GPU の WPR 保護領域と SMMU の範囲を確認し、PMU の仮想アドレス空間を作って署名付き ACR を一度だけ起動する。2 秒で待ちを打ち切り、必ず GPU をリセットしてから一時メモリーと変換表を戻す |
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

## 段階 B3a / B3b(FwTest、0.4.16)

B2a の結論: 署名のない PMU から物理アドレスで DMA すると、GPU の MMU が権限違反(PRIV_VIOLATION)で止める。nvgpu と nouveau の本番の手順は、署名つきの ACR のブートローダーを PIO で入れて起動し、そのローダーが GPU の仮想アドレスで DMA する。B3a はその前半(ファームウェアの読み込みと PIO での書き込み)だけをする。

準備: 自分の linux-firmware の `nvidia/gm20b` フォルダーを、キットの中に `firmware\nvidia\gm20b` として写す(`acr\bl.bin` 832 バイトと `acr\ucode_load.bin` 18592 バイトを使う)。0.4.1 からは、次の段階のために `pmu\desc.bin`、`pmu\image.bin`、`pmu\sig.bin`、`gr\fecs_bl.bin`、`gr\fecs_inst.bin`、`gr\fecs_data.bin`、`gr\fecs_sig.bin` もレジストリに写す(ドライバーは届いたかと大きさを確かめるだけで、まだ使わない。`B3a other firmware in the registry`)。キットにはファームウェアを入れていない。キットの中に無いときは、`C:\SwitchWOA\firmware\nvidia\gm20b`、`C:\firmware\nvidia\gm20b`、`C:\lib\firmware\nvidia\gm20b` の順に探す。どこにも無いときは、前の FwTest がレジストリに残したものを使う(`firmware ... (registry)` と出る)。

`Install.cmd`(0.4.16 に入れ替え)→ `FwTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `firmware acr\bl.bin: ... SHA256 ...`: キットがレジストリに写したファイル。
- `B3a firmware` / `B3a load header`: ヘッダーの値。ドライバーはどの値もファイルの大きさの内側にあるかを確かめてから使う。
- `B3a signed image`: ヒューズ(production / debug)に合う署名を、像の決まった場所に当てた。
- `B3a PMU PIO: ... N of N words read back OK`: ブートローダーが IMEM の上の端に書けた。
- `B3a BL descriptor at DMEM 0: 19 of 19 words read back OK`
- `B3a result 0x00000000 (OK)`

FwTest はCPU側のファームウェア確認だけで、ローダーが使うDMAアドレスは0のまま、起動もしない。実機での起動は次の独立モード。

### ACR起動(AcrTest、PowerTest=5)

**FwTestの結果を確認してから**使う。実機で確認されたGPUのWPR容量範囲は`0xFEB00000..0xFEB40000`(256 KiB)です。GM20B用Nouveauでは、この範囲を容量確認に使い、WPR像自体は別のINSTメモリーとして確保してACR descriptorの`ucode_blob_base`/`ucode_blob_size`に渡します。AcrTestもこの方式に変更し、通常の連続メモリーにWPR像を置き、512 KiB DMAアリーナにはPMU用の仮想アドレス表を作ります。ACR descriptorのWPR blob情報と`code_dma_base`/`data_dma_base`を設定し、PMUのinstance blockをbindしてから、ACRを一度だけ起動します。待ちは最大2秒。終了時はGPUをリセットしてからSMMU設定を復元し、メモリーを解放します。起動後にPMUが継続実行する場合もあるため、**フリーズ時は電源長押しが必要になる可能性がある**。

`Install.cmd`(0.4.16に入れ替え)→ `AcrTest.cmd` → `SUMMARY`を写真に撮る。Step 47〜56と`AcrBootResult`を確認する。`AcrWprCpuReadback`と`AcrSmmuRestored`はSUMMARYにも表示される。`ACR VM walk`にはGPU側PGD/PTEのindexと64-bit entryが出る。0.4.12では、GPU側のページ表を格納するページに対する外側Tegra SMMUのPDEと、Arena/PGD/PGT/instance/image各ページのPTEを読み返して期待値と並べる。`AcrSmmuMapReadback=1`はメモリ上の表と期待値のreadback一致、`0`は不一致、`2`はGPU SMMU bypassで直接物理アドレスを使ったことを示す。これはCPUから読んだ表の内容の確認で、GPU DMAが成功した証明ではない。また、読戻し不一致でも診断用にログするだけで、従来のACR起動フローは変えない。

PDB個別の無効化(Step 54)の後、0.4.11から診断用に全PDBのPAGE_ALL無効化(Step 55、`0x80000003`)も要求してからACRを起動する。これは「個別PDBの無効化範囲またはキャッシュ残留が原因か」を切り分ける追加試験で、全PDB無効化が修正になるとは未確認。`0x100CB8`の読み返しは診断値で、成功判定には使わない。無効化前後の状態、空きスロット数、要求値、ポーリング値を記録する。bit 15 は要求前から1の場合があるため、SUMMARYも要求完了の証明とは扱わない。空きスロットを得られない場合や要求後にbit 15を観測できない場合はACRを起動せず、個別要求は`0xE000006A`、全PDB要求は`0xE000006B`で終了する。待ちは各最大10 ms。`AcrBootResult=0`はブート完了とmailbox0=0を示すが、後続のFECS起動や画面出力までの成功を意味しない。`AcrBindOk`、`AcrDescOk`、両方のPDB invalidateの状態、`AcrBootPolls`、mailbox、IRQ、PCも記録される。

0.4.5 の実機では、ACR は起動したが 2 秒で終わらなかった(CPUCTL 0、mailbox0 は 0xCAFEBEEF のまま)。0.4.6でpriv ring、PFIFO、FB flushを足すと、ブートローダーのDMAがGPU MMUに達したが、VA 0x10053000のPDE faultで停止した。0.4.7でNouveauのGM20B設定に合わせPTE apertureを修正してもfaultは変わらず、PGD/PTE entry自体は想定値と一致していた。0.4.8はMMU invalidate前の空きスロット待ちで停止し、`0x100C80=0x00208001`を記録した。この値のビット23:16は`0x20`で、空きスロットが32個ある状態だった。0.4.9で条件を修正し、空きスロット待ちを通過したが、0x100CB8の読み返し不一致で停止した。Nouveauは0x100CB8を読み返さずに書き込み、無効化を要求するため、0.4.10では読み返しを診断値にし、無効化要求へ進める。要求前からbit 15が1である場合があるので、要求前の値と要求後のポーリング結果を併記する。PDE faultの解消は実機で未確認。待ちの後には、ファルコンのPCトレース、DMAの状態、MCとSMMUのエラー、PFIFOのMMUエラーを記録する(`B3c after the boot wait`、`B3c faults during the boot`、`B3c MMU fault record`)。

実機では未確認。特にWPR範囲へのCPU書き込み可能性、SMMUの新しい表によるDMA、ACRのmailbox値はテスト前に保証できない。`Status.cmd`で結果を読み、フリーズ後は電源長押しで起動し直す。通常FwTestはファルコンを起動しないまま。

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

## 0.4.13: GPU PDEの4 KiB SPT配置を修正

mesa-switch-woaを手掛かりにLinux本家v6.12のgf100_vmm_pgd_pde()を再確認したところ、従来の式は4 KiB用SPTの住所を大ページ用LPTの下位側に書いていた。0.4.13ではPDEを `(pgtIova << 24) | (3ULL << 32)` に修正し、下位32 bitを0、上位32 bitをSPTの住所とNCOH属性にする。既存の128 KiB大ページ構成(dir shift=27、SPT index mask=0x7fff)とPDB、PTE属性は継続する。これは64 KiB構成を使う別実装を一式取り込む変更ではない。

直近0.4.11ログのPgtIova=0xFB9DA000なら、PDEは以前の00000000:00FB9DA3から00FB9DA3:00000000になる。GPU MMUのPDE faultが変わるかを確認する。ACR boot成功は実機未確認。0.4.12の外側SMMU読戻し診断も含むので、Install.cmd後にAcrTest.cmdを1回実行し、C:\SwitchWOA-Gpu.txt全体を保存する。

比較資料:
- https://github.com/ruje0504-2/mesa-switch-woa/blob/main/tegra-x1-wddm/core/gm20b_mmu.c
- https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmgf100.c
- https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/subdev/mmu/vmmgm20b.c

別実装ではSoC向けのPDE/PDB target、VOL、64 KiB構成にも違いがある。これは残る比較候補で、今回の単独修正で原因が確定したという意味ではない。参照先のビルド済みドライバーやスクリプトは実行していない。上流コードの許諾はNOTICE-GM20B-MMU.txtを参照。
## 0.4.14: SoC DRAM向けのページ表target/VOLを検証

引継ぎメモの0.4.13結果は、外側SMMU表のCPU読戻しがすべて一致し、instanceページの住所もfaultと一致した一方、VA 0x10053000のPDE faultは変わらなかった。0.4.14ではmesa-switch-woaのSoC DRAM向け設定を仮説として検証する。実機での改善は未確認。

変更は同じページ表経路に関する3か所: RAMIN+0x200のPDBをtarget=VIDMEM(0)、VOL=1(bit2)とし、PDE上位dwordをVIDMEM(1)+VOL(4)の5にし、選択PDB無効化のtargetもVIDMEM(0)にそろえる。PMU NEW_INSTBLKのSYS_NCOH(3)は別のレジスター形式なので継続する。128 KiB big-page構成(bit11=0、PGD index=VA>>27、SPT mask=0x7fff)、GPU PTE、4 MB SMMUウィンドウと有限待ち/リセット手順は従来どおり。

SUMMARYのACR table-route probe行に64-bit PDB、VA limitとtarget/VOLを表示する。0.4.13ログのarena=0xFE5E5000を例にすると、PDBは0xFE5E7004、PDEは0x00FE5F75:0x00000000、選択PDB無効化値は0x00FE5E70になる。住所は毎回変わるので実測のAcrArenaIovaと比較する。

Install.cmd → AcrTest.cmdを1回実行し、C:\SwitchWOA-Gpu.txt全体を保存する。同じPDE faultが続くか、別のfault/PCに進むかを確認する。AcrBootResult=0だけでPMU/FECS全体の起動や描画成功とは判定しない。参照コードはMITのMMU部分のみ。参照プロジェクトのビルド済みバイナリやスクリプトは実行していない。
## 0.4.15: GPU向けアドレスのIOMMU bit 34を検証

0.4.14でもVA 0x10053000のPDE faultが継続したとの実機報告を受け、Linux nouveau v6.12のnouveau_platform.cとinstmem/gk20a.cを再確認した。GM20Bのiommu_bitは34であり、IOMMU経由で確保したGPU向けアドレスにはbit34を付けている。従来のAcrVmPrepareはCPU物理住所をULONGのままGPUへ渡していたため、この区別を表現できていなかった。

0.4.15ではSMMU経路が有効なとき、arenaのGPU向け住所をIOVA | (1ULL<<34)とし、インスタンス、PDB、PDE、PTE、選択PDB無効化まで64-bitで伝える。SoC SMMU表に書く物理PFN/IOVA、CPUポインター、MCのPTB_DATAにはこの印を入れない。SMMU bypassなら従来どおりタグを付けない。GPU VA(0x10053000)は変えない。

SUMMARYにはACR IOMMU tag probeとNEW_INSTBLK encoded/readを追加。0.4.14ログのarena=0xFE5DC000を例にすると、GPU instance=0x00000004:FE62E000、PDB=0x00000004:FE5DE004、PDE=0x04FE5EE5:00000000、PTE=0x00000000:04FE62F1、無効化要求のPDB値=0x04FE5DE0、NEW_INSTBLK書込値=0x704FE62Eになる。実際の配置は毎回異なる。faultのinstにタグが含まれるかも比較する。

これはIOMMUアドレス選択の仮説検証であり、原因やACR成功は実機未確認。今回PRAMINによる別経路のメモリー読み取りは追加していない。128 KiB構成、0.4.14のtable target/VOL設定、既存の有限待ちとGPU reset後SMMU復元を使う。WPR blob descriptorは従来のCPU物理アドレスのままであり、その後のWPRアクセス経路まで検証済みとはしない。

Install.cmd → AcrTest.cmdを1回実行し、C:\SwitchWOA-Gpu.txt全体を保存する。IOMMU tag行、PDB/PGD/PTE、NEW_INSTBLK書込と読戻し、MCエラー、MMU fault、PC、最終SMMU restoredを比較する。途中の取得値だけで成功扱いにせず、AcrBootResultと最終状態も確認する。
## 0.4.16: MMUのper-instance大ページサイズ指定を有効化

起動経路全体の比較は同梱のCOMPARE-B3c-ja.mdに記録した。今回はPDEの式を変える代わりに、MMU_CTRL(0x100C80) bit11(USE_PDB_BIG_PAGE_SIZE)をセットする。これにより既存RAMIN PDB bit11=0の128 KiB構成を選ばせる。CPUから見た表と、MMUが採用する表の階層設定の不一致を検証する試験であり、原因や実機改善は未確認。

新しいStep57はbindとTLB無効化より前に実行する。読戻しでbit11が立たなければE000006CでACR起動をスキップする。成功なら従来のStep50/51/54/55/56へ進む。SUMMARYのACR page geometry probeでMMU_CTRL変更前後とbit11を確認する。DMACTL、ITFEN、TRANSCFG1はbind前の読み取りのみで、DMACTL.REQUIRE_CTX解除は今回行わない。次候補として比較表に記録した。

Install.cmd→AcrTest.cmdを1回実行し、C:\SwitchWOA-Gpu.txt全体を保存する。タイムアウト時はfaultのVA/type/inst、PC、mailboxが以前と同じかを比較する。GPU reset後のSMMU復元と最終電源状態も確認する。