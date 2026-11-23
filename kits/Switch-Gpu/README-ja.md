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

どれもダブルクリックで動く(0.4.78 から)。UAC の確認が出たら「はい」を押す。右クリックの「管理者として実行」でもよい。最後の `SUMMARY` を写真に撮る。記録は `C:\SwitchWOA-Gpu.txt` に追記される。

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

## 0.4.37: Stage C1 FECS image size の照会

0.4.37 では Stage C の最初の限定手順を追加します。`B3cFecsResult == 0` の場合だけ Step 76 を実行し、nvgpu `gr_gk20a_init_ctx_state()` と同じ順で FECS に3つの method を送ります。golden image size は `0x10`、zcull image size は `0x16`、PM image size は `0x25` です。`gr_gk20a_submit_fecs_method_op()` に合わせ、mailbox ID 0 を `0xFFFFFFFF` で clear し、method data に 0、method push に method address を書いて、mailbox 0 が 0 以外になるまで待ちます。各待ちは最大2000回、1 msごとです。method、mailbox clear前後、返信値、poll数と戻り値を各バイナリ trace に記録します。

指定の `hw_gr_gm20b.h` では FECS method data/push が `0x409500` / `0x409504`、ctxsw mailbox 0 / clear 0 が `0x409800` / `0x409840`、3つの method address が上記の値であることを確認しました。汎用 mailbox PASS/FAIL 値は `1` / `2` です。size query の nvgpu 条件は mailbox 0 が 0 以外になること（NOT_EQUAL、FAIL 条件は SKIP）なので、C1 では2も失敗 sentinel として扱います。`E0000081` は mailbox timeout、`E0000082` は FAIL marker または all-ones read、`E0000083` は C1 前に GR が無効の場合です。context の確保、bind、書き込みは行いません。FECS SCTL の診断アドレスも `0x409240`（Falcon base + `0x240`）へ修正しました。`0x409200` は SCTL ではありません。

SUMMARY では `B3c C1 result`、`B3cC1GoldenQueryWords`、`B3cC1ZcullQueryWords`、`B3cC1PmQueryWords`、3つの image size と `B3cFecsSctl` を確認してください。C1 は未ビルド・実機未確認です。

## Stage C2 の小段階計画

各段階は独立したバージョンで実機確認し、成功・失敗のどちらでも既存の GPU reset 経路で終了します。後段の初期化を先取りせず、段階ごとのトレースで確認して進めます。

| 段階 | 版 / Step | 作業 | 新しい失敗コード |
| --- | --- | --- | --- |
| C2a | 0.4.38 / 77 | `sw_nonctx`、`sw_ctx`、`sw_bundle_init` をレジストリから読み、サイズ・レコード境界・レジスタアドレスを検査して件数と先頭/末尾を記録する。レジスタ書き込みはしない。 | `E0000084` firmware 欠落/128 KiB 超、`E0000085` 形式/範囲不正、`E0000086` GR 無効 |
| C2b | 0.4.39 / 78 | nvgpu `gk20a_init_gr_prepare()` / `gk20a_init_gr_reset_enable_hw()` の順序に合わせて GR reset と nonctx 初期化を FECS ctxsw ucode 初期化より前に配置し、実行前後を記録する。 | `E0000087` reset/idle または nonctx 段階失敗 |
| C2c | 0.4.40 / 79 | FECS ready、C1 size query、C2a 検査後に `sw_ctx` の AIV 項目を適用し、GR idle を上限付きで待つ。 | `E0000088` context list/idle 失敗 |
| C2d | 0.4.41 / 80 | C1 の画像サイズに基づく最小 GR context と必要な backing を上限付きで確保し、仮想アドレス/instance 情報を組み立てて readback する。 | `E0000089` 確保/境界、`E000008A` bind 準備失敗 |
| C2e | 0.4.42 / 81 | FECS context bind の後に bundle init と golden save を参照順で実行し、golden image のサイズと readback を検証する。 | `E000008B` FECS method/idle timeout、`E000008C` golden readback 不一致 |

0.4.38 は C2a、0.4.39 は C2b を実装しました。nvgpu `gr_gk20a_init_ctx_vars()` と `gk20a_init_gr_reset_enable_hw()` では nonctx list は reset-enable 中、ctx list は後続の `gk20a_init_gr_setup_hw()` 中、bundle init は `gr_gk20a_init_golden_ctx_image()` から context bind 後に実行されます。C2c はこのうち `sw_ctx` の適用だけを切り出します。

## 0.4.38: Stage C2a GR init-list の取込みと検査

`FwTest` / `AcrTest` は、ユーザー提供の `gr/sw_ctx.bin`、`gr/sw_nonctx.bin`、`gr/sw_bundle_init.bin` を `FwGrSwCtx`、`FwGrSwNonctx`、`FwGrSwBundleInit` としてレジストリへ取り込みます。ドライバーは C1 が成功し、`PMC_ENABLE` bit 12 が立っている場合だけ Step 77 を行います。各 blob は最大 128 KiB、同時に一つだけ読み込み、検査後に必ず解放します。`sw_nonctx` と `sw_bundle_init` は 8-byte AV (`addr,value`)、`sw_ctx` は 12-byte AIV (`addr,index,value`) の整数レコードとして、空/端数サイズと 16 MiB BAR0 外または非 DWORD 境界のアドレスを検出します。各トレースにはバイト数、レコード長、件数、先頭アドレス/値、末尾アドレスを残します。これは読み取りだけで、GR レジスタには書きません。

不足または128 KiBを超える blob は `E0000084`、不正なサイズ/アドレスは `E0000085`、Step 前に GR が無効なら `E0000086` です。`Gpu.ps1` の SUMMARY では Step 77 と `B3cC2aSwNonctx` / `B3cC2aSwCtx` / `B3cC2aSwBundleInit` の各バイナリ統計を確認します。ファームウェアはキットへ含めません。ビルドと実機結果は未確認です。

sw_bundle_init の先頭の値はレジスターの番地ではなく bundle/method の番号(linux-firmware の gm20b では 0x38〜0x1E100、952 件中 704 件が 4 の倍数でない)なので、4 の倍数確認は sw_nonctx と sw_ctx にだけ適用します。GM20B の `sw_method_init.bin` は配布物に存在せず、nvgpu は method-init 件数がある場合のみ処理します。詳細は 0.4.40 節を参照してください。

## 0.4.39: Stage C2b GR reset と sw_nonctx 適用

Step 78 は FECS bootstrap command より前、PMU の INIT_WPR_REGION 成功後に実行します。PMC_ENABLE bit 12 が立っていることを確認し、GM20B の FECS ctxsw reset (`0x409614`: reset assert `0x70`、release `0x770`) と FE clock force-on/auto 要求を実行します。続いて SCC RAM (`0x40802C`) を初期化し、レジストリの `FwGrSwNonctx` を AV 形式で再生します。レコード全件を先に検査し、DWORD 境界と GR PRI (`0x400000-0x40FFFF`)、GPCS PRI (`0x418000-0x41FFFF`) または 2 GPC PRI (`0x500000-0x50FFFF`) に入るアドレスだけを書きます。リストの書込み数と先頭 8 件の addr/expected/actual readback を記録します。

