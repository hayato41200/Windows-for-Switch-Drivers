# B3c 起動経路の比較 / 0.4.16

2026-11-12。実機結果はClaude引継ぎメモの0.4.15まで。ここでの比較はコードの比較であり、参照プロジェクトの成功報告の追試ではない。参照先のバイナリ、ビルドスクリプト、テストは実行していない。

## 固定した参照先

- mesa-switch-woa: `b72d0737c179e48b31fd58a9111adc9b01aee3ab`、`tegra-x1-wddm/`以下。
- CTCaer/switch-l4t-kernel-nvgpu: `1ae0167d360287ca78f5a2572f0de42594140312`、`drivers/gpu/nvgpu/`以下。
- torvalds/linux: `v6.12`、`drivers/gpu/drm/nouveau/`以下。
- 読み取り用ソース: `D:\Codex\Windows-for-Switch-Drivers\build\Gpu-B3c-compare-20261112`。取得URLとSHA256は同フォルダーのmanifestを参照。

## 起動の順番に沿った比較

| 段階 | 参考コードと比較結果 | 今回の扱い |
|---|---|---|
| PMUリセット・scrub | mesa `platform/t214_acr.c` prepare、nvgpu `gm20b/pmu_gm20b.c` setup、`gk20a/flcn_gk20a.c` reset、nouveau `falcon/gm200.c` enable。こちらもPMUのresetとDMACTL[2:1]の有限scrub待ちを行う。参照側には割込mask解除、idle待ち、gating設定もある。 | 一致する部分と追加の初期化差がある。実装全体の同一性は未確認。今回追加しない。 |
| FB/MMU初期化 | nvgpu `common/fb/fb_gm20b.c` の `gm20b_fb_set_mmu_page_size()` がMMU_CTRL(0x100C80)のbit11を1にする。nouveau `subdev/fb/gm20b.c` はdefault_bigpage=0、`fb/gm200.c` のinit_page(case0)がbits12:11を1にする。従来ログ0x00208001ではbit11=0。 | **0.4.16でbit11のみセット**。圧縮関係bit12は変更しない。nvgpuの単独bit設定に対応。 |
| MMU debug/flush領域 | nvgpu fb初期化とnouveau `fb/gm200.c` は0x100CC8/CCCにdebug bufferを設定する。こちらは既存B2aの診断とFB flushのみで、B3c固有のdebug buffer設定がない。 | 未実装差として残す。今回同時に追加しない。 |
| RAMIN PDB・VA limit | nvgpu `gk20a/mm_gk20a.c` init_inst_blockは0x200/204にPDB、0x208/20CにVA limit、gm20b/mm_gm20b.cでRAMIN bit11を設定する。nouveau `gf100_vmm_join_()`も0x200と0x208を64-bitで書き、`gm200_vmm_join_()`が64 KiB構成時のみbit11を足す。比較した初期化関数にはほかの必須RAMIN欄の設定は見つからない。nvgpuはlimit低位12 bitをmask、nouveauはlimit-1をそのまま書く。 | こちらは後者と同じlimit形式。PDB bit11=0で128 KiB構成。インスタンスの住所がfaultに出ても、GPUがRAMIN内容を正しく読んだ証明にはならない。 |
| PGD/SPTの形 | mesa `core/gm20b_mmu.c/.h`は64 KiB大ページ構成(dir shift26、SPT14bit、PDB bit11=1)。こちらは128 KiB構成(dir shift27、SPT15bit、PDB bit11=0)。両方の構成はnouveauとnvgpuに定義されている。 | 表の式をさらに変更する前に、MMUがPDBのサイズ指定を参照する設定を追加する。現在のVAではPGD[2]とSPT[0x53]。 |
| 結び付け | mesa t214_acr、nvgpu pmu_gm20bのbl_bootstrap、nouveau falcon/gm200のfw_loadを比較。ITFENを有効にしNEW_INSTBLKをSYS_NCOH/VALID/instance PFNで書く点は対応。こちらはnouveau式のbind handshakeも実施する。 | 0.4.15実測のencoded/read一致とfault instタグ一致を記録。GPU向け住所のbit34は継続。 |
| TRANSCFGとdescriptor | nvgpu pmu_setup_apertures、mesa t214_acrはTRANSCFG[0..4]={4,0,4,5,6}。こちらのPmuBindInstも同じ。ctx_dma=1はvirtual。code/data DMA base=GPU VA>>8、ns/sec offset、sizeはacr_gm20bとnouveau acr/gm20bの同種欄に対応。 | 起動直前のDMACTL、ITFEN、TRANSCFG1を追加で読む。snapshotはbind前なので、bindによる変更は含まない。 |
| descriptor/IMEMの順番 | nvgpu flcn_gk20aのbl_bootstrapはDMEM descriptor→IMEM→mailbox→bootstrap。こちらはIMEMを先にPIO配置し、bind後にdescriptorを再度書く。いずれもCPU起動前に配置・読戻しを行う。 | 順序差を記録。現在のPCはBL内であり、この差が原因とは未確認。 |
| TLB無効化 | mesa `platform/t214_mmu.c`とnvgpu fb_gm20bがqueue空き確認、PDB指定、PAGE_ALL、bit15待ち。こちらも実施し、さらにALL_PDB診断を行う。status bit15観測は個別要求の完了を保証しない。 | 既存要求を維持し、MMUサイズ指定はbind/無効化より前に行う。 |
| CPU起動直前のDMACTL | nvgpu `gk20a_falcon_bl_bootstrap()`→`gk20a_falcon_bootstrap()`はDMACTL.REQUIRE_CTX=0を書いてBOOTVEC/STARTCPUへ進む。mesa t214_acrもDMACTL=0。こちらは書いておらず実測1。一方nouveau fw_loadはinstanceなしの場合にのみ0を明示する。 | **次候補**。0.4.16では変更せず読み取りのみ。MMUモード設定と同時に変えて原因を混ぜない。 |
| mailbox・有限待ち | mesa/nvgpuはDEADA5A5、nouveauとこちらはCAFEBEEF。どちらも成功が書き換えることを検出。こちらの2000回×1msは有限。 | 成功判定はhaltとmailbox0=0。これだけで後続PMU/FECSの起動成功とはしない。 |
| faultと終了 | mesa `core/fault.c`はPFIFO/MMU/PBDMAの状態をまとめる。テスト群はmock IOでの順序/値/timeout等を検査するモデルで、こちらの実機成功の証明にはならない。こちらはPFIFO MMU recordを読む。 | 同じVA/type/unit/inst/PCか変化を比較。GPU reset後にSMMU復元・メモリ解放する従来フローを使う。 |

