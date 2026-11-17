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
| `AcrTest.cmd` | 段階 B3c。GPU の WPR 保護領域と SMMU の範囲を確認し、署名付き ACR を一度だけ起動した後、LS PMU と FECS の bootstrap を試す。待ちは上限付きで、必ず GPU をリセットしてから一時メモリーと変換表を戻す |
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

## 段階 B3a / B3b(FwTest、0.4.25)

B2a の結論: 署名のない PMU から物理アドレスで DMA すると、GPU の MMU が権限違反(PRIV_VIOLATION)で止める。nvgpu と nouveau の本番の手順は、署名つきの ACR のブートローダーを PIO で入れて起動し、そのローダーが GPU の仮想アドレスで DMA する。B3a はその前半(ファームウェアの読み込みと PIO での書き込み)だけをする。

準備: 自分の linux-firmware の `nvidia/gm20b` フォルダーを、キットの中に `firmware\nvidia\gm20b` として写す(`acr\bl.bin` 832 バイトと `acr\ucode_load.bin` 18592 バイトを使う)。0.4.1 からは、次の段階のために `pmu\desc.bin`、`pmu\image.bin`、`pmu\sig.bin`、`gr\fecs_bl.bin`、`gr\fecs_inst.bin`、`gr\fecs_data.bin`、`gr\fecs_sig.bin` もレジストリに写す(ドライバーは届いたかと大きさを確かめるだけで、まだ使わない。`B3a other firmware in the registry`)。キットにはファームウェアを入れていない。キットの中に無いときは、`C:\SwitchWOA\firmware\nvidia\gm20b`、`C:\firmware\nvidia\gm20b`、`C:\lib\firmware\nvidia\gm20b` の順に探す。どこにも無いときは、前の FwTest がレジストリに残したものを使う(`firmware ... (registry)` と出る)。

`Install.cmd`(0.4.32 に入れ替え)→ `FwTest.cmd` → `SUMMARY` を写真に撮る。見たいもの:
- `firmware acr\bl.bin: ... SHA256 ...`: キットがレジストリに写したファイル。
- `B3a firmware` / `B3a load header`: ヘッダーの値。ドライバーはどの値もファイルの大きさの内側にあるかを確かめてから使う。
- `B3a signed image`: ヒューズ(production / debug)に合う署名を、像の決まった場所に当てた。
- `B3a PMU PIO: ... N of N words read back OK`: ブートローダーが IMEM の上の端に書けた。
- `B3a BL descriptor at DMEM 0: 19 of 19 words read back OK`
- `B3a result 0x00000000 (OK)`

FwTest はCPU側のファームウェア確認だけで、ローダーが使うDMAアドレスは0のまま、起動もしない。実機での起動は次の独立モード。

### ACR起動(AcrTest、PowerTest=5)

**FwTestの結果を確認してから**使う。実機で確認されたGPUのWPR容量範囲は`0xFEB00000..0xFEB40000`(256 KiB)です。GM20B用Nouveauでは、この範囲を容量確認に使い、WPR像自体は別のINSTメモリーとして確保してACR descriptorの`ucode_blob_base`/`ucode_blob_size`に渡します。AcrTestもこの方式に変更し、通常の連続メモリーにWPR像を置き、512 KiB DMAアリーナにはPMU用の仮想アドレス表を作ります。ACR descriptorのWPR blob情報と`code_dma_base`/`data_dma_base`を設定し、PMUのinstance blockをbindしてから、ACRを一度だけ起動します。待ちは最大2秒。終了時はGPUをリセットしてからSMMU設定を復元し、メモリーを解放します。起動後にPMUが継続実行する場合もあるため、**フリーズ時は電源長押しが必要になる可能性がある**。