nvgpu `gk20a_init_gr_reset_enable_hw()` の順序に合わせ、再生後に FECS/GPCCS の IMEM/DMEM scrub と GR idle をそれぞれ最大 2000 回待ちます。いずれかの値が不正、reset request / scrub / idle が時間内に終わらない場合は `E0000087` とし、既存の GPU reset 経路に進みます。bundle/method ID の `sw_bundle_init` は BAR0 レジスタとして書きません。C2b では `sw_ctx`、bundle init、context の確保/bind、golden save は行いません。

参照: MIT nvgpu `gk20a/gr_gk20a.c` の `gk20a_init_gr_prepare()`, `gr_gk20a_ctx_reset()`, `gk20a_init_gr_reset_enable_hw()`, `gr_gk20a_wait_mem_scrubbing()`, `gr_gk20a_wait_idle()` と `include/nvgpu/hw/gm20b/hw_gr_gm20b.h` の `gr_fecs_ctxsw_reset_ctl_*`, `gr_fe_pwr_mode_*`, `gr_scc_init_*`, `gr_fecs_dmactl_*`, `gr_gpccs_dmactl_*` を照合しました。Step 78 は `gk20a_init_gr_prepare()` の PGRAPH/BLG/PERFMON reset と、続く `gk20a_init_gr_reset_enable_hw()` の FECS ctxsw reset → SCC RAM 初期化 → nonctx 再生の順を実装します。MC_ENABLE (`0x200`) の reset mask は PGRAPH/BLG/PERFMON の `0x1000|0x08000000|0x10000000` で、他の bit (PMU を含む) を保持して前後を trace します。GPFIFO access setup と GR 割り込み有効化はこの段階では含めません。

`sw_method_init.bin` は GM20B の配布物にありません。ユーザー確認の upstream linux-firmware `nvidia/gm20b/gr` には9ファイルがあり、`sw_method_init.bin` は含まれません。nvgpu は method-init の件数が非0の場合だけ処理するため、空リストを許容し、本キットでも取り込みや欠落エラーを追加しません。

## 0.4.40: Stage C2c sw_ctx 適用

Step 79 は FECS/GPCCS の起動と FECS ready、C1 の golden/zcull/PM size query、C2a の全リスト検査が成功した後に実行します。これは nvgpu `gk20a_init_gr_setup_hw()` 内で `sw_ctx_load` を走査する位置に対応します。レジストリから `FwGrSwCtx` を最大128 KiBで読み、全454件相当のレコード境界と全アドレスを最初の書き込み前に検証します。許可するアドレスは C2b と同じ GR (`0x400000-0x40FFFF`)、GPCS (`0x418000-0x41FFFF`)、GPC PRI (`0x500000-0x50FFFF`) の範囲です。

nvgpu の setup_hw ループは各 AIV の `addr` に `value` を直接書き、`index` はそのループでは使いません。このため本実装も `index` を記録しつつ `addr/value` を書きます。適用件数と先頭8件の addr/index/expected/actual を記録し、エンジン/GR idle を最大2000回、各1 msで待ちます。PMC_ENABLE bit 12 が無効、リスト異常、idle timeout は `E0000088` で終了し既存の GPU reset 経路に入ります。

この小段階では setup_hw にある他の処理(SLCG/FB MMU debug 設定、floorsweeping、zcull、clock gating、IRQ/exceptions、ZBC/CBC、timeslice、context manager 等)は実装しません。これらは追加の設定値、割り込み状態、または GR context/backing が必要です。bundle init、context allocate/bind、golden save も未実施です。

参照: MIT nvgpu `gk20a/gr_gk20a.c` の `gk20a_init_gr_setup_hw()` と `gr_gk20a_wait_idle()`。本段階では setup_hw の sw_ctx ループの順序と addr/value 書き込みを照合し、実装コードは複製していません。ビルドと実機結果は未確認です。

## 0.4.41: Stage C2d GR context backing と独立 VM

Step 80 は FECS ready、C1 の golden/zcull/PM サイズ照会、C2a のリスト検証、C2b reset/nonctx、C2c sw_ctx 適用がすべて成功した場合だけ実行します。2 MiB の物理連続 GR arena を別に確保し、ACR arena と ACR instance block は変更しません。arena 内に別の GPU page directory/table と RAMIN instance block を作り、C1 のサイズに基づく channel context、golden image、zcull、PM の各 backing と global bundle/pagepool/attribute/private-access-map buffer を配置して GPU VA に写します。

GM20B nvgpu gr_gm20b_calc_global_ctx_buffer_size() の式と hw_gr_gm20b.h の値から、1 GPC・2 TPC の attribute buffer は 0x48000 bytes、bundle buffer は 0x1800 bytes、pagepool は 0x8000 bytes、private access map は 0x80000 bytes です。W4T の MIT t210_gr_context.h の CTX/GOLD/BUNDLE/PAGEPOOL/ATTRIB/ACCESS_MAP offsets を配置の照合に使います。各 backing の先頭/末尾、GPU PTE、outer SMMU PTE/PDE、instance PDB/limit/WFI pointer を記録します。SMMU mapping は別の outer window なら GR arena 内の専用 table と ACR outer directory の追加 PDE を使い、同じ window なら ACR の既存 table に GR pages のみを追加します。GPU reset 後に変更した outer entries を戻して arena を解放します。

この段階では FECS bind_pointer、bundle init、golden image save は送りません。0x418810 の attribute-base register と期待 field は patch 用の診断値として記録するだけです。E0000089 は PMC_ENABLE/サイズ/arena 条件、E000008A は SMMU/GPU page table/instance/backing の readback 不一致です。失敗時も既存の GPU reset cleanup を通します。 Step 80 を試した実行では KeepOn=1 でも GPU reset と arena cleanup を行います。実機で B3cC2dResult が 0 になり、arena/PDE/PTE/PDB/WFI と backing samples が期待値に一致することを確認してください。

## 0.4.42: Stage C2e golden context image

Step 81 は C2d が成功し、PMC_ENABLE bit 12 が有効なときだけ実行します。FECS bind_pointer method 3 を data=`instance block PA >> 12 | 0xB0000000` で送り、mailbox 0 の clear/pass/fail は `0x30/0x10/0x20` です。GR global backing の pagepool/bundle/attribute 設定と timeslice を反映し、`FwGrSwBundleInit` は bundle data/address port に流します。bundle file の値は method ID なので BAR0 register address として書きません。続いて `0x100` byte FECS header を golden buffer に複製し、zcull mode/pointer を設定、instance block の WFI pointer を golden VA に向け、WFI_GOLDEN_SAVE method 9 を送ります。mailbox clear/pass/fail は `3/1/2` です。成功時は header magic `0x600DC0DE`、header sample、FECS/GPCCS、FE HWW ESR、PFIFO fault id、MC error を記録し、instance pointer を channel context に戻して FECS current context を無効化します。終了時は既存 reset と C2d outer-map/arena cleanup を通します。

