# 再起動で hekate に戻す・シャットダウンで電源を切る

## 何をするか

- **これまで:** Windows はシャットダウンを最後まで終えるが、最後のリセット(ファームウェアの呼び出し)が効かない。画面は黒、バックライトはついたままで止まる。
- **このキット:** 電源ボタンのドライバー(`switchpwrbtn`)を version 2 にする。シャットダウンの最後(デバイスの電源を切る段階。ファイルとレジストリの書き出しはもう終わっている)で、ドライバーが自分でリセットや電源オフをする。
  - **再起動:** hekate の payload を IRAM 0x40010000 に置き、PMC でリセットする(reboot-to-payload。switchroot の bl31 と同じ手順)。→ hekate のメニューに戻る。
  - **シャットダウン:** PMIC(MAX77620)に電源オフを命じる。
- **payload:** 起動ディスク(SD)の 1 番目の区画(hekate の FAT)の `bootloader\update.bin`。Windows の起動後に読むだけで、SD には書かない。読めなければ、代わりに PSCI SYSTEM_RESET を呼ぶ(hekate から起動していれば、bl31 が hekate に戻す)。
- 電源ボタンの動き(押すと画面を消す等)は今までと同じ。

## 中身

| ファイル | 内容 |
|---|---|
| `switchpwrbtn.sys/.inf/.cat` | ドライバー version 2(1.0.2.0、テスト署名)。ソースは `drivers\SwitchPowerButton\src\switchpwrbtn.c` |
| `Install.cmd` | payload の場所を設定してドライバーを入れる |
| `Status.cmd` | 状態の表示(読むだけ) |
| `Disable.cmd` / `Enable.cmd` | ドライバーのリセットを止める / 戻す |
| `Restart-Psci.cmd` / `Restart-Auto.cmd` | 再起動のやり方を PSCI(bl31 任せ)にする / 標準に戻す |
| `Reset.ps1` | 上の .cmd の本体。ログは `C:\SwitchWOA-Reset.txt` |

## 手順

1. このフォルダーを Switch の Windows にコピーする(例: `C:\Switch-Reset`)。
2. `Install.cmd` を右クリック → **管理者として実行**。最後に表示される行を見る。
   - `driver version: 2 OK`
   - `payload: status=0x00000000 (loaded) size=…`
   - `restart will use: R2P from Windows …`
   - payload が `file not found` なら、SD の 1 番目の区画に `bootloader\update.bin` がない。その場合も PSCI で試せる(hekate の経路なら戻るはず)。
3. スタート → 電源 → **再起動**。見ること:
   - **hekate に戻る** → 成功。
   - **バックライトが消えて、止まる** → ドライバーは動いたが、リセットが効かなかった。電源ボタン長押しで切り、もう一度起動して `Restart-Psci.cmd` を実行してから、また再起動を試す。
   - **バックライトがついたまま止まる** → ドライバーの処理まで来ていない。`Status.cmd` の結果を見せてほしい。
4. 再起動がうまくいったら、スタート → 電源 → **シャットダウン**。
   - **電源が切れる**(バックライトも消える)→ 成功。
   - **バックライトだけ消えて止まる** → PMIC への命令が届かなかった。長押しで切る。
5. 結果(どれになったか)と、`Status.cmd` の出力を共有してほしい。

## 元に戻すには

- `Disable.cmd` を管理者として実行(前と同じ、止まる状態に戻るだけ)。
- ドライバーを前の版に戻すなら、`kits\Switch-PowerButton` の手順で入れ直す。

## 注意

- 長押しでの電源断は、Windows のシャットダウンが終わったあとなので、ファイルは書き出し済み。それでも何度も続けるのは避けたい。
- payload を読むとき、Windows が FAT の区画をマウントする(読むだけ。ファイルの最終アクセス日が更新されることはある)。
- Mariko(新型)では R2P ができないので、ドライバーは自動で PSCI に切り替える(このキットは Erista 向け)。

## 結果(2026-11-17、ユーザー報告)

- **再起動: 成功。** hekate の経路で、Windows の再起動から hekate に戻った(Windows から R2P、payload は bootloader/update.bin)。
- **シャットダウン: 成功。** 電源が完全に切れた。そのあと電源ボタンを押すと RCM で認識される(冷えた状態からの起動は今までどおり RCM で、payload を入れる)。