## 0.4.16の試験範囲

書き込み上の追加はMMU_CTRL bit11の設定のみ。Step57で以前の値、設定後、readbackのbit11を記録する。読戻しでbit11が立たなければE000006CでACRを起動せず既存の終了処理へ進む。正常に立てばStep50/51/54/55/56へ進む。Step番号は実行順の連番ではない。

従来値0x00208001なら、同じqueue状態の設定後は0x00208801が目安。queueに関するread-only fieldは変動し得るため、ワード全体の一致で判定しない。PDB bit11=0による128 KiB指定をMMUが使うことを狙う。従来のglobal modeが実際にどのサイズだったかは、bit0だけから断定しない。

Install.cmd→AcrTest.cmdを1回実行し、C:\SwitchWOA-Gpu.txt全体を保存する。ACR page geometry probe行と、PGD faultが変化するかを比較する。原因が直ったという主張は実機結果が出るまで行わない。

主要な一次資料:
- https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/common/fb/fb_gm20b.c
- https://github.com/CTCaer/switch-l4t-kernel-nvgpu/blob/1ae0167d360287ca78f5a2572f0de42594140312/drivers/gpu/nvgpu/gk20a/flcn_gk20a.c
- https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/nouveau/nvkm/subdev/fb/gm200.c
- https://github.com/ruje0504-2/mesa-switch-woa/tree/b72d0737c179e48b31fd58a9111adc9b01aee3ab/tegra-x1-wddm