実装で参照した nvgpu の関数は `gr_gk20a_fecs_ctx_bind_channel()`、`gr_gk20a_submit_fecs_method_op()`、`gk20a_init_sw_bundle()`、`gr_gk20a_fecs_ctx_image_save()`、`gr_gk20a_init_golden_ctx_image()` です。register/method 値は `hw_gr_gm20b.h` と header layout/magic は `hw_ctxsw_prog_gm20b.h` で確認しました。W4T の `tg_gpu_gr_context_global_patches()` と `tg_gpu_gr_context_init()` は global backing の register grouping の MIT cross-check です。E000008B は method/idle timeout、E000008C は method failure、bundle validation、または header magic mismatch です。実機では bind/save mailbox、bundle entry count、golden magic/samples、FE HWW ESR、PFIFO fault id を確認してください。

この実装は段階的な試行用です。`gr_gk20a_init_golden_ctx_image()` の FE power/reset handshakes、`init_preemption_state()`、GM20B `init_fs_state()` の floorsweep 全体、global zcull/context-buffer patch の全項目はまだ含めていません。したがって mailbox/save が通っても nvgpu と同等の完成した golden context とは見なさず、これらの初期化を含める次段階が必要です。

C2c の過去の sample では sw_ctx が 0 を書いた 0x404004 の readback が 0x70000 でした。GM20B header にこの番地の register name はなく、nvgpu setup_hw の list loop も readback を検査しません。この値だけでは read-only/sticky bit と異常状態のどちらかは断定できません。

(Claude 追記、0.4.42) FECS の bind/save に渡す instance block の番地を、物理番地から C2d の PDB と同じ IOMMU タグ付き IOVA に直した(ACR の PMU bind 0x704FDC68 と同じ扱い。タグなしは B2a の PRIV_VIOLATION の経路)。また bind の前に、ACR と同じ AcrMmuInvalidatePdb() で新しい GR の PDB を invalidate する(結果は B3cC2eGrPdbInvalidate)。

## 0.4.43: Stage C2e wait diagnostics and GR setup

Step 81 now records the last C2e wait stage, bundle index, poll count and the GR, engine and FIFO status values on every exit. The bounded waits use the nvgpu predicates: `gr_gk20a_wait_idle()` treats an invalid FIFO context as idle, or requires both GR not busy and no context switch active; `gr_gk20a_wait_fe_idle()` waits until FE method upper/lower and FE-GI bits are all zero. Stage IDs 10/11 are FE force-on/auto handshakes, 20 FECS bind mailbox, 30/35/40/60 GR idle, 50 FE method idle, and 70 golden-save mailbox. All time limits remain bounded.

Before the golden sequence, Step 82 performs the FE power force-on/auto requests, FECS context-switch reset pulse and SCC clear. Step 83 applies a bounded, fixed 1-GPC/2-TPC GR/zcull setup subset through validated GR PRI ranges, then captures readback samples. These are diagnostic bring-up stages. Step 83 includes the fixed one-GPC/two-TPC mapping tables, SM numbering, skip table, ZROP/CROP active-LTC count, and zcull inputs; per-fuse topology discovery and broader setup_hw state remain outside this bounded step. GM20B has no `init_preemption_state` hook on this path; the driver records that no preemption hook ran. `E000008D` identifies FE power/reset or PDB-invalidate failure; `E000008E` identifies the fixed-topology write validation/readback failure.

Every C2e exit now captures FE HWW ESR/info, GR/engine/FIFO status, FE go-idle/power registers, FECS and GPCCS state/PC traces, PFIFO fault records and MC status while GR remains enabled. SUMMARY prints the last wait checkpoint and the post-run diagnostics. This remains unbuilt and requires hardware review; it does not establish successful golden-image creation.
## 0.4.44: C2e GR interrupt and HWW exception setup

Step 84 follows the exception/interrupt portion of nvgpu `gk20a_init_gr_setup_hw()`: clear and enable GR interrupts; enable FECS host error interrupts; reset/enable FE, MEMFMT, SCC, MME, PD, SKED and DS HWW ESRs; program the GM20B HWW report masks; enable TPC/GPC exception forwarding; then clear and enable GR exception registers. All writes are prevalidated against GR/GPC PRI ranges. MC and CPU interrupt output routing is not enabled; the test continues to poll registers.

The driver captures FE HWW ESR at `0x404000`, the header-defined info register `0x4041B0`, and the separately requested raw `0x404004` value, along with GR interrupt/exception registers, GR status 1/2 and FECS host-interrupt status/clear/enable before setup, before the first bundle entry, and on every C2e exit. If FE HWW ESR status bits are nonzero before setup or immediately before the first bundle entry, C2e stops with `E000008F` and enters the existing GPU reset path. The nvgpu FE handler only reads/clears the ESR and requests recovery; its code and the GM20B register header do not define a decoding layout for the reported `0x7FFD6` info value, so it is kept raw.

This adds the interrupt/exception subsection only. Other `gk20a_init_gr_setup_hw()` items not added here include GPC MMU init, private-ring timeout setup, FIFO access enable, ECC scrub, ZBC/CBC init, read-coalesce control and clock-gating setup. The fixed topology/zcull setup is isolated in Step 83; context list replay is already in C2c. No MC/CPU interrupt output is enabled.

## 0.4.45: C2e nvgpu idle predicates

The C2e waits now match `gr_gk20a_wait_fe_idle()` and `gr_gk20a_wait_idle()` in the pinned nvgpu `gk20a/gr_gk20a.c`. FE idle tests only `gr_status_fe_method_lower_v(val) == 0U`; it does not wait for FE-method-upper or FE-GI to clear. GR idle is true when FIFO context status is INVALID, or when GR engine busy is false and FIFO context-switch-in-progress is false. The current Switch has one GR engine at FIFO slot 0. In the bundle replay, nvgpu calls GR `wait_idle()` for the `GR_GO_IDLE_BUNDLE` entry and calls `wait_fe_idle()` after each entry. Every wait remains bounded to 2000 polls.

The wait trace retains the raw GR, GR-engine and FIFO-engine status and now adds the selected idle branch, FIFO context-status field, ctxsw-active and GR-busy values. Idle branch is 1 for invalid FIFO context, 2 for the no-busy/no-ctxsw predicate, and 0 for timeout (FE wait uses 1 when the FE lower-method field is idle). This is a predicate correction; no new hardware validation has been performed.

## 0.4.46: C2e golden-save order and failure diagnostics

失敗時に `Gpu.ps1` の SUMMARY が `n/a` だった原因は、診断関数が `B3cC2ePost...` の名前で記録する一方、SUMMARY は `B3cC2eFecsCpuctl` など `Post` なしの名前を参照していたためです。診断はその両方の名前で記録し、全ての C2e 終了経路から、PMC_ENABLE bit 12 が有効な間に読みます。失敗時も FECS/GPCCS の状態、FECS host interrupt、ctxsw mailbox 0..7、PFIFO fault ID と有効な per-unit fault records、MC 状態、golden buffer の先頭16ワードを確認できます。`B3cC2eGoldenRawOnExit` は保存が完了しない場合も含むバイナリ記録です。

Step 85 は `gk20a_init_sw_bundle()` の終了順に合わせます。pipe override を解除し、FE go-idle timeout が無効な間に GR idle を待ち、timeout を production 値へ戻します。GM20B の `sw_method_init` は0件として記録し、`gr_gk20a_init_golden_ctx_image()` の次の GR idle wait を実行します。その後、nvgpu `gk20a_mm_l2_flush(g, true)` と同じ dirty L2 flush → L2 invalidate を `0x70010` / `0x70004` で実行し、各々最大2000 pollで完了を待ってから channel context の0x100-byte FECS headerを golden buffer にコピーします。timeout は `E0000090` です。