`Install.cmd`(0.4.32に入れ替え)→ `AcrTest.cmd` → `SUMMARY`を写真に撮る。Step 47〜72、`AcrBootResult`、`B3cLsResult`、`B3cFecsResult`を確認する。`AcrWprCpuReadback`と`AcrSmmuRestored`はSUMMARYにも表示される。`ACR VM walk`にはGPU側PGD/PTEのindexと64-bit entryが出る。0.4.12では、GPU側のページ表を格納するページに対する外側Tegra SMMUのPDEと、Arena/PGD/PGT/instance/image各ページのPTEを読み返して期待値と並べる。`AcrSmmuMapReadback=1`はメモリ上の表と期待値のreadback一致、`0`は不一致、`2`はGPU SMMU bypassで直接物理アドレスを使ったことを示す。これはCPUから読んだ表の内容の確認で、GPU DMAが成功した証明ではない。また、読戻し不一致でも診断用にログするだけで、従来のACR起動フローは変えない。

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

## 0.4.17: ACR の後のファルコンの状態(読むだけ)

0.4.16 の実機で ACR が最後まで動いた(結果 0、CPUCTL 0x50、mailbox0 0、MMU の fault なし)。段階 B の完了条件は ACR が PMU と FECS を起動することなので、0.4.17 では ACR の後のファルコンの状態を読むだけで記録する。

- PMU: CPUCTL、SCTL(ビット 13〜12 がセキュリティのモード)、mailbox0/1
- FECS(0x409000)と GPCCS(0x41A000): CPUCTL、SCTL、mailbox0。PMC_ENABLE で GR(ビット 12)がすでに有効なときだけ読む。GR を有効にする書き込みはしない。

`SUMMARY` の `B3c falcons after ACR` と `B3c GR falcons after ACR` の行を見る。書き込みは増やしていない。

## 0.4.19: LS PMU の開始と INIT メッセージ

0.4.18 の実機では、ACR は成功し、その直後の PMU は CPUCTL=0x50、SCTL=0x7021、mailbox=0/0 だった。Step 58 の後は CPUCTL=0x40、SCTL/mailbox は変化せず、MSGQ head/tail(0x4C8/0x4CC)も0/0のまま2秒でタイムアウトした。PMU PC/IRQ と詳細設定値が写真になく、起動失敗点は確定できない。

参照コードの比較では、ACR 成功後に PMU をもう一度リセットしたりファームウェアを再読込したりする手順は見つからなかった。nvgpu は ACR 側の Falcon を HS bootstrap 前に reset し、ACR 後は `CPUCTL_ALIAS.STARTCPU` を書いて LS PMU を開始する。nouveau の `gm20b_pmu_init()` は `nvkm_falcon_get()` 後に args を DMEM へ書き、`nvkm_falcon_start()` を呼ぶ。mesa `tg_gpu_pmu_start()` は PMU halted / secure / mailbox 0 の状態確認、HALT interrupt clear、aperture 設定、32-byte `cmdline_args_v3`(204 MHz、secure_mode=true)、CPUCTL_ALIAS=2 の順で、MSGQ INIT を待つ。0.4.19 はこの順を維持し、args PIO の全word readbackと cursor を検査する。失敗は E0000073。

Step 58 は設定と開始、Step 59 は最大2秒の INIT 待ちとヘッダー読み取り。キュー先頭が unit=7、size=42、msg_type=0 で PIO cursor が44-byte 境界へ進んだ場合だけ INIT と判定する。その他の形式は E0000072、無効なqueue pointerは E0000071。INIT payload はまだ acknowledge せず、WPR init command や FECS の起動にも進まない。タイムアウト時は IRQSTAT、CPUCTL/SCTL/mailbox、PMU PC trace、DMEM size、aperture、args offset/cursor を SUMMARY で確認する。PMU CPUCTL=0x40 は実機の観測値であり、ここでは実行状態の成功判定に使わない。

## 0.4.20: PMU の待ち状態の記録

0.4.19 の実機では LS RTOS の PC trace が 7 個すべて 0x3256、INIT queue head/tail は 0/0 のまま 2 秒で timeout した。比較した MIT ソースには、この実行時ファームウェア内の PC 0x3256 とシンボルの対応はないため、待ちの対象は未確定。0.4.20 は起動手順を変えず、Step 60 で IRQMODE/MASK/DEST、Falcon GPTMR、GPU PTIMER、cmdline args の前後、INIT queue pointer 周辺 DMEM を読み取り専用で記録する。DMEM の内容は B3cLsPmuArgsWindow と B3cLsPmuQueueDataWindow のバイナリ Trace に出る。実機では PC trace とこれらの値を比較し、次の変更箇所を絞る。

