# SwitchPatcher

PC で動く道具です。Switch の SD を PC に挿して、SD の Windows に次のものを入れます。

- Switch のドライバー
- テストモードとテスト証明書
- コントロールセンター

Windows のイメージ（ARM64）を、SD に入れ直すこともできます。設計は `docs/Design-SwitchPatcher-ja.md`。


## v0.6.1（2026-11-26）: GPU のファームウェアも入れる

- 「ファームウェア」のフォルダーに linux-firmware の `nvidia\gm20b`（`acr`、`gr`、`pmu` の 14 ファイル）があれば、SD の `SwitchWOA\firmware\nvidia\gm20b` にコピーする。GPU キット（`kits\Switch-Gpu`）のインストールは、ここ（Switch では `C:\SwitchWOA\firmware\nvidia\gm20b`）から読み込む。NVIDIA のファイルなので、リリースやこのツールには入っていない。
- 探す場所: フォルダーの中の `nvidia\gm20b`、`gm20b`、`firmware\nvidia\gm20b`、`lib\firmware\nvidia\gm20b`、フォルダーそのもの。ファイルが欠けていたら、「使えないもの」に足りない名前を出して、GPU の分はコピーしない。
- switchgpu はリリースに入っていないので、ドライバーは今まで通り GPU キットで入れる。
- 確認したこと（2026-11-26）: PC で、ユーザーのフォルダーから 14 ファイルを見つけて、一時フォルダーに同じ中身でコピーできた。実機: Windows 11 23H2（22631.7714）の新しい SD を 0.6.1 で作り、Switch で起動。`C:\SwitchWOA\firmware\nvidia\gm20b` に `acr`、`gr`、`pmu` ができていて、GPU キットのインストールも通った。23H2 の SD を作ったのも、これが初めて。

## v0.6.0（2026-11-18）: hekate からの起動

- 「4. hekate」を足した。「hekate から起動できるようにする」を選ぶと、SD の ESP（hekate が読む FAT の区画）に次のものを置く。

  | ESP のどこに | 中身 | どこから |
  |---|---|---|
  | `switchroot\switchwoa\bl33.bin` | hekate 用の Switch UEFI | このプロジェクトのリリース（manifest の `uefi`）。ファイルを選べば、そちらを使う |
  | `switchroot\switchwoa\bl31.bin` | ARM Trusted Firmware | **配布しない。** 自分で用意したファイル（LineageOS の Switch 版から取り出したもの）を選ぶ。SD にすでにあれば選ばなくてよい。新しい SD では必要 |
  | `bootloader\hekate_ipl.ini` | `[Windows (SwitchWOA)]` の項目（`l4t=1`、`boot_prefixes=switchroot/switchwoa/`、`id=SWOAHKT`） | 項目がなければ足す。ini がなければ作る |
  | `bootloader\...`、`hekate_ctcaer_<版>.bin`、`bootloader\update.bin` | hekate そのもの | 「hekate も入れる・新しくする」を選んだときだけ。hekate の公式リリース（GitHub の CTCaer/hekate）の最新版を、その場でダウンロードする |

- 前からある `bl33.bin`、`bl31.bin`、`hekate_ipl.ini` は、`.bak-<日時>` の名前で残してから書き換える。同じ中身なら書き換えない。
- `id=SWOAHKT` は、電源ボタンのドライバー（switchpwrbtn 1.0.3.0 以降）が、Windows の再起動で hekate から Windows に直接戻るときに使う。
- 置く前に確かめること: bl33.bin は大きさと UEFI のファームウェアボリューム（`_FVH`）、bl31.bin は大きさ。hekate の zip は、`bootloader\sys\l4t` の 3 つのファイルとペイロードがあること。
- 試したこと（2026-11-18）: PC 上の空のフォルダーを ESP に見立てて、hekate v6.5.5 のダウンロード、置き方、ini の作成と追記、2 回目に何も変えないこと、bl31 がないときに止まることを確かめた。
- 未確認: 本物の SD での書き込みと、それを Switch で起動すること。「新しい SD」（GPT、先頭が ESP）を hekate が読めるかどうか。