保存 method は nvgpu `gr_gk20a_fecs_ctx_image_save()` と同じ clear=`3`、成功条件 mailbox0 bit0、失敗条件 bit1 を維持しました。bind の mailbox bit4 が残っても、wait は bit0/bit1 だけを見るため、clear を `0xFFFFFFFF` に変更していません。確認した `gr_gk20a_init_golden_ctx_image()` は header copy 前の L2 flush/invalidate を行いますが、golden save の後に追加の L2/MM flush は行わず、続けて golden buffer を読みます。このため post-save flush は追加せず、timeout時は reset 前の golden buffer と FECS 状態を記録します。save 実行中の WFI pointer と golden VA も記録します。

参照は MIT nvgpu `gk20a/gr_gk20a.c` の `gk20a_init_sw_bundle()`、`gr_gk20a_init_golden_ctx_image()`、`gr_gk20a_fecs_ctx_image_save()` と `gk20a/mm_gk20a.c` の `gk20a_mm_l2_flush()`、および GM20B `hw_flush_gm20b.h` / `hw_ctxsw_prog_gm20b.h` です。ビルドと実機確認は未実施です。

## 0.4.47: C2e golden save の停止診断

C2e は nvgpu `gr_gk20a_wait_idle()` の判定を変えず、FIFO context status が INVALID なら idle、それ以外は GR busy と ctxsw active の両方が解除された場合に idle とします。各 bounded wait の終了時に、GR status、GR engine status、FIFO engine status、context/ctxsw/busy の各フィールド、stage、bundle index、poll 数、選択された分岐を `B3cC2eWaitHistory` に記録します。各レコードは32ビット値10個で、最大2048件です。実際の件数と未記録件数も記録します。

FECS save method 9 の直前と C2e の終了時に、`gr_status`、`gr_status_1`、`gr_status_2`、GR engine/FIFO status、GR activity 0、1、2、4 を読みます。GM20B の `hw_gr_gm20b.h` では activity 0/1/2/4 は `0x400380`、`0x400384`、`0x400388`、`0x400390` です。同ヘッダーに activity 3 の accessor はありません。status bit 21 は FE-GI と定義されていますが、bit 5 と 7 に名前付き decoder はないため、raw bit のまま記録します。wait 条件やハードウェア書き込みは変更していません。診断のみの変更で、未ビルド・未実機確認です。

FECS ctxsw mailbox 2=`0x41009`、mailbox 3=`0x20`、GPCCS mailbox 4=`0x10137` は raw 値で記録します。確認した nvgpu の wait/dump 処理と GM20B register header は、これらの mailbox slot/value の意味を定義していないため、意味を推定しません。

## 0.4.48: C2e bundle idle diagnostics and post-bind sw_ctx order

0.4.47 のログでは、960 wait のうち958回で FIFO ctx_status INVALID の分岐が idle と判定しました。その全てで GR engine busy bit 0 は立っており、最初の bundle entry 以降は `gr_status` と `gr_activity_1` が動作中の値を示したまま、GO_IDLE後も save 直前も engine busy が解除されませんでした。PFIFO/MMU fault と FE HWW exception はありませんでした。これは bundle replay の後も GR engine が停止しないことを示します。

nvgpu 1ae0167d の `gr_gk20a_init_golden_ctx_image()` は FECS channel bind と wait の後に `sw_ctx_load` を書き、その後に global buffers/timeslice/floorsweep と bundle init を行います。Step 86 で同じリストを bind 後にも適用して、従来の C2c pre-bind 適用と比較できるようにしました。`gk20a_init_sw_bundle()` は各 bundle entry の前に FE idle を待ち、`GR_GO_IDLE_BUNDLE` の後に GR idle を待つため、entry 前の待ち順もこの順に合わせています。

既存の nvgpu idle predicate は維持します。別に Step 87 で GO_IDLE の後と golden save 直前に、GR engine status bit 0 が 0 になるまで各最大2000回、1ms間隔で待ちます。GO_IDLE後の timeout は記録して継続し、save直前も busy なら `E0000091` で保存を止めて既存のGPU resetへ進みます。両 checkpoint の GR status、engine/FIFO status、poll 数を記録します。

GM20B `hw_gr_gm20b.h` は `gr_status` の FE-GI bit 21 と FE method bits 1/2 を定義しますが、観測された bit 0/5/7/11/14/24 の名称や `gr_activity_1` の内部 unit/state field は同ヘッダーに定義されていません。activity_1 は `0x400384` の raw 値として記録し、`0x200` を特定 unit/state と断定しません。nvgpu `gk20a_gr_debug_dump()` の調査でも当該 bit/field の decoder は確認できませんでした。

参照: MIT Switchroot nvgpu `gk20a/gr_gk20a.c` の `gr_gk20a_wait_idle()`、`gr_gk20a_wait_fe_idle()`、`gr_gk20a_init_golden_ctx_image()`、`gk20a_init_sw_bundle()`。参照元に沿った順序と register names の比較で、ソースコードはコピーしていません。
補足: sw_ctx の再適用は `gr_gk20a_init_golden_ctx_image()` の FECS channel bind 後、global buffer commit と bundle replay の前です。`gk20a_init_sw_bundle()` は各 bundle entry の前に FE idle を待ち、GO_IDLE entry の後に GR idle を待ちます。実装はこの順序に合わせました。

診断の解釈: `0x400700` の観測値 `0x01200821 / 0x00204001 / 0x00200021 / 0x002000A1` は、それぞれ bit 24,21,11,5,0 / bit 21,14,0 / bit 21,5,0 / bit 21,7,5,0 です。GM20B header で名前を確認できたのは bit 21 の FE-GI で、FE method upper/lower は bit 1/2 です。残りの set bit と `gr_activity_1=0x200` の unit/state 解釈は、照合した header/debug path から確定できません。FIFO context status が INVALID でも engine busy bit 0 が立つ状態を別の bounded check で捕捉します。

この試験は host FIFO runlist/channel をロードしていません。nvgpu の golden path は FECS channel bind 後に進みますが、現状の FECS bind だけで FIFO engine の実 context が VALID になるかは未確認です。Step 87 の二つの raw status を次の実機結果で確認してください。
## 0.4.49: Nouveau GM20B golden generation order

0.4.48 の E0000091 では、GO_IDLE 後も GR status 0x00200021、GR engine busy=1、FIFO context status=INVALID のままでした。FECS save は送っていません。以前のログにあった golden magic は bind 時点で既に書かれていたため、save 完了の証拠として扱いません。

Nouveau v6.12 の gm20b_grctx_generate_main() は sw_ctx と idle wait の後に FE_GO_IDLE timeout を保存して一時無効化し、attrib/floorsweep と topology 設定を行います。そこから idle を待って timeout を元の値へ戻し、再度 idle を待って method-init、idle wait、bundle replay の順です。gf100_gr_icmd() は pipe override を有効にして各 method を投稿し、GO_IDLE の後に GR idle、各 entry 後に FE method-lower idle を待ち、最後に override を解除します。今回 timeout は bundle replay の前に保存値へ戻し、FE idle wait を method 投稿後に置きました。

