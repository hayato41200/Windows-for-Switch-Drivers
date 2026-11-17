# 画面: switchdisplay(試験)

`switchdisplay.sys` は、Windows 標準の Basic Display の代わりに、内蔵パネル(720x1280)を表示する WDDM の表示専用ドライバーです。

- **UEFI R408 以降が要る。** DSDT の `DISP`(`ACPI\SWOA0E01`)に付く。R405 以前で起動すると読み込まれず、Basic Display に戻る。
- coreboot が用意したフレームバッファー(0xDFB80000)を Basic Display から引き継いで描く。
- 変わったところ(dirty rect)だけを書き写す。回転(0/90/180/270)はドライバーの中で行う。Windows の設定と SwitchCenter の自動回転の両方で回せる。
- 画面オフ(電源ボタン、自動の画面オフ)のときは、パネルをスリープさせ、表示コントローラーの走査も止める。画面オンで元に戻す。
- vsync(画面の書き換えのタイミング)は Windows のタイマーで動く。表示コントローラーの割り込みを使う処理(1.2.0 から)は入っているが、DSDT に割り込みを足した UEFI R409 では Windows が画面のデバイスを止めてしまう(エラー 43)ため、使っていない。**R409 では起動しないこと。**
- 描画は今までどおり CPU で行う(GPU による高速化は無い)。速さは Basic Display と同じ。
- EDID は渡していない。拡大率は「設定 → ディスプレイ → 拡大/縮小」で選ぶ。

## 中身

| ファイル | 内容 |
|---|---|
| `Report.cmd` | 読むだけ。画面のドライバー、今のモードと回転、DPI、`powercfg /a`、dxdiag |
| `Install.cmd` | switchdisplay を入れる(古い試験版のデバイスとパッケージを消してから) |
| `Status.cmd` | 画面のドライバーと、switchdisplay の記録 |
| `RotateTest.cmd` | SwitchCenter と同じ方法で 90° / 180° / 270° / 元に戻す、と回して、結果を出す |
| `DcStop-Off.cmd` / `DcStop-On.cmd` | 画面オフで表示コントローラーを止める処理を、切る / 戻す(パネルのスリープは残る) |
| `Remove.cmd` | switchdisplay を外す(再起動すると Basic Display に戻る) |

どれも右クリックして「管理者として実行」。最後の `SUMMARY` を写真に撮る。記録は `C:\SwitchWOA-Display.txt` に追記される。

## 入れ方

1. テスト署名の準備が済んでいること(ほかのドライバーと同じ。`SwitchWOA-Test.cer`)。
2. UEFI R408 で一度起動する(`DISP` を Windows に見せるため)。
3. `Install.cmd` を管理者として実行する。R405 で起動しているときは、パッケージを置くだけで、次に R408 で起動したときに入る。
4. Windows を再起動する。
5. `Status.cmd` で、`framebuffer: taken from Basic Display (POST)` と `DDI failures: none` を確かめる。

## うまくいかないとき

- **起動中に止まる・ブルースクリーンを繰り返す**: 2 回続けて 60 秒もたずに落ちると、3 回目の起動ではドライバーが自分で起動をやめ、Basic Display に戻る。
- **画面が真っ黒のまま、または画面オフから戻らない**: 電源ボタンを長押しして切り、**UEFI R405 で起動する**(ドライバーは読み込まれない)。そのあと `Status.cmd` の写真を撮り、画面オフから戻らなかったときは `DcStop-Off.cmd` を実行してから R408 に戻す。
- **外す**: `Remove.cmd` を実行して再起動する。

## 記録

`HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters` に、版、起動の回数、POST の受け取りの結果、回転、present の回数、画面オフの回数、Windows からの呼び出しごとの回数と失敗を書く。`Status.cmd` が読んで表示する。

設定(どれも DWORD。変えたら再起動):
- `PanelPower` = 0: 画面オフでパネルをスリープさせない。
- `DcStop` = 0: 画面オフで表示コントローラーを止めない。
- `Vsync` = 0: 割り込みを使わない(Windows のタイマーで動く)。
- `UseEdid` = 1: EDID(パネルの大きさ 77 x 137 mm)を渡す。Windows がおすすめの拡大率を上げることがある。