## v0.5.0（2026-11-08）: ファームウェアのフォルダー

- 「3. 入れるもの」に「ファームウェア」のフォルダーを選ぶ欄を足した（任意）。ユーザーが自分の Switchroot Linux からコピーしたファイルを、SD の次の場所に入れる。どのファイルもリリースやこのツールには入っていない。

  | ファイル（Switchroot の場所） | SD のどこに | 使うもの |
  |---|---|---|
  | `tegra21x_xusb_firmware`（`/lib/firmware`） | ESP の一番上 | UEFI（USB） |
  | `brcmfmac4356A3-pcie.bin` / `.txt`（`/lib/firmware/brcm`） | `Windows\System32\drivers\switchwifi\brcmfmac4356-pcie.*` | switchwlan（Wi-Fi） |
  | `BCM4356A3.hcd`（`/lib/firmware/brcm`） | `Windows\System32\drivers\SwitchWOA\` | switchbt（Bluetooth） |
  | `wifi-mac.txt`（自分で作る 1 行、L4T の DT の `nvidia,wifi-mac`） | SYSTEM ハイブの `Services\switchwlan\Parameters\MacAddress` | switchwlan |

- フォルダーの中の `firmware`、`brcm`、`lib\firmware(\brcm)`、`nvidia\tegra210` も探す。XUSB は linux-firmware の名前 `xusb.bin` も受け付ける（実機では未確認）。
- 選んだときに、見つかったものと使えないもの（大きさがおかしい、.hcd の形が違う、MAC の形が違う）を表示する。
- 未確認：実際の SD での書き込み（PC でビルドが通ったところまで）。

## 使い方

1. **SD のバックアップを取る。** 「Windows を入れる」では、SD の Windows の区画が消えます。
2. **起動する。** `SwitchPatcher.exe` を起動する。管理者の確認が出る。
3. **SD を選ぶ。** 「SD カード」と「Windows の区画」を選ぶ。SD は、Switch の SD のように、GPT で ESP と NTFS の区画があるものに限る。
4. **することを選ぶ。**
   - **入っている Windows にパッチを当てる:** 今の Windows はそのまま。
   - **Windows のイメージを入れる:** ISO、WIM、ESD と、ARM64 のエディションを選ぶ。Windows の区画をフォーマットして入れ直す。
   - **新しい SD に Windows を入れる（0.2.0 以降）:** SD 全体を消して、GPT・ESP（FAT32、260 MB）・MSR（16 MB、0.3.0 以降）・Windows の区画（NTFS、残り全部）を作ってから、イメージを入れる。
     - 消せるのは、カードリーダーの SD（SD、USB、MMC）だけ。
     - 消す前に、ダウンロードしたものを全部確かめる。
     - SD の大きさを入力しないと進まない。
5. **入れるものを選んで、「開始」を押す。**
   - 確認の画面が出る。
   - イメージを入れるときは、区画の大きさを入力する画面も出る。
6. **SD を外して、Switch で起動する。** 終わったら、SD を「安全に取り外す」から外す。

## 入れるもの

- **ドライバー:** GitHub の最新のリリースから取ってくる。入れる前に、次を確かめる。
  - ZIP の SHA-256
  - 各 .sys
  - 署名
- **テストモード:**
  - 証明書を、起動していない Windows の「信頼されたルート証明機関」と「信頼された発行元」に入れる。
  - ESP の BCD で `testsigning on` にする。
- **コントロールセンター:**
  - `C:\SwitchWOA\SwitchCenter\` に置く。
  - ログインしたときに起動する（`HKLM\...\Run`）。
- **SD からの起動（0.3.0 以降、リリースの `Switch-Sd`）:** SD の Windows のビルドを見て決める。
  - **Windows 11（ビルド 22000 以上）:** ESP の `EFI\Boot\bootaa64.efi` に `SDLOADAA64-NOCID.EFI` を置き、SD のドライバー nvtsdhc を入れて、起動時に読み込む（Start=0）。nvtsdhc はテスト署名なので、テストモードも必ず入れる。
  - **Windows 10:** 「Windows を入れる」ときだけ `SDLOADAA64.EFI` を置く。パッチだけのときは、ESP のローダーを変えない。
  - テストモードを入れるときは、BCD の起動のデバッグ設定（bootlog、sos、bootstatuspolicy、nocrashautoreboot）を外す。
- **Windows を入れたときだけ:**
  - 初回起動で、Microsoft アカウントとネットワークを求めないようにする（unattend.xml、BypassNRO）。
  - アカウントは、初回起動の画面で自分で作る。

ESP の起動ファイル（BCD など）は、書き換える前に `ドキュメント\SwitchPatcher\backup-<日時>\` に保存します。

Windows のイメージとライセンスは、各自で用意してください。

## 試したこと

- **仮想ディスク（VHDX、Switch の SD と同じ GPT・ESP・NTFS の形）での試験（2026-10-27）:** ARM64 の WinPE（10.0.26100）を相手に、「Windows を入れる」と「パッチ」を最後まで通した。
  - 展開と bcdboot（ESP のバックアップを含む）
  - 証明書（ROOT、TrustedPublisher）、自動起動、BypassNRO
  - 8つのドライバーの登録（`dism /Get-Drivers` で確かめた）
  - テストモード（`testsigning Yes`）
  - コントロールセンターの配置
- **0.2.0 の「新しい SD」（2026-10-27）:** 空の仮想ディスク（RAW）から試した。GPT・MSR・ESP・NTFS ができて、そのあとの展開とパッチも全部通った。
- **0.2.1:** 本物の SD（32 GB）で「新しい SD」を試したところ、消したあとの SD が MBR で初期化されたまま残り、GPT にできずに止まった。0.2.1 では、消したあとの形を見て、RAW なら初期化し、MBR なら GPT に変える。
- **0.2.2:** 0.2.1 で SD の初期化と Windows 11 22621 ARM64 の展開までは通ったが、PC（Windows 11 26100、x64）の bcdboot が「ブート ファイルをコピーしようとして、エラーが発生しました」（exit 193）で失敗した。0.2.2 では、bcdboot が失敗したら、`Windows\Boot\EFI` を ESP にコピーし、bcdedit で BCD を作り直す。
- **0.2.3:** 0.2.2 で作った Windows 11 22621 の SD は、Switch でロゴとくるくるまで進んだが、`INACCESSIBLE_BOOT_DEVICE` で止まった。
  - 理由: Switch の UEFI は、SD を「ベンダー独自の装置」として Windows に渡す。そのため Windows の起動ローダーは SD からの起動と分からず、SD のときだけ読み込むドライバー（sdbus、sdstor）を読み込まない。動いている Win10 の SD では、この2つが Start=0 になっていた。
  - 0.2.3 では、毎回のパッチで、オフラインの SYSTEM レジストリの sdbus と sdstor を Start=0 にする。
  - あとで分かった本当の理由: 新しい ESP の `bootaa64.efi` が `bootmgfw.efi` のままで、SD ホストの SSDT を足すローダーが無かった（UEFI の DSDT には SD ホストが無い）。
- **0.2.4:** 自分で作った BCD に `Description\System = 1` を足す。無いと、初回起動の設定（specialize）が「File is not system store」で失敗する。
- **0.3.0（2026-10-29）:** Windows 11 22621 は標準の sdbus では SD から起動できない（起動ドライブを読めないか、止まる）。手で試して動いた組み合わせ（NOCID のローダー、nvtsdhc 0.3.2、テストモード）を、パッチに入れた。新しい SD に MSR を作り、空のカードリーダー（0 GB）は一覧に出さない。
- **本物の SD と Switch:** 0.2.4 までで作った Win11 の SD に、手作業で 0.3.0 と同じ変更をしたものは、Switch でデスクトップまで動いた（SD の読み込み 18 MB/s）。0.3.0 そのものでも、本物の SD と Switch で動作を確かめた。
