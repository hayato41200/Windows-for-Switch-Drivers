# 電源ボタン・再起動・シャットダウン(switchpwrbtn)

## できること

- **電源ボタン:** 押すと、Windows の電源オプションの「電源ボタンを押したときの動作」をする(画面を消す、シャットダウンなど)。
- **再起動で hekate に戻る(1.0.2.0 から):** Windows の再起動で、hekate のメニューに戻る。PC から payload を入れ直さなくてよい。
- **シャットダウンで電源が切れる(1.0.2.0 から):** 前は、画面が黒・バックライトがついたまま止まっていた。
- 2026-11-17 に実機で確かめた(Erista、Windows 11。hekate から起動した場合と、RCM の coreboot から起動した場合の両方)。

## 中身

| ファイル | 内容 |
|---|---|
| `switchpwrbtn.sys/.inf/.cat` | ドライバー(ARM64、テスト署名)。ソースと説明は `drivers\SwitchPowerButton` |
| `PowerButton.ps1` | ボタン: Install、Status、Watch、SetAction、Remove |
| `Install.cmd` | ドライバーを入れ、hekate の payload の場所を設定する(`Reset.ps1 -Mode Install`) |
| `Status.cmd` | 再起動・電源オフの状態を表示する(読むだけ) |
| `Reset-Disable.cmd` / `Reset-Enable.cmd` | ドライバーによる再起動・電源オフを止める / 戻す |
| `Restart-Psci.cmd` / `Restart-Auto.cmd` | 再起動のやり方を PSCI(bl31 任せ)にする / 標準に戻す |
| `RebootTo-Windows.cmd` / `RebootTo-Menu.cmd` | 再起動のあと、hekate が Windows を自動で起動する / hekate のメニューを出す(1.0.3.0 から) |
| `Reset.ps1` | 上の .cmd の本体。ログは `C:\SwitchWOA-Reset.txt` |

.cmd は、どれも右クリック →「管理者として実行」で使う。

## 入れ方

1. **UEFI R374 以降で起動する。** I2C のドライバー(Stage3)を先に入れておく。
2. **このフォルダーを Switch に移す。** 例: `C:\SwitchWOA\PowerButton`。
3. **`Install.cmd` を管理者として実行する。** 最後の表示で次を確かめる。
   - `driver version: 2 OK`
   - `payload: status=0x00000000 (loaded)`
   - `restart will use: R2P from Windows …`
4. **ボタンの動作を決める。** 最初は安全な「画面を消す」がよい。
   ```bat
   powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\PowerButton\PowerButton.ps1 -Mode SetAction -Action Display
   ```
   ほかに `-Action Shutdown`、`Nothing`、`Sleep` がある。スリープは、動くか確かめるまで使わないでほしい。
5. **再起動とシャットダウンを試す。**

コントロールセンターの「ドライバー」タブから更新したときは、payload の場所は既定(`\Device\Harddisk0\Partition1\bootloader\update.bin`)になる。SD が 1 枚だけなら、たいていこれで合っている。`Status.cmd` で `loaded` になっていなければ、`Install.cmd` を実行する。

## 再起動したら Windows を自動で起動する(1.0.3.0 から)

既定では、再起動すると hekate のメニューに戻る。hekate がそのまま Windows を起動するようにもできる。

1. **SD の `bootloader\hekate_ipl.ini` の、Windows の項目に `id=SWOAHKT` があるか確かめる。** SwitchWOA の hekate キットの項目には、はじめから入っている。ほかの項目名で作ったときは、その項目に 1 行足す(PC で SD を開くか、hekate の中で編集する)。例:
   ```ini
   [Windows (SwitchWOA)]
   l4t=1
   boot_prefixes=switchroot/switchwoa/
   id=SWOAHKT
   ```
2. **`RebootTo-Windows.cmd` を管理者として実行する。** `after a restart: autoboot the ini entry with id=SWOAHKT` と出れば設定済み。
3. **再起動する。** hekate が Windows の項目を自動で起動する。id は ini と大文字・小文字まで同じでないと一致しない(別の id にするときは `Reset.ps1 -Mode RebootTo -Id <id>`)。
   - hekate のメニューに入りたいときは、hekate が起動するときに音量の「−」ボタンを押したままにする。
   - メニューに戻す設定は `RebootTo-Menu.cmd`。
   - 項目の番号で指定することもできる: `Reset.ps1 -Mode RebootTo -Index 2`(`[config]` の次の項目が 1)。
- 自動で起動するのは、Windows から再起動したときだけ。PC から payload を入れて起動したときは、今までどおりメニューが出る。
- 設定はレジストリ(`RebootToId`、`RebootToIndex`)にあり、ドライバーは 10 秒ごとに読み直す。ほかのアプリやメニューから変えてもよい(値の意味は `drivers\SwitchPowerButton\README-ja.md`)。

## 再起動・シャットダウンのしくみ

- Windows はシャットダウンを最後まで終えるが、最後のファームウェアの呼び出しが効かない。そこでドライバーが、シャットダウンの最後(デバイスの電源を切る段階。ファイルとレジストリの書き出しはもう終わっている)に、自分で処理する。
- **再起動:** hekate の payload を IRAM 0x40010000 に置いて、PMC でリセットする(reboot-to-payload。switchroot の bl31 と同じ手順)。
  - payload は、起動した SD の 1 番目の区画(hekate の FAT)の `bootloader\update.bin`(hekate の SD にある、hekate 自身のコピー)。ほかのファイルにするときは `Reset.ps1 -Mode Path -Payload \payload.bin` のようにする。Windows の起動後に読むだけで、SD には書かない。
  - 読めなかったときは PSCI SYSTEM_RESET を呼ぶ。hekate から起動していれば、bl31 が hekate に戻す。RCM(coreboot)から起動したときは、普通のリセットになる(payload が読めていれば、どちらの経路でも hekate に戻る)。
- **シャットダウン:** PMIC(MAX77620)に電源オフを命じる。電源が切れたあとの起動は今までどおり(Erista の AutoRCM なら RCM になるので、payload を入れる)。
- 処理の直前にバックライトを消す。うまくいかなかったときに、どこで止まったかの目印になる。

## うまくいかないとき

- **再起動で、バックライトが消えて止まる:** ドライバーは動いたが、リセットが効かなかった。長押しで切ってから起動し、`Restart-Psci.cmd` を実行して、もう一度試す。
- **再起動・シャットダウンで、バックライトがついたまま止まる:** ドライバーの処理まで来ていない。`Status.cmd` の結果を共有してほしい。
- **シャットダウンで、バックライトだけ消えて止まる:** PMIC への命令が届かなかった。
- 長押しでの電源断は、Windows のシャットダウンが終わったあとなので、ファイルは書き出し済み。
- **元に戻す:** `Reset-Disable.cmd`(前の止まる動きに戻るだけ)。
- **注意:** Mariko(新型)では R2P ができないので、ドライバーは自動で PSCI を使う。payload を読むときに Windows が FAT の区画をマウントするので、ファイルの最終アクセス日が更新されることはある。

## 結果

- 2026-10-25: 動作を「画面を消す」にして電源ボタンを押すと、画面が消えてロックされた。もう一度押すと、画面がついた。
- 2026-11-17(1.0.2.0、hekate から起動): 再起動で hekate に戻った。シャットダウンで電源が完全に切れた。RCM(coreboot)から起動した場合も、再起動で hekate に戻った。
- 2026-11-17(1.0.3.0): `RebootTo-Windows.cmd`(id=SWOAHKT)のあと再起動すると、hekate のメニューを通らずに Windows が起動した。