sw_ctx の後の idle wait は FE timeout を変更する前に置きました。Step 84 の例外/割り込み setup_hw 部分も FECS bind より前に置き、FECS bind から sw_ctx、timeout/floorsweep、method-init、bundle replay までの Nouveau の並びに割り込まないようにしました。

GM20B 関数にある 0x4064D0..0x4064EC のゼロ設定、0x405B00 の topology、0x408908 の read/OR/write、0x4041C4 の TPC mask は既存 Step 83 にあり、設定後の idle wait を追加しました。追加チェックは gf100_gr_wait_idle() と同じく GR が MC で無効、または GR busy と ctxsw active の両方が 0 のときだけ成功とします。FIFO context INVALID は成功条件にしません。Step 88 の各 wait は最大2000回です。GO_IDLE 後の timeout は記録して bundle replay を続け、save 直前も idle でなければ E0000091 で save を送りません。セットアップ境界で idle にならない場合は E0000092 で中断します。

ctxgf100.c の WFI firmware path は FECS save 後に 0x409B00 の current-context valid bit を消します。legacy unload の 0x409B04 / 0x409000 経路は non-firmware path のため適用しません。0x5044B0 bit 27 は照合した GM20B/Nouveau context-generation 関数で確認できなかったため書きません。GM20B 専用 main override は nvkm_mc_unk260() を呼ばないため、0x260 は前後を読むだけにしています。GR 外への書き込みは追加していません。

参照: Linux Nouveau v6.12 MIT ctxgm20b.c:gm20b_grctx_generate_main()、ctxgf100.c:gf100_grctx_generate()、gf100.c:gf100_gr_wait_idle() / gf100_gr_icmd() / gf100_gr_mthd()。FECS firmware save の条件は ctxgf100.c:gf100_grctx_generate() の firmware 分岐と照合しました。参照ソースのコードはコピーしていません。

## 0.4.50: GPC/TPC PRI と GM20B GPC MMU の切り分け

0.4.49 の実機では Step 90 の前に bundle method `0x1000` で GR が busy になり、`0x5044B0` は `0xBADF5040` を返しました。これは TPC 側の読み出しが応答していない兆候ですが、それだけで clock gating、priv ring、floorsweep のどれが原因かは確定できません。FE HWW ESR `0x80000001` の bit 0 と info `0x73258` は、照合した `hw_gr_gm20b.h` と Nouveau/nvgpu の FE handler にフィールド定義が見当たらないため raw 値で記録します。

C2b の Step 78 開始時と、Step 90 の最初の bundle method 直前に、GPC0/TPC0・TPC1・broadcast の activity/status、SM/PE 設定、FECS floorsweep、TOP の GPC/TPC 数、fuse の GPC/TPC opt 値、PRI ringmaster と ringstation の状態・priv error address/data/info/code を読みます。レジスタ offset/value の組を `B3cC2bPreResetGpcPri` と `B3cC2ePreBundleGpcPri` に記録し、BADFxxxx の件数を別記録します。診断の読み出しは GPU 状態を変更しません。

新しく有効化する確定手順は Step 89 の GM20B `gm20b_gr_init_gpc_mmu()` です。Nouveau `gk20a_gr_init()` の順序は、sw_nonctx、Falcon scrub、GR idle の後に GPC MMU、続いて zcull/ROP/FIFO/interrupt setup です。既存 C2b が nonctx・scrub・idle を終えた直後に、この GPC MMU 部分を行います。`0x100C80 & 0xF000187F` を `0x418880` に設定し、`0x418890/0x418894` を 0、`0x100CC4/CC8/CCC` と `0x100800` をそれぞれ `0x4188B0/B4/B8/AC` に反映します。書き込み先は GR/GPC PRI のみで、expected/readback を記録します。secure ACR boot 中のため Nouveau の non-secure 専用 bypass `0x100CE4=0xFFFFFFFF` は実施しません。readback 不一致は `E0000093` です。

Priv ring は既に ACR 前の Step 53 で初期化しており、今回は再列挙しません。nvgpu `gk20a_init_gr_setup_hw()` は SLCG GR/PERF と、その後の BLGC/PG/ELCG prod 設定を呼びます。今回確認できた範囲では、それらの GM20B 製品値テーブルと有効化条件を特定できなかったため追加していません。したがって clock/power gating は未完了の確認項目です。0x260 と `0x5044B0` は読み取り専用です。実機では二つの `GpcPri` バイナリ記録における BADF 値、Step 89 の `B3cC2bGpcMmuReadback`、Step 90 の FE HWW ESR/INFO と最初の bundle index を確認してください。

## 0.4.51: bundle replay の最初の FE/GR 状態変化を特定

0.4.50 の実機では `sw_nonctx` entry 79 の `0x419DF4 <- 0` が GPC priv-ring error (`adr=0x00419DF4`, `wrdat=0`, `info=0x1C40820C`, `code=0xBADF510C`) として記録されました。照合した nvgpu `gk20a_init_gr_reset_enable_hw()` は `sw_non_ctx_load` の各 address/value を通常の GR MMIO write で順に適用します。個別 entry の priv-level 判定や再試行は同関数に見当たらず、拒否時の設定変更は追加していません。

Step 90 では bundle の各 method/data 投稿直後に FE ESR `0x404000`、FE HWW info `0x4041B0`、GR intr `0x400100`、GR exception `0x400108`、GR status `0x400700` を読みます。baseline から FE ESR/info、GR intr/exception のいずれかが最初に変化した entry で `E0000094` として replay を止めます。GR status は記録しますが、entry 0 から busy になるため停止条件には使いません。`B3cC2eBundleBaseline` と `B3cC2eBundleEntry0State` は 5/8 DWORD、`B3cC2eBundleFirstChangeState` は index/method/data/ESR/info/intr/exception/GR status の 8 DWORD です。変更がなければ index は `0xFFFFFFFF`、found は 0 です。全 exit path でこれらの trace を出し、既存の reset/cleanup へ進みます。

`hw_pri_ringstation_gpc_gm20b.h` は priv-error ADR/WRDAT/INFO/CODE の register address を定義しますが、`0x1C40820C` や `0xBADF510C` の subfield decoder はありません。確認した nvgpu FE exception handler と GM20B register header も `0x53242`、`0x73258`、`0x7FFD6`、`0x5FFD6` の FE HWW info field layout を定義していないため raw 値のまま扱います。priv-ring error の clear/ack 方法も参照コードで同じ GM20B register に対する安全な clear write を確認できなかったので、記録後の書き込みはしていません。

## 0.4.52: GR PRI write ごとの priv-ring error 記録

Step 78 の `sw_nonctx`、Step 79/86 の `sw_ctx`、Step 89 の GPC MMU、C2e の exception setup・global/timeslice・floorsweep writes の直後に、ringmaster status0/1 と SYS/GPC priv-error ADR/WRDAT/INFO/CODE を読みます。ringmaster interrupt status が非0なら、直前の write address/value と station の生値を最大64件記録し、`ACK_INTERRUPT` を発行して command が `NO_CMD` に戻るまで最大2000 poll 待ちます。未完了は `E0000095` で中断し、GPU reset に進みます。各記録は16 DWORDで、write address/value、status0/1、SYS 4語、GPC 4語、ACK poll数、command readback、ACK実施/timeoutを含みます。64件超は dropped count に記録します。