## 0.4.21: argv 指定位置と PMU 引数 ABI の一致

実機の 0.4.20 では WPR の `B3bPmuArgv=0x5FD4` に対して Step 58 は `0x5FE0` へ 32 バイトを書いていた。参照コードを照合すると、この WPR の PMU loader_config は `argv = DMEM - sizeof(struct nv_pmu_args)` を使い、GM20B nouveau の `gm20b_pmu_init()` は同じ 44 バイト構造をその位置へ書く。

`nv_pmu_args` の frequency は byte 4、secure_mode は byte 24。mesa `tg_gpu_pmu_start()` の 32 バイト `cmdline_args_v3` は別の直接起動経路で使われるため、WPR が指定する argv ABI より優先しない。

0.4.21 は `B3bPmuArgv` を Step 58 に渡し、指定位置と `DMEM - 44` の一致を確認してから 44 バイトを書き込む。不一致は `E0000074` で止める。書いた 11 word の readback は `B3cLsPmuArgvReadback` に出る。実機では `B3cPmuAppVersion`、argv の advertised/computed/match、size=44、args readback=1 と、DMEM window の先頭 11 word を確認する。想定値は `00000000 0C28CB00 00000000 00000000 00000000 00000000 00000001 00000000 00000000 00000000 00000000`。timeout が続く場合も ACR は再試行せず、既存の GPU reset 経路に入る。

## 0.4.22: LS PMU の IMEM / DMA / FBIF 状態記録

0.4.21 では argv の advertised/computed/readback は一致したが、LS PMU は INIT を出さず、PC trace は 7 回すべて `0x3256` だった。0.4.22 は起動順や DMA 設定を書き換えず、Step 61 で最後の PC 付近の IMEM 16 word、IMEM tag、DMATRFCMD/BASE/MOFFS/FBOFFS、DMACTL、bind/context、FBIF TRANSCFG 0〜7、PFIFO fault、MC error を読み取る。DMEM の args 周辺と queue 周辺も各最大 256 byte まで広げる。

IMEM の PIO read は nvgpu `gk20a_flcn_copy_from_imem()` のブロック/offset/auto-increment 手順を参照する。起動前 bind の手順は参考先にあるが、ACR 後に同じ bind や DMA 設定を再実行する根拠は確認できなかったため、今回は状態の記録だけにした。実機では `B3c LS PMU IMEM` と `IMEM around last PC`、`loader DMA`、`FBIF`、`post-wait faults`、`MMU fault records`、拡張 DMEM windows を確認する。`B3cLsResult=E0000070` は従来どおり INIT timeout を示し、必ず GPU reset 経路に入る。

## 0.4.23: PMU UCODE DMA aperture と古い fault latch の切り分け

0.4.22 では PMU client の fault が VA `0x1000` で発生し、PFIFO fault ID bit 31 が立った。WPR の PMU LSF loader config が DMA index 4 を指定していた一方、nvgpu の `GK20A_PMU_DMAIDX_UCODE` は 0。GM20B の起動前 aperture では index 0 が physical local FB、index 4 が noncoherent sysmem なので、WPR ローダーの設定先とアドレス空間の不一致と判断し、PMU descriptor の index を 0 に修正した。これは実機での再確認前の仮説に基づく修正で、0.4.23 の実機結果で判定する。0.4.23 では WPR 相対 offset を使っていたが、0.4.24 の実機 fault 再発後に nvgpu / Nouveau の処理を再確認した。0.4.25 で PMU の code/data DMA base に絶対 WPR address を設定する。

