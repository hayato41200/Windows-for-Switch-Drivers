# Wi-Fi 段階 2a：Windows から Wi-Fi チップが見えるかの試験

## 中身

| ファイル | 内容 |
|---|---|
| `switchwifi.sys` | KMDF ドライバー（ARM64、テスト署名）。ソースは `..\..\drivers\SwitchWifi\src\switchwifi.c` |
| `switchwifi.inf` | `ACPI\SWOA0D01` |
| `Wifi.ps1` | Install、Status、Retry、Remove |
| `Install.cmd` など | `Wifi.ps1` を各モードで動かすだけのもの。右クリックして「管理者として実行」 |

## ドライバーの作り

- **なぜ ACPI デバイスか:** Tegra210 の PCIe は設定空間が ECAM ではないので、Windows の pci.sys では列挙できない。UEFI R405 がリンクを上げて BAR を割り当て、DSDT の `WLN0` としてドライバーに渡す。
- **リソース（この順番で受け取る）:**
  1. BAR0（0x13400000、32 KB）。先頭 4 KB がバックプレーンの窓。
  2. BAR2（0x13000000、4 MB）。チップの RAM。
  3. バス 1 の設定空間の窓（0x11FFF000、4 KB）。
  4. ルートポート 1 のレジスタ（0x01001000）。
  5. AFI（0x01003800）。
- **このバージョンでやること（読むだけ）:**
  - ルートポートのリンク状態を見る。
  - リンクが上がっていれば、エンドポイントの設定空間を読む。
  - ChipCommon のチップ ID を読み、EROM をたどってコアの一覧を作る。
- **安全策:** エンドポイントが応答しないと、SError で Windows が止まる。そこで、最初のアクセスの前に `Busy=1` を書いて保存し、最後に 0 に戻す。次に起動したときに `Busy=1` が残っていたら、エンドポイントには触らない（`Skipped=1`）。そのため、起動を繰り返して止まり続けることはない。`Retry.cmd` で `Busy` を消すと、もう一度試せる。
- **記録:** `Services\switchwifi\Parameters` に書く。`Step` は、止まったときにどこまで進んでいたかを示す。

## 手順

1. **Switch を UEFI R405 で起動する。** 起動画面の「R404 wifi: BCM4356 rev 3 found on PCIe.」が出ることを確認する。
2. **このフォルダーを Switch に移す。**
3. **`Install.cmd` を右クリックして「管理者として実行」する。** 最後に出る結果を写真に撮る。
4. **もし Windows が止まったら:** 次の起動ではドライバーがエンドポイントに触らないので、Windows は起動する。`Status.cmd` を管理者として実行し、`step=` の値を見せてほしい。

## 0.1.1 の結果（2026-11-07、ユーザー報告）

- **Windows の中から読めた。** リンクは R405 のまま上がっていて、設定空間、チップ ID 17234356、EROM（102 語、コア 13 個）を読めた。
- **コア:** ChipCommon r47、D11（0x812）r48、ARM CR4 r6（0x18002000）、PCIe2 r11（0x18003000）など。
- **RAM:** ARM CR4 の cap=0x524（A 4 個、B 2 個）で、768 KB。brcmfmac の BCM4356（rambase 0x180000）と同じ。

## 0.2.0: 段階 2b（ファームウェアの読み込み）

- **流れ（Linux 4.9 の brcmfmac と同じ）:**
  1. ARM CR4 を止め、D11 をリセットする（set_passive）。
  2. ファームウェアを RAM の 0x180000 に書いて、読み返して確かめる。
  3. RAM の最後の語を 0 にして、NVRAM を RAM の最後に置く。
  4. リセットベクターを TCM の 0 番地に書き、CPU を動かす（set_active）。
  5. 最大 5 秒、ファームウェアが共有領域のアドレスを書くのを待つ。