nvgpu `gm20b_priv_ring_isr()` の診断順と ACK 手順、および `hw_pri_ringmaster_gm20b.h` の `ACK_INTERRUPT=2` / `NO_CMD=0` を参照しました。`hw_pri_ringstation_gpc_gm20b.h` と `hw_pri_ringstation_sys_gm20b.h` は各 station のエラー register address を定義しますが、照合した版に priv-error INFO/CODE の field decoder はなく、数値は raw のまま表示します。確認した `hw_gr_gm20b.h` に FECS/GPCCS の priv-level mask 設定は見つかりませんでした。`gk20a_init_gr_reset_enable_hw()` は FECS 起動より前に `sw_non_ctx_load` を直接適用します。今回、priv-level mask の推測書き込みは加えていません。

## 0.4.53: GM20B GR hang status snapshots

Step 91 adds read-only snapshots from nvgpu `gr_gm20b_dump_gr_status_regs()` at three points: immediately before the first bundle method, immediately after entry 0, and at the stop/exit point. Each snapshot contains the exact name and offset list from that function, including both TPC1 activity registers for this 1-GPC/2-TPC GPU. The binary records are `B3cC2eGrStatusDumpBeforeBundle`, `B3cC2eGrStatusDumpAfterEntry0`, and `B3cC2eGrStatusDumpAtStop`; the log prints every register as `name=value`. The on-screen SUMMARY shows only before/after-entry-0 differences. Activity values remain raw because the referenced GM20B headers and dump helper do not define per-unit 3-bit state names. This is a diagnostics-only change; it adds no register writes or waits. Reference: MIT nvgpu `gm20b/gr_gm20b.c`, `gr_gm20b_dump_gr_status_regs()`, with offsets cross-checked against `hw_gr_gm20b.h` and `hw_fifo_gm20b.h`.

## 0.4.54: GM20B LTC/FB/ZBC/CBC and BE setup

Steps 92-97 extend C2e with the newly available nvgpu function bodies. Step 92 follows `gm20b_ltc_init_fs_state()`: it reads the full `TOP_NUM_LTCS` maximum, extracts the active count from `pri_ringmaster_enum_ltc_count_v()`, records slice/cache-line/comptag parameters, writes active LTC counts to `0x17E27C` and `0x17E000`, copies the LTS0 DSTG config to `0x17E318` with VDC 4-to-2 disabled, and clears the three LTC interrupt-enable bits in `0x17E20C`. Step 93 follows `fb_gm20b_init_fs_state()` and writes the active LTC count to `0x100800`. The physical-secure bypass at `0x100CE4` is omitted: nvgpu only writes it when secure privilege is disabled, while this path has completed ACR secure boot.

Step 94 loads the three default ZBC color entries and two FP32 depth entries in the `gr_gk20a_load_zbc_default_table()` order, using the LTC DSTG index/value ports and GR DS table ports. Step 95 follows the zero-size comptag path: nvgpu `gm20b_ltc_init_comptags()` returns without allocating when max comptag lines are zero, `gm20b_ltc_init_cbc()` writes a zero CBC base for a zero-sized store, and `gm20b_ltc_cbc_ctrl()` returns when the backing store has size zero. This probe therefore chooses zero comptag backing; it does not claim that every compressed rendering workload can run without comptags. Step 96 clears the LG/SU read-coalesce bits as `gm20a_gr_disable_rd_coalesce()` does before `sw_ctx_load`. Step 97 applies the BE active-LTC ZROP/CROP settings and CROP debug3 bit from `gr_gm20b_init_fs_state()` after the remaining GR floorsweep/timeslice setup.

New write locations outside GR PRI are: LTC `0x17E27C`, `0x17E000`, `0x17E318`, `0x17E20C`, ZBC ports `0x17E338`, `0x17E33C..0x17E348`, `0x17E34C`, and CBC base `0x17E278`, all from `common_ltc_ltc_gm20b.c` (`gm20b_ltc_init_fs_state`, `gm20b_ltc_init_cbc`, `gm20b_ltc_set_zbc_color_entry`, `gm20b_ltc_set_zbc_depth_entry`); FB `0x100800` from `common_fb_fb_gm20b.c` (`fb_gm20b_init_fs_state`). ZBC's GR DS registers, read-coalesce `0x419A3C`, and BE ZROP/CROP registers remain GR PRI writes. The conditional `fb_priv_mmu_phy_secure` write, FB `init_cbc` callback (not assigned for GM20B), PMU ZBC-save synchronization (ELPG path is not active here), `gm20b_ltc_set_enabled()`, `gm20b_flush_ltc()`, and `gm20b_determine_L2_size_bytes()` are omitted; ordinary compression remains unverified for this no-comptag probe. ECC scrub and unrelated SLCG/BLCG/ELCG setup are also omitted. The existing three GR status dumps and reset/cleanup flow remain in place; each new step has a bounded wait where applicable and readback traces. CROP_STATUS1 and FE_GO_IDLE_INFO remain raw because the checked headers do not provide decoders for those observed fields.

References: MIT Switchroot nvgpu 1ae0167d `common/ltc/ltc_gm20b.c` (`gm20b_ltc_init_fs_state`, `gm20b_ltc_init_comptags`, `gm20b_ltc_init_cbc`, `gm20b_ltc_cbc_ctrl`, `gm20b_ltc_set_zbc_color_entry`, `gm20b_ltc_set_zbc_depth_entry`), `gk20a/gr_gk20a.c` (`gk20a_init_gr_setup_hw`, `gr_gk20a_load_zbc_default_table`, `gr_gk20a_add_zbc`), `gm20b/gr_gm20b.c` (`gr_gm20b_init_fs_state`), and `common/fb/fb_gm20b.c` (`fb_gm20b_init_fs_state`). Addresses were checked against MIT `hw_ltc_gm20b.h`, `hw_gr_gm20b.h`, `hw_top_gm20b.h`, `hw_pri_ringmaster_gm20b.h`, and `hw_fb_gm20b.h`.

## 0.4.55: GR setup_hw の PE/zCull/FIFO/ROP 前処理

0.4.55 は実機 0.4.54 で確認した `GRFIFO_CONTROL (0x400500)=0` を受け、C2e の FECS bind 前に不足していた setup_hw 項目を追加します。Step 101 は PE stream-master と既存の固定 1-GPC/2-TPC zCull 設定を早め、Step 102 は Nouveau の active-FBP ROP 設定を、Step 103 は FIFO access と semaphore access を `0x00010001` にします。各 GR write は範囲検証、priv-ring ACK、readback を行い、失敗は既存の GPU reset/cleanup に進みます。新しいエラーは `E000009C` (PE/zCull)、`E000009D` (FBP ROP)、`E000009E` (FIFO access) です。

参照順序は nvgpu `gk20a_init_gr_setup_hw()` の PE master → `gr_gk20a_zcull_init_hw()` → FIFO access → interrupts です。Nouveau `gk20a_gr_init()` は `gf117_gr_init_zcull()`、`gk104_gr_init_rop_active_fbps()` を用い、FIFO access も有効化します。GM20B nvgpu の `gr_gm20b_init_fs_state()` は ZROP/CROP に active LTC count を使います。一方 Nouveau の ROP helper は `0x120074` の active FBP count を ZROP/CROP の low nibble に反映します。SUMMARY には両方のカウント、書き込み前後の値と FIFO control の前/期待/実測値を出します。

