# SwitchPowerButton(switchpwrbtn)

[English](README.md)

Nintendo Switch(Erista)の電源ボタンの KMDF ドライバー。Windows のシャットダウンの最後に、再起動(hekate に戻る)と電源オフもする。デバイスは `ACPI\SWOA0601`(UEFI R374 以降)、PMIC は I2C5 の MAX77620(アドレス 0x3C)。入れ方は `kits/Switch-PowerButton`。

## 電源ボタン

- ボタンは PMIC の EN0 につながっている。SoC の GPIO にはつながっていない。
- 50 ms ごとに SPB で次の 2 つを読む。
  - ONOFFSTAT(0x15)の bit 2: 押している間は 1。
  - ONOFFIRQ(0x0B): 読むと消える。50 ms より短い押し方も、ここで拾う。
- 押したことは、VHF(下に `vhf.sys`)の HID の「System Power Down」ボタンとして報告する。何をするかは、Windows の電源オプションで決まる。
- 数秒の長押しでは、これまでどおり PMIC が自分で電源を切る。

## 再起動とシャットダウン(1.0.2.0)

Windows はシャットダウンを最後まで終えるが、最後のファームウェアの呼び出しで、リセットも電源オフも効かない(どちらの起動の経路でも、UEFI の ResetSystem まで来ない)。そこでドライバーが、S5 でデバイスの電源を切る段階(`EvtDeviceD0Exit`)で、自分で処理する。ファイルシステムとレジストリの書き出しは、その前に終わっている。再起動か電源オフかは `WdfDeviceGetSystemPowerAction` で見分ける。

- **再起動**(`PowerActionShutdownReset`): reboot-to-payload。switchroot の bl31(`r2p_reboot_to_payload`、`tegra_soc_prepare_system_reset`)と同じ手順。
  1. CAR の `SCLK_BURST_POLICY`(0x28)= 0x10000000、`SCLK_RATE`(0x30)= 0、1 ms 待つ。
  2. IRAM 0x40010000〜0x4003FFFF を 0 にし、payload を 0x40010000 にコピーし、+0x94 の 32 バイト(起動の設定)を 0 にする。
  3. PMC の `SCRATCH0`(0x50)|= 1(warmboot)、`SCRATCH45`(0x234)= 0x2E38DFFF、`SCRATCH46`(0x238)= 0x6001DC28、`SCRATCH33`(0x120)= 0x40010000、`SCRATCH40`(0x13C)= 0x6000F208。
  4. `PMC_CNTRL`(0x0)|= `MAIN_RST`(0x10)。

  Erista だけ(APB_MISC の `GP_HIDREV` のメジャーリビジョンが 2 未満)。payload(既定は `\Device\Harddisk0\Partition1\bootloader\update.bin`、SD の FAT の区画にある hekate)は、Windows が動いている間に非ページメモリーに読んでおく。シャットダウンのときには読まない。大きさは 0x200〜0x30000 バイト。payload がなければ PSCI `SYSTEM_RESET` を呼ぶ。switchroot の bl31 なら、hekate から渡された payload で R2P をする。coreboot の bl31 なら普通のリセットになる。
- **シャットダウン**(`PowerActionShutdown`、`ShutdownOff`): MAX77620 の `ONOFFCNFG2`(0x42)の `SFT_RST_WK`(bit 7)を外し、`ONOFFCNFG1`(0x41)= `SFT_RST`(bit 7)。Linux の `max77620_pm_power_off` と同じ。だめなら `PWR_OFF`(bit 1)。まず SPB で送り、I2C コントローラーがもう受け付けなければ、I2C5 のレジスタ(0x7000D000)を直接使う。
- 処理の前に、バックライトの PWM(0x7000A000)を消す。黒い画面でバックライトが消えていれば、ドライバーの処理まで来ている。
- ほかの電源の動き(デバイスの取り外し、ドライバーの更新、休止状態、スリープ)では、何もしない。

2026-11-17 に実機で確かめた(Erista、Windows 11)。hekate から起動した場合も、RCM の coreboot から起動した場合も、再起動で hekate のメニューに戻った。シャットダウンで電源が切れた(hekate の経路で確認)。

## 設定

`HKLM\SYSTEM\CurrentControlSet\Services\switchpwrbtn\Parameters`。起動したときと、10 秒ごとに読む。

| 値 | 意味 |
|---|---|
| `ResetEnable` | 1(既定): ドライバーが再起動・電源オフをする。0: Windows に任せる(前と同じく止まる) |
| `RestartMethod` | 0 自動(payload があれば R2P、なければ PSCI)、1 PMC だけ、2 PSCI `SYSTEM_RESET`、3 普通の PMC リセット、4 バックライトを消すだけ(試験用) |
| `PowerOffMethod` | 0 PMIC(既定)、1 バックライトを消すだけ(試験用)、2 Windows に任せる |
| `PayloadPath` | REG_SZ。payload の NT パス |
| `RebootToId` | REG_SZ、空白なしの ASCII で 1〜7 文字(1.0.3.0 から)。再起動のあと、hekate が `hekate_ipl.ini` の `id=` がこの値の項目を自動で起動する。空(既定)なら hekate のメニュー |
| `RebootToIndex` | DWORD(1.0.3.0 から)。`RebootToId` が空のときに使う。`hekate_ipl.ini` の N 番目の項目を自動で起動する(`[config]` の次が 1)。0 ならメニュー |

再起動の行き先は、hekate の起動設定(`boot_cfg_t`、hekate の `bdk/utils/types.h`)として渡す。id のときは `boot_cfg` = 0x05(AUTOBOOT_EN と FROM_ID)と `id[8]`、番号のときは `boot_cfg` = 0x01 と `autoboot` = N。Windows から R2P をするときは payload の +0x94 に書き、PSCI を呼ぶときは先に switchroot の bl31 の SiP 0xC2FFFE04(R2P_SET_CFG)で渡す。hekate が起動するときに音量の「−」ボタンを押していれば、メニューが出る。

ドライバーは、同じキーに状態を書く(`DriverVersion`、`Hidrev`、`Erista`、`Mapped`、`PayloadStatus`、`PayloadSize`、`PayloadWord0` など)。`kits/Switch-PowerButton/Status.cmd` で見られる。

## ビルド

```powershell
.\tools\Build-Driver.ps1 -Name switchpwrbtn -Sources drivers\SwitchPowerButton\src\switchpwrbtn.c `
  -Inf drivers\SwitchPowerButton\switchpwrbtn.inf -OutDir build\SwitchPowerButton -Libs vhfkm.lib,libcntpr.lib `
  -ExtraIncludes '$Kits\Include\10.0.19041.0\km\spb\1.0','$Kits\Include\10.0.26100.0\shared'
```

リリースでは `release\Make-Release.ps1` がビルドする。