Step 62 は ACR 直前に fault ID で有効な unit の記録を保存し、Nouveau `gk104_fifo_intr()` の W1C 手順で `0x259C` と PFIFO INTR0 の MMU 関連 bit を ACK して状態を再読する。fault payload レジスターは ACK 後も残り得るので、前後の payload を SUMMARY に 16 進表示する。ID bit が消えなければ `E0000075` で ACR を起動せず、GPU reset 経路に入る。実機では `B3b PMU LS loader` の DMA index 0、Step 62 の clear result、Step 61 の fault id/record を確認する。ログの `B3cLsPmuFaultRecs` は fault ID bit が立っていた unit だけを含む。

## 0.4.24: LS 開始前の DMACTL.REQUIRE_CTX

0.4.23 では、WPR PMU loader config の DMA index を 0 にしても、LS 起動後に PMU fault (unit 0x1F、inst 0、VA 0x1000) が再発した。DMEM offset 0 は ACR boot descriptor であり、WPR の loader config とは別のデータである。`B3bPmuBld` は WPR に組み立てた loader config 全体を示す。SUMMARY に全 word を 16 進表示し、先頭 word の DMA index と `B3bPmuBldOff` を確認できる。`AcrWprCpuReadback=1` はその WPR 像を ACR 用 buffer にコピーした後の CPU readback 一致を示す。

nvgpu `gk20a_falcon_bootstrap()` は Falcon 開始前に `DMACTL.REQUIRE_CTX=0` を設定してから BOOTVEC / STARTCPU を書く。実機では ACR 直後の DMACTL が 1 だったため、0.4.24 は Step 63 で DMACTL を 0 にしようとしたが、実機では 0x1 のまま変わらず `E0000076` で LS start を中止した。0.4.25 では Step 63 を read-only の DMACTL snapshot に戻す。

実機では `B3b PMU WPR loader-config words`、`B3c LS start DMACTL REQUIRE_CTX`、fault の inst/VA、INIT queue と PC trace を確認する。WPR loader config が index 0 で DMACTL が 0 になっても fault が続く場合、この仮説は支持されない。FECS/GPCCS は今回の範囲外。
## 0.4.25: PMU loader の DMA base に hardware WPR address を含める

再確認で、PMU LSF loader config の `code_dma_base` / `data_dma_base` は WPR image 内の相対値のままではなく、WPR 物理 base を加えたアドレスを 256-byte 単位で格納すると確認した。nvgpu `gm20b_pmu_populate_loader_cfg()` は `wpr_base + ucode_off + app offsets` を使い、Nouveau `gm200_acr_wpr_patch()` は `gm20b_pmu_acr_bld_patch()` を呼んで WPR base relocation を行う。0.4.22/0.4.23 の fault VA `0x1000` は、WPR base を欠いた code DMA address に合うため、0.4.25 は PMU の code/data base に hardware WPR start を加える。表現できない値は `E0000077` で停止する。

0.4.24 で書き込みが無効だった DMACTL は変更しない。Step 63 は開始直前の値を読むだけ。Step 64 で loader config を生成し、`B3bPmuBld` 全 word、`B3cPmuWprBaseLo/Hi`、`B3cPmuCodeDmaBase` / `B3cPmuDataDmaBase`、`AcrWprCpuReadback` を確認できるようにする。DMEM offset 0 の ACR boot descriptor はこの loader config と別物である。

実機では SUMMARY の `B3b PMU WPR loader-config words` の word 0〜5、`B3cPmuWprBaseLo/Hi`、`B3cPmuCodeDmaBase` / `B3cPmuDataDmaBase`、`B3c LS start DMACTL snapshot`、PMU fault の inst/VA、PC trace と INIT queue を確認する。DMA base が WPR 範囲内になっても同じ fault が出る場合、この仮説は支持されない。FECS/GPCCS は今回の範囲外。
## 0.4.26: PMU から FECS LS を bootstrap

PMU LS が INIT を出した後、Step 65 で INIT の 42-byte payload から HPQ/MSGQ descriptor を取り出し、DMEM 範囲、queue index、重なり、INIT cursor を検証してから INIT を ACK する。Step 66 では PMC_ENABLE の GR bit 12 だけを有効にし、nvgpu `gm20b_load_falcon_ucode()` の順に FECS context-switch mailbox 0 を clear する。Step 67 は PMU ACR unit の bootstrap-falcon command (FECS id 2、RESET_YES) を一度だけ HPQ に post し、最大 2 秒で reply と `0x55AA55AA` ready mailbox を待つ。Step 68 で FECS CPUCTL/SCTL/mailbox、PC trace、IRQ、PFIFO fault を読み取る。失敗は `E0000078` として既存の GPU reset 経路へ進む。