Step 83 にあった PE stream-master と zCull の 6 write は Step 101 へ移したため、bundle 直前に遅れて上書きしません。既存の fixed topology floorsweep/SM setup は Step 83 に残します。今回の zCull は既存の fixed mapping を使う限定実装で、nvgpu がソフトウェアで構成する一般 topology の tile/bank map 生成は追加していません。ECC scrub、clock-gating prod、private-ring timeout、full FIFO channel/runlist setup も対象外です。新しい hardware result では Step 101/102/103 の readback と、bundle entry 0 後の GR dumps (特に FIFO control、CROP status/activity、FE exception) を確認してください。

## 0.4.56 (Claude): 0.4.55 の止まり方の修正

0.4.55 の実機では、Step 101 の zcull の設定で、書いた値と読み返しが違うレジスターがあり(0x500910 は bit30 が足される、0x500918 と放送用の 0x41BFD4 は 0 と読める)、E000009C で止まった。priv ring の拒否はなく、同じ値は以前の Step 83 でも止めずに書いていたので、読み返しの違いは記録だけにした。また 0x120074(FBP の数)が 0 と読めたので、0 のときは nouveau 流の ROP 設定を飛ばし、Step 92 の nvgpu 流(LTC の数 2)に任せる(B3cC2eNouveauRopSkippedZeroFbp)。

## Stage D: channel command path

各段階は一回だけ実行し、成功/失敗のどちらでも既存の GPU reset と一時領域の cleanup へ進みます。各 wait は上限を設けます。C2e の golden context までの処理は変更しません。

| Version | Sub-step | Steps / errors | Scope |
| --- | --- | --- | --- |
| 0.4.57 | D1 | Step 104 / `E000009F` | PFIFO/PBDMA topology と状態を記録し、確認できた最小の PFIFO/PBDMA 初期化を実施。runlist/USERD はまだ設定しない。 |
| 0.4.58 | D2 | Step 105 / `E00000A0` | D1 成功後に C2d arena の RAMIN/RAMFC、USERD、空の GPFIFO ring、pushbuffer を設定し、channel 0 を GR runlist に登録して bounded wait 後に unbind/cleanup する。GPFIFO entry は送らない。 |
| 0.4.59 | D2 再試行 | Step 105 / `E00000A0` | 0.4.58 は channel instance の bind で PFIFO BIND_ERROR(target VID)。target を stage C の FECS bind と同じ sys_mem_ncoh(3)に変更。 |
| 0.4.60 | D2 再試行 2 | Step 105 / `E00000A0` | 0.4.59 は inst の読み返し OK でも BIND_ERROR。USERD の target も sys_mem_ncoh(3)にし、bind 前後の fifo_intr_0 と 0x252C(bind error 情報)を記録。古い bit0 は bind 前に消す。 |
| 0.4.61 | D2 再試行 3 | Step 105 / `E00000A0` | 0.4.60 の bind error 情報 0x2 = SNOOP_WITHOUT_BAR1。channel bind の前に BAR1(0x1704)を GR instance block に結び、0x1710 を bounded wait、終了時に元へ戻す。 |
| 0.4.62 | D2 再試行 4 | Step 105 / `E00000A0` | 0.4.61 は BAR1 を結んでも bind error 0x2。1 回の実行で BAR1 target {3, 0} x inst/USERD target {0, 2, 3} の 6 通りを bind/unbind して intr0 と 0x252C を記録し、エラーのない最初の組で続ける。 |
| 0.4.63 | D2 再試行 5 | Step 105 / `E00000A0` | 0.4.62 は 6 通りすべて bind error 0x2(target は無関係)。nvgpu の FIFO 初期化と同じく fifo_bar1_base(0x2254)= USERD の GPU VA >> 12 | valid を bind 前に設定し、終了時に戻す。6 通りの試しも残す。 |
| 0.4.64 | D2 後片付け | Step 105 / `E00000A0` | 0.4.63 で bind / runlist / channel ON_PBDMA まで成功。PBDMA0 intr_0 bit 24 が残り preempt が時間切れ。PBDMA0 の状態を記録し、preempt の前に PBDMA の割り込みを消す。 |
| 0.4.65 | D2 PBDMA 初期化 | Step 105 / `E00000A0` | 0.4.64 で PBDMA0 intr_0 bit 24 は write-1 でも消えず preempt 時間切れ。nouveau gk104_runq_init と同じ PBDMA0 初期化(0x4013C、0x40108、0x40148、0x4010C、0x4014C)を bind 前に行い、bit 24 が立つ時点を記録。 |
| 0.4.66 | D2 ENGINE 割り込み | Step 105 / `E00000A0` | 0.4.65: PBDMA 初期化後も bit 24(nvgpu の名前で [ENGINE])が runlist の直後に立つ。GR / FECS / FIFO の状態を記録し、後片付けでは nouveau と同じく method0 = 0x80600008 にしてから割り込みを消す。 |
| 0.4.67 | D2 RAMFC 修正 | Step 105 / `E00000A0` | RAMFC は nvgpu 値と一致し channel は ON_PBDMA、runlist submit も完了したが、PBDMA0 intr_0 bit 24 が発生。W1C で消えず cleanup preempt は 2000 poll timeout。 |
| 0.4.69 | D3 GP_PUT 試行 | Step 106 / `E00000A1` | GPFIFO entry と pushbuffer は正しく配置されたが、CPU が USERD backing page に GP_PUT=1 を書いても USERD/PBDMA GP_GET は 0、semaphore も 0 のまま。PBDMA bit24 はマスク状態で残り、FIFO PBDMA status は 0x2000。BAR1 snoop を使う doorbell が必要。 |
| 0.4.70 | D3 BAR1 doorbell retry | Step 106 / `E00000A1` | BAR1 を CPU map し、USERD を低位 GPU VA 0x00100000 に alias。BAR1+VA+0x8C で GP_PUT を通知し、BAR1 readback、backing、MMU fault、FIFO intr0 を記録してから bounded wait。 |
| 0.4.71 | D3 再試行 | Step 106 / `E00000A1` | 0.4.70 実機: BAR1 経由 GP_PUT は読み直し 1、元のメモリーは 0 のままで、この不一致を失敗と扱い待つ前に中断。元のメモリー一致の条件を外し、USERD GP_GET も BAR1 から読む(Claude)。 |
| 0.4.72 | D3 再試行 | Step 106 / `E00000A1` | bit 24 = ENG_RESET(open-gpu-doc)。CE2(PMC_ENABLE bit 21、runlist 0 を GR と共有)が reset のままだった。PFIFO と一緒に CE2 も reset 解除(nvgpu と同じ順)(Claude)。 |
| 0.4.73 | D3 再試行 | Step 106 / `E00000A1` | 0.4.72 実機で bit 24 解消、GP_GET 1、PB GET=PUT、preempt 完了。semaphore 値だけ CPU から見えないため、L2 flush/invalidate 後に読み直し、ref は BAR1 経由で読む(Claude)。 |
| 0.4.73 実機 | **D3 完了** | semaphore 0xD3D3D3D3、ref 0x1234、GP_GET 1、PB GET=PUT、preempt/cleanup 成功。bit 24 は CE2 reset 解除不足 (ENG_RESET) が原因で、D1 の CE2 enable で解消した。 |
| 0.4.74-0.4.75 | D4 の実装・再試行 | Steps 107-108 / `E00000A2`-`E00000A3` | SET_OBJECT 0xB197 + NOP + WFI semaphore。golden を channel context に複写し、header、FECS watchdog、START_CTXSW を設定。GP_GET 後の semaphore 完了を追跡する診断を追加。 |
| 0.4.74 実機 | D3 成功、D4 context switch 停止 | D3 semaphore `0xD3D3D3D3`。D4 GP_GET=2、全 9 dword 読み込み、PBDMA/FIFO/MMU/GR intr は 0。FECS current context は channel instance、FIFO engine は `0x8000A000` (CTXSW_LOAD)。semaphore 未完了、cleanup preempt は CTXSW_SAVE で timeout。 |
| 0.4.76 実機 | **D4 完了** | semaphore `0xD4D4D4D4` を 70 回目に確認、engine `0x2000`、GR status/intr `0`、preempt 成功。GP_GET=2 後も完了 semaphore を L2 flush/invalidate しながら bounded wait する。 |
| 0.4.77 | **D5: 3D color clear** | Step 109 / `E00000A4` | D4 成功後、arena offset `0x1D6000` の 64x64 A8R8G8B8 pitch-linear RT を `0xFFFF00FF` に clear。GP_GET と WFI semaphore を bounded wait し、4096 pixels を CPU readback する。Step 108 は D4 が使用中のため D5 は Step 109。 |

