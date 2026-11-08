# 画面 段階 0・1(試験)

## 段階 0: 今の状態を調べる(読むだけ)

`Report.cmd` を右クリックして「管理者として実行」。何もインストールしない。約 1 分。

見るもの:
- **画面のドライバー**: 「Microsoft Basic Display Adapter」(BasicDisplay)かどうか。デバイス ID。
- **モニター**: Windows が思っている画面の大きさ(cm)。今は EDID が無いので 0 か推測の値のはず。
- **今のモード**: 解像度、リフレッシュレート、向き(DMDO_*)、拡大率(DPI)。
- **powercfg /a**: 使えるスリープの種類と、使えない理由。「グラフィックス」の名前が出るかどうか(出たら SUMMARY に書く)。
- **dxdiag**: Direct3D が何で動いているか(WARP = CPU で描いている)。

最後の `SUMMARY` の画面を写真に撮る。記録は `C:\SwitchWOA-Display.txt` に追記される。

## 段階 1: 表示専用のドライバー switchdisplay(試験)

`switchdisplay.sys` は、Basic Display と同じように、coreboot が用意したフレームバッファー(0xdfb80000、720x1280)に描く。
違うところ:
- フレームバッファーを Write-Combine で使い、変わったところ(dirty rect)だけ書き写す。
- 回転(0/90/180/270)をドライバーの中で行う。
- パネルの本当のタイミング(78 MHz、60 Hz)と大きさ(77 x 137 mm)を EDID で Windows に伝える。
- 表示コントローラー(DC)のレジスターには書き込まない。読むのは起動時に 1 回だけ(記録用)。
- **UEFI R408 以降が要る。** DSDT の `DISP`(`ACPI\SWOA0E01`)に付く。1.0.0 と 1.0.1 はルートに置く仮のデバイスを使ったが、フレームバッファーのメモリーを割り当てられなかった(エラー 12)。`Install.cmd` はその古いデバイスと古いパッケージを消してから入れる。

まだやらないこと: 画面オフでパネルを止める(段階 2)、vsync(段階 3)。画面オフは今までどおり switchfan がバックライトを消す。

### 入れ方

1. テスト署名の準備が済んでいること(ほかのドライバーと同じ。`SwitchWOA-Test.cer`)。UEFI R408 で起動していること。
2. `Report.cmd` を一度実行して、結果を残しておく(比べるため)。
3. `Install.cmd` を右クリックして「管理者として実行」。
4. **Windows を再起動する。**
5. `Status.cmd` を管理者として実行して、SUMMARY を写真に撮る。

### 見てほしいこと

- 起動して画面が出るか。タッチ、回転(SwitchCenter の自動回転)、明るさ、画面オフ・オンが今までどおりか。
- `Status.cmd` の `framebuffer:` が「taken from Basic Display (POST)」になっているか。
- `presents:` の数が増え、`errors` が 0 か。
- `adapter:` に Basic Display と switchdisplay の両方が出ていないか(両方なら、どちらが動いているか)。
- 体感の速さ(スクロール、ウィンドウの移動)。

### うまくいかないとき

- **起動中に止まる・ブルースクリーンを繰り返す**: 2 回続けて 60 秒もたずに落ちると、3 回目の起動ではドライバーが自分で起動をやめ、Basic Display に戻る。そのあと `Remove.cmd` で外す。
- **画面が真っ暗のまま**: PC で SD の Windows のパーティションを開き、`Windows\System32\drivers\switchdisplay.sys` の名前を変える(例: `switchdisplay.sys.off`)。これで Basic Display に戻る。この操作は SD への書き込みなので、自分で行う。
- **外す**: `Remove.cmd` を管理者として実行し、再起動する。

### 記録

`HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters` に、起動の回数、POST の受け取りの結果、DC-A の値、回転、present の回数などを書く。`Status.cmd` が読んで表示する。