FECS WPR loader descriptor の DMA index は `DMAIDX_UCODE=0`、code/data DMA base は hardware WPR base を加えた物理 address とする。nvgpu `gm20b_gr_acr_bld_write()` は WPR base を含む FECS descriptor を作り、Nouveau `gm20b_gr_acr_bld_patch()` は ACR 前に base relocation を行う。この WPR 修正と PMU command/reply の初回実機確認は未実施。FECS reply payload の ID 解釈、ctxsw mailbox clear register の実機 readback、および GR enable の readback を確認する。FECS が ready でも GR context/channel 初期化や描画成功は意味しない。GPCCS は起動しない。

実機では SUMMARY の `B3c FECS bootstrap result`、HPQ/MSGQ、reply、ready mailbox、FECS CPUCTL/SCTL/mailboxes/PC trace、PFIFO fault records、および `FECS WPR BLD words` を確認する。`B3cFecsResult=0` と ready mailbox `0x55AA55AA` が揃えば FECS LS bootstrap の完了を示す。

## 0.4.27: BAR0 を 16 MB すべてマップ

0.4.26 は FECS のレジスター(0x409000 など)を読んだところでブルースクリーン(PAGE_FAULT_IN_NONPAGED_AREA、switchgpu.sys)になった。ドライバーが BAR0 を 2 MB(0x200000)しかマップしておらず、0x400000 より上の GR/FECS のレジスターがマップの外だったため。ACPI の BAR0 は 16 MB(0x57000000、0x01000000)なので、0.4.27 では 16 MB すべてをマップする。FECS の処理そのものは 0.4.26 と同じ。試し方も同じで、`Install.cmd`(0.4.28 に入れ替え)→ `AcrTest.cmd`。

## 0.4.28: PMU の WPR 初期化を FECS bootstrap より先に実行

0.4.27 では FECS bootstrap command を HPQ に書いた後、PMU message queue に返信が現れず `E0000078` になった。比較すると、nvgpu `gm20b_pmu_init_acr()` は `INIT_WPR_REGION` を先に post し、`pmu_handle_acr_init_wpr_msg()` が成功を記録した後に `gm20b_pmu_load_lsf()` が FECS bootstrap を post する。mesa-switch-woa の `post_wpr()` / `wpr_reply()` も同じ順序で WPR region 1 / offset 0 command を送り、12-byte ACR reply の success code を検査する。0.4.28 はこの前提の欠落を補う。

Step 65 は INIT queue descriptor を検証し、Step 66 は INIT_WPR_REGION command を HPQ に一度 post して最大2秒 reply を待つ。reply unit/size/sequence/type と success code を確認し、失敗は `E0000079`。その後 Step 67 で GR bit 12 を有効にして FECS ctxsw mailbox clear を行い、Step 68 で bootstrap command を一度 post する。Step 69 で FECS state、PFIFO fault、HPQ head/tail と MSGQ head/tail を読む。mesa `post_wpr()` は queue head を更新して command を公開し、別の doorbell write は行っていない。nvgpu の返信処理と mesa の `wpr_reply()` は PMU message head/tail `0x4C8/0x4CC` を使用する。

実機では `B3cFecsWprReplyPolls` / `B3cFecsWprReplyHeader` / `B3cFecsWprReplyCode`、bootstrap reply、`B3cFecsHpqTailAfterWait`、MSGQ head/tail、ctxsw mailbox clear 前後と clear register readback を確認する。ctxsw clear の register address は nvgpu の `gr_fecs_ctxsw_mailbox_clear_r(0)` に基づくが、0.4.27 では mailbox 値が変わらなかったため、今回のログでその挙動を再確認する。0.4.28 はビルド・実機未確認で、FECS ready 成功を保証しない。
## 0.4.29: 0.4.28 のブルースクリーンを修正