0.4.69 では USERD backing page に CPU write した GP_PUT が PBDMA に届かず、GP_GET と semaphore は 0 のままでした。0.4.70 は2つ目の Memory32Fixed resource を必要範囲だけ uncached map し、GPU PGT に USERD の低位 VA alias を追加します。fifo_bar1_base (0x2254) は alias の page number と valid bit にし、BAR1 block (0x1704) は従来どおり GR instance block に結びます。GP_PUT は BAR1 CPU mapping + 0x00100000 + 0x8C へ書き、BAR1/backing readback と FIFO MMU fault/intr を確認します。BAR1 mapping は ReleaseHardware で解除します。

0.4.67 は D2 の RAMFC subdevice/acquire fields を nvgpu の GM20B `hw_ram_gm20b.h` と `channel_gk20a_setup_ramfc()` に合わせる診断付き修正です。新しい MMIO write は追加せず、RAMFC の subdevice field を `0x30000001`、acquire field を `0x7FFFF902` とします。BAR1/CCSR probe の全 MMIO write は `B3cD2Writes` に記録し、各 write 後に priv-ring を確認します。実機では PBDMA0 interrupt、map/target、FECS current/new context の runlist 前後の値と cleanup 結果を確認してください。

### 0.4.57 D1: PFIFO/PBDMA setup

Step 104 は C2e の成功後にだけ実行します。まず TOP の GPC/TPC/FBP/LTC 値と device-info table、PFIFO interrupt/status、engine status、PBDMA0 status/interrupt と MC_ENABLE_PB を読みます。GM20B の TOP/MC headers で確認できる範囲で PBDMA0 を識別し、TOP device-info の raw words も残します。書き込みは `gk104_fifo_init_pbdmas()` に沿う MC_ENABLE_PB (`0x204`)、PFIFO FB-timeout (`0x2A04`)、GM20B FIFO function table が選ぶ `gk208_fifo_init_pbdma_timeout()` の PBDMA0 timeout (`0x4012C`)、および `gk104_fifo_init()` の PFIFO interrupt clear/enable (`0x2100`, `0x2140`) に限定します。各 write 後に priv-ring station を確認し、readback を記録します。この段階では polling wait は行いません。`E0000095` は priv-ring ACK timeout、`E000009F` は D1 の GR-enable guard/readback failure です。

PFIFO/GR 外の write は MC aperture の `0x204` (Nouveau `gk104_fifo_init_pbdmas()` の PBDMA mask) と PBDMA0 の `0x4012C` (Nouveau `gk208_fifo_init_pbdma_timeout()` の timeout) です。`0x2A04` / `0x2100` / `0x2140` は PFIFO registers です。MC interrupt routing は変更しません。runlist/USERD/RAMFC は backing と IOVA を用意する D2 まで設定せず、PBDMA local interrupt enable は D1 では変更せず現状値を記録します。TOP/device-info encoding と register offsets は MIT nvgpu L4T `hw_top_gm20b.h`, `hw_fifo_gm20b.h`, `hw_pbdma_gm20b.h`, `hw_ram_gm20b.h`, `hw_mc_gm20b.h` で照合しました。Nouveau の GM20B path は `gm20b_fifo` → `gk104_fifo_init_pbdmas()`, `gk208_fifo_init_pbdma_timeout()`, `gk104_fifo_init()` を使います。nvgpu の `gk20a_init_fifo_setup_hw()` の残りと per-PBDMA interrupt policy は D1 の hardware trace を確認後に扱います。

### 0.4.58 D2: one empty channel and runlist registration

Step 105 は C2e と D1 が成功した場合だけ実行します。既存の C2d 2 MiB GR arena 内に RAMFC/RAMIN (同じ instance block と GPU page directory)、USERD page、空の 512-entry GPFIFO ring、空の pushbuffer、GR runlist の1-entry backing を配置します。RAMFC の channel fields と runlist entry は readback し、WFI pointer は C2e が golden save 後に消した値を channel context VA に戻してから公開します。TOP device-info の engine/runlist enum を読み、runlist ID と GR engine ID を別々に記録します。

Nouveau MIT `gk104.c:gk104_chan_ramfc_write()` の RAMFC fields、`gk104_chan_bind()` / `gk104_chan_bind_inst()` の runlist/instance binding、`gk104_runl_insert_chan()` の2-DWORD plain channel entry、`gk104_runl_commit()` の runlist base/submit、`gk104_runl_pending()` の pending bit を参照しました。channel 0 を CCSR に bind/enable して 1-entry runlist の完了を最大2000 poll待ちし、最後は enable clear、channel preempt、empty runlist submit、instance unbind を行います。各 wait は最大2000回 x 1 ms、各 MMIO write は `B3cPrivRingAfterGrWrite()` で確認します。失敗は `E00000A0` です。

明示的な MMIO write は CCSR channel `0x800004` (runlist ID と enable set/clear)、CCSR instance `0x800000` (bind/unbind)、PFIFO runlist base/submit `0x2270/0x2274`、PFIFO preempt `0x2634` です。これら以外に MC interrupt routing や CPU interrupt を変更しません。USERD、GPFIFO、runlist、pushbuffer は C2d の既存 SMMU/GPU page table にマップされた arena suballocation で、追加の物理 allocation はありません。GPFIFO command は一つも投稿せず、channel 登録後に clean unbind してから既存 reset/arena cleanup に進みます。実機では TOP engine/runlist IDs、各 `B3cD2Writes` 行、runlist/pending と preempt polls、channel status、PFIFO/PBDMA/GR status、cleanup final values を確認してください。