- **うまくいったら:** 共有領域のフラグを記録し、ファームウェアのコンソール（チップ自身のログ）を読む。
- **まだやらないこと:** brcmf_pcie_reset_device にあるチップのウォッチドッグリセット。UEFI が電源を入れた直後で、チップはまだブート ROM にいるため。
- **ファームウェアは同梱しない（再配布できない）。** 自分で `brcmfmac4356-pcie.bin` を用意する。NVRAM の `brcmfmac4356-pcie.txt` があれば、それも使う。入手先は、Switch の Linux の `/lib/firmware/brcm/` か linux-firmware。
- **置き場所:** `Firmware.cmd`（または `Install.cmd`）が、このフォルダー（か `firmware\`）にあるファイルを `System32\drivers\switchwifi` にコピーする。
- **ファイルが無いとき:** 段階 2b は飛ばす。段階 2a の結果だけが出る。

## 0.2.0 の結果（2026-11-07、ユーザー報告）

- **ファームウェアが動いた。** 自分の Switchroot のバックアップから取り出した 7.35.252.108 を使った。
  - 書き込んだ内容は読み返しても誤りなし（verifyBad=0）。
  - CPU を動かしてから 50 ms で、共有領域のアドレス（0x1DF298）が書かれた。flags=0x20005（msgbuf プロトコル版 5）。
- **ファームウェアのコンソール（チップ自身のログ）が読めた。** 「RTE (USB-SDIO-PCIE-MSG_BUF) 7.35.252.108 on BCM4356 r3」「pciemsgbuf0: Broadcom PCIE MSGBUF driver」「wl0: Broadcom BCM4356 802.11 Wireless Controller」と出た。
- **次:** msgbuf のリング（DMA）を作り、最初のコマンド（MAC アドレスの読み出しなど）をファームウェアに送る。

## 0.3.0: 段階 3（msgbuf のリングとコマンド）

- **流れ（brcmfmac の pcie.c と msgbuf.c と同じ）:**
  1. 共有領域から ring info を読む。
  2. ホストのメモリにリングを 5 本作る。H2D が制御と受信バッファー、D2H が制御の完了、送信の完了、受信の完了。
  3. リングの場所を、デバイスの RAM にあるリングの記述子に書く。
  4. scratch と ringupd のバッファーを渡す。
- **インデックス:** このファームウェアは DMA インデックスに対応していない（flags=0x20005）。そのため、読み書きのインデックスはデバイスの RAM に置く。
- **ホストのメモリ:** Windows の DMA の仕組みは使わない。4 GB より下の連続した 128 KB を、write-combined（Normal でキャッシュなし）で確保し、その物理アドレスをそのままチップに渡す。Tegra の AFI は受信側のアドレスを変換せず、SMMU も使っていないため。
- **コマンド:**
  1. イベント用と ioctl の応答用のバッファーを 4 個ずつ渡す。
  2. ioctl を 3 つ送る。GET_VERSION、iovar `ver`、iovar `cur_etheraddr`。
  3. 制御の完了リングを 10 ms ごとに見に行く（割り込みは使わない）。
- **MAC アドレス:** 表示はベンダーの半分だけにする。全体はレジストリの `IoctlMac` にだけ残す。
- **ドライバーが外れるとき:** チップの CPU を止めてから、メモリを解放する。止めずに解放すると、チップが解放したメモリに書き込むおそれがあるため。

## 0.3.0〜0.3.2 の結果（2026-11-07、ユーザー報告）

- **0.3.0 / 0.3.1:** 最初の呼び出し（H2D_MAILBOX_0）の直後に、Windows が止まった（固まる、または WHEA_INTERNAL_ERROR）。止まったのは step 22〜23。
- **原因:** エンドポイントの CMD が 0x0002 で、バスマスターが無効だった。チップがホストのメモリを DMA で読めなかった。
- **0.3.2:** バスマスターを有効にしたら（CMD=0x00100006）、通った。
  - `msg : ok: ioctls answered over DMA`
  - GET_VERSION は st=0、値は 2。
  - `ver` は st=0、「wl0: Aug 29 2021 19:23:35 version 7.35.252.108 (c216b7c) FWID 01-65333037」。
  - `cur_etheraddr` は st=0、00:90:4C:xx:xx:xx（Broadcom の既定の OUI）。Switchroot の NVRAM の macaddr は仮の値で、Linux は本体の MAC を DT から設定している。Windows でも、本体の MAC を別に設定する必要がある。

## 0.4.0: 段階 4a（スキャン）

- **目的:** Windows の Wi-Fi 画面に対応する（WiFiCx か WDI）前に、チップの無線が本当に動くかを確かめる。
- **流れ（brcmfmac と同じ）:**
  1. WLC_SET_INFRA 1 と WLC_UP を送る。
  2. `event_msgs` を読み、ESCAN_RESULT（69）のビットを足して書き戻す。
  3. iovar `escan` を、全チャンネルのアクティブスキャンで送る。
- **イベントの受け取り:**
  - WL_EVENT の完了を受け取ったら、そのイベントバッファーの中身（brcmf_event、ビッグエンディアン）を読んでから、同じバッファーを渡し直す。
  - ESCAN_RESULT の status 8（途中）では、BSS の記録（SSID、チャンネル、RSSI）を集める。それ以外の status が来たら、スキャンの終わり。
  - 最長 10 秒待つ。
- **表示:** 見つかったネットワークを、強い順に 12 件まで表示する（SSID は UTF-8 から変換）。
- **あわせて表示するもの:** Windows のビルドと、wificx.sys / wdiwifi.sys の有無（WiFiCx を使えるかを決めるため）。