0.4.28 は WPR の初期化の返事まで成功した(返事 1 ms、code 0)が、その後の Step 68 で PAGE_FAULT_IN_NONPAGED_AREA になった。HPQ の head レジスターの位置を入れていた変数を、Step 67 で PMC_ENABLE の値(0xC0013120)の保存に使い回していたため、Step 68 で PMU_FALCON + 0xC0013120 という BAR0 の外を読んでいた。0.4.29 では PMC_ENABLE 用に別の変数を使う。ほかの処理は 0.4.28 と同じ。試し方も同じで、`Install.cmd`(0.4.29 に入れ替え)→ `AcrTest.cmd`。

## 0.4.30: FECS bootstrap command の sequence を進める

0.4.29 では `INIT_WPR_REGION` の reply は sequence 1 で成功し、PMU はこの command を dequeue した。FECS bootstrap も sequence 1 だった。nvgpu の `pmu_seq_acquire()` は command ごとに sequence slot を確保し、reply を対応付けるため、既に完了した command と同じ sequence を続けて使う現在の実装は違いの候補になる。ただし公開されている呼び出し側だけでは、次の ID が必ず 2 になるとは確認できない。0.4.30 は切り分けとして WPR init sequence の次の値(2)を FECS bootstrap に設定し、reply も同じ sequence であることを検証する。queue head の公開方法、command payload、GR/FECS reset 手順は変えない。nvgpu `gm20b_pmu_load_lsf()` は `RESET_YES` flag を ACR command に渡すので、独立した FECS reset write は追加しない。

Step 69 は bootstrap post 直後の PMU IRQSTAT/PC trace と最大2秒間の HPQ tail/IRQSTAT sample を記録する。Step 70 は最終 PMU/FECS state、HPQ/MSGQ pointer、fault を記録する。PMU が bootstrap command の新しい head まで dequeue しなかった場合は `E000007A`、dequeue 後に reply/ready を得られない場合は `E0000078` とする。

実機では `B3cFecsBootstrapSeq` が 2、`B3cFecsBootstrapCommand` の header が `0x02030C0A`、`B3cFecsHpqTailSamples` が command head `0x5048` まで進むかを確認する。併せて `B3cFecsPmuTraceAfterPost` / `B3cFecsPmuTraceAfterWait`、PMU IRQSTAT、MSGQ head/tail、FECS ready mailbox と fault records を見る。sequence 再利用が原因か、2 が適切な次 ID かは未確認。`Install.cmd`(0.4.32 に入れ替え)→ `AcrTest.cmd` を一度実行し、ログを保存する。

## 0.4.31: FECS 準備中の PMU HALT を切り分け

0.4.30 では WPR init の reply 後に PMU IRQSTAT が `0x51` となり、その後の PC trace は変化しなかった。Step 71 は PMU CPUCTL/IRQSTAT/mailbox0/1/PC trace index/trace[0]/EXCI を、WPR ACK 後、GR enable 後、FECS ctxsw mailbox clear の段階、bootstrap 直前の順で記録する。EXCI は nvgpu `gk20a_falcon_dump_stats()` と同じ ICD 読み出しで取得する。

切り分けのため、0.4.31 は `0x419A80` の ctxsw mailbox clear write を一度省く。nvgpu `gm20b_load_falcon_ucode()` はこの write を FECS bootstrap 前に行うため、今回の変更は参照実装との差分を試す診断である。bootstrap 直前に PMU CPUCTL HALT bit が立っていれば command を送らず `E000007B` で既存の GPU reset 経路へ進む。GR enable や ACR の順序は変更しない。

SUMMARY の `B3cPmuCheckpointAfterWprAck`、`B3cPmuCheckpointAfterGrEnable`、`B3cPmuCheckpointAfterCtxswClearStage`、`B3cPmuCheckpointBeforeBootstrap` は各7 word(CPUCTL、IRQSTAT、mailbox0、mailbox1、trace index、trace[0]、EXCI)で、この順に表示される。ctxsw clear write の省略で HALT が消えるかは実機未確認。FECS bootstrap 成功も未確認。

## 0.4.32: FECS bootstrap command の構造体を修正

0.4.31 の command は 12 byte で、payload の先頭 byte が `0x02` だった。nvgpu `gm20b_load_falcon_ucode()` / `gm20b_pmu_load_lsf()` と Nouveau `gm20b_pmu_acr_bootstrap_falcon()` の定義では、ACR unit `0x0A`、command ID `0x01`、`RESET_YES=0`、FECS falcon ID `2` を使い、command 全体は 16 byte になる。header size は 16。command ID の後には 3 byte padding があり、その後に 32-bit flags と 32-bit falcon ID が続く。0.4.32 はこの配置に合わせ、`0x419A80` mailbox clear write を復元する。

Step 72 は bootstrap command を一度 post し、post 直後の PMU CPUCTL を記録する。post 直後に HALT bit が立った場合は `E000007C`、bootstrap 前に既に立っていた場合は `E000007B` とする。各待ちは従来どおり上限付き。実機では SUMMARY の `B3cFecsBootstrapCommand` が `0203100A 00000001 00000000 00000002`、`B3cFecsPmuCpuctlAfterPost`、HPQ tail、reply、FECS ready を確認する。command 形式修正で PMU が受理するかは未確認。

## 0.4.33: PMU bootstrap reply 後に FECS Falcon を開始

0.4.32 では PMU が FECS bootstrap を受理し、成功 reply を返しましたが、FECS の ready mailbox は 2000 回の待ちでも変化しませんでした。Nouveau `gf100_gr_init_ctxctl_ext()` は secure bootstrap の後に FECS DMACTL を 0 にして FECS Falcon を開始し、その後 GR ctxctl ready を待ちます。nvgpu `gr_gm20b_load_ctxsw_ucode()` は FECS ctxsw mailbox を clear し、PMU bootstrap 完了後に ready mailbox を確認します。

Step 73 は FECS DMACTL を記録して 0 にし、Nouveau `nvkm_falcon_v1_start()` と同じく CPUCTL bit 6 が立っていれば CPUCTL_ALIAS.STARTCPU、そうでなければ CPUCTL.STARTCPU に書き込みます。今回の実機では CPUCTL 0x50 なので alias が選ばれる想定です。CPUCTL の HALT bit が最大 2000 回の polling で解除されなければ `E000007D` で既存の GPU reset 経路に進みます。直後の FECS CPUCTL/mailbox0 と HALT-clear poll 数を SUMMARY に記録します。BAR0 16 MB、PMC_ENABLE bit 12 の FECS 読み取りガード、既存の bounded wait を維持します。GPCCS と GR context/channel 初期化は行いません。`Install.cmd` のドライバーを 0.4.33 に更新してから `AcrTest.cmd` を一度実行し、Step 73 と `B3cFecsResult`、`B3cFecsStartUsesAlias`、`B3cFecsCpuctlAfterStart`、`B3cFecsMailbox0AfterStart`、`B3cFecsStartPolls`、ready mailbox、fault を確認してください。

## 0.4.34: GM20B FECS ctxsw mailbox と GPCCS 起動

0.4.33 では FECS Falcon 自体は動き始めましたが、ready 判定に使った `0x419A00` は参照先が示す FECS ctxsw mailbox ではありませんでした。nvgpu `hw_gr_gm20b.h` と `gr_gk20a_wait_ctxsw_ready()` の経路では mailbox 0 は `0x409800`、mailbox clear 0 は `0x409840` です。ready 値は ucode INIT 完了の `0x10` として待ちます。旧 `0x419A00` は比較用に読み取りだけ残し、mailbox 0..7 を待ちの後に記録します。

GPCCS は GM20B では ACR の LS 対象ではありません。Nouveau `gf100_gr_init_ctxctl_ext()` / `gf100_gr_init_fw()` と nvgpu `gr_gm20b_load_ctxsw_ucode()` に合わせ、ユーザーが用意した linux-firmware の `gr/gpccs_inst.bin` と `gr/gpccs_data.bin` を GPCCS IMEM/DMEM に PIO し、GPCCS、FECS の順に起動します。Step 74 が PIO load、Step 75 が mailbox clear/set と両 Falcon の起動です。GR context/channel の初期化は対象外です。

`FwTest` / `AcrTest` は GPCCS firmware も `FwGpccsInst` / `FwGpccsData` としてレジストリへ取り込みます。ファームウェアはユーザー提供のものを実行時に読み、リポジトリやキットには含めません。SUMMARY で確認する項目は `B3cGpccsInstLen` / `B3cGpccsDataLen`、GPCCS PIO word 数、start CPUCTL/alias/HALT poll、`B3cFecsReadyMailbox` (期待値 `0x10`)、`B3cFecsCtxswMailboxWords`、旧 mailbox の比較値、PMU/FECS 状態と MMU fault です。欠落 firmware は `E000007E`、サイズ/容量不整合は `E000007F`、GPCCS HALT timeout は `E0000080` で GPU reset 経路に進みます。ビルドと実機結果は未確認です。
## 0.4.36: PMU INIT キューの確認をゆるめる (Claude)

0.4.35 の実機テストで、PMU の INIT を待つところ (Step 59) が 2 回目の読み取りで head/tail 0x51D8/0x51AC の組を見て E0000071 で終わり、FECS の段階まで進まなかった。PMU がキューの位置を設定している途中の値を一瞬読んだと考えられる。0.4.36 では、おかしな組を 1 回見ても失敗にせず、記録して待ち続ける。2000 回待っても正しい組にならないときだけ E0000071 にする。最初に見たおかしな組と回数は `B3cLsPmuInitBadPairs/Head/Tail/Poll` に残る。FECS/GPCCS の処理は 0.4.35 と同じ。

## 0.4.35: FECS handshake と GPCCS 診断

0.4.34 の FECS ready wait は `0x409800` が 0 のまま timeout しました。参照実装を照合し、FECS 起動前に nvgpu `gr_gm20b_load_ctxsw_ucode()` の secure path と同じ順で、clear mailbox 0 に `0xFFFFFFFF`、mailbox 1 に `1`、clear mailbox 6 に `0xFFFFFFFF` を書きます。追加で mailbox 0 自体を 0 に書く操作はしません。Nouveau `gf100_gr_init_ctxctl_ext()` が行う GPCCS DMACTL=0、FECS DMACTL=0、GPCCS 起動、FECS 起動の順も維持します。

ready 条件は nvgpu `gr_gk20a_wait_ctxsw_ready()` が期待する ucode INIT complete の `0x10` 完全一致と、Nouveau が使う mailbox 0 bit 0 の検査を照合しました。Step 75 は `0x10` または bit 0 を成功として扱い、待ち中の最初の非ゼロ値、値の変化回数、最終値を記録します。FECS mailbox 0..7 も待ち後に記録します。別の既存 mailbox `0x419A00` は比較用に読み取りのみです。

GPCCS は引き続きユーザー提供の `gpccs_inst.bin` / `gpccs_data.bin` を IMEM/DMEM offset 0 へ PIO します。Nouveau `gf100_gr_init_fw()` / `nvkm_falcon_v1_load_imem()` に合わせ、IMEM は 256-byte block (64 dword) 単位でタグを進め、末尾をゼロ padding します。Nouveau の直接 PIO 起動経路に BOOTVEC 書き込みはないため変更せず、前後値を記録します。Step 70 は GPCCS CPUCTL/SCTL、Falcon mailbox、PC trace、IRQ、ctxsw mailbox 0..7 を追加記録します。既存の `E0000078` ready timeout を使います。

確認する SUMMARY 行は FECS pre-start mailbox sequence、FECS ctxsw ready value/first-nonzero/change-count/polls、FECS ctxsw mailbox 0..7、GPCCS firmware source/padded IMEM words、BOOTVEC、GPCCS PC/IRQ/ctxsw mailboxes です。ビルドと実機結果は未確認です。
