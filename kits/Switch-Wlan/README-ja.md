# Wi-Fi 段階 4e：switchwlan（WPA2鍵制御とEthernetデータ経路）

## 中身

| ファイル | 内容 |
|---|---|
| `switchwlan.sys` | WiFiCx のクライアントドライバー（ARM64、テスト署名）。ソースは `..\..\drivers\SwitchWlan\src\`（`wlan.cpp` と `bcm.c`） |
| `switchwlan.inf` | `ACPI\SWOA0D01`。クラスは Net（Native 802.11） |
| `Wlan.ps1` | Install、Status、Retry、Remove |
| `Install.cmd` など | `Wlan.ps1` を各モードで動かすだけのもの。右クリックして「管理者として実行」 |

ファームウェア（`brcmfmac4356-pcie.bin`、あれば `.txt`）は同梱しない。自分で用意して、このフォルダーに置く。

## 必要なもの

- Windows 11 22H2 以降（WiFiCx 1.0）
- UEFI R405 以降（DSDT の WLN0）

## ドライバーの作り

- **全体:** WDK の WiFiCx サンプル（`network/wlan/wificx`）の流れに合わせた。
  - **初期化:** EvtDeviceAdd で `NetDeviceInitConfig`、`WifiDeviceInitConfig`、`WdfDeviceCreate`、`WifiDeviceInitialize` を順に呼ぶ。
  - **能力:** EvtDevicePrepareHardware でチップを起動したあと、`WifiDeviceSet{Device,Station,Band,Phy}Capabilities` で知らせる。
  - **アダプター:** EvtWifiDeviceCreateAdapter で NETADAPTER（ステーション）を作る。
  - **コマンド:** WDI のメッセージとして EvtWifiDeviceSendCommand に来る。
- **ビルド:** WiFiCx のヘッダーが C++ なので、WiFiCx とつなぐ部分は C++（`wlan.cpp`）で書いた。チップの処理（`bcm.c`）は、switchwifi 0.4.0 の C のコードをそのまま使っている。
  - **使うもの:** WDK 10.0.26100 の WiFiCx 1.0、NetAdapterCx 2.2、KMDF 1.33。
  - **clang の対策:** clang は wdfdevice.h の C++ 前方宣言（`enum _WDF_REQUEST_TYPE : int;`）を受け付けない。そのため Build-Driver.ps1 が、その行を消した KMDF ヘッダーのコピーを `build\_kmdfinc` に作る。
- **スレッド:** チップの処理は待ち時間（sleep）を含むので、ワーカースレッドで動かす。コマンドの受け取り側は、やることを記録するだけ。割り込みはまだ使わず、ワーカーが 20 ms ごとに完了リングを見る。
- **対応しているコマンド（ソース 0.6.2）:**

| コマンド | 0.6.2 での扱い |
|---|---|
| SET_RADIO_STATE | M3 と M4 で成功を返す |
| DOT11_RESET、DISCONNECT | M3 を返し、ワーカーで切断処理後に M4 を返す |
| SCAN | M3 で成功を返す。ワーカーが escan を実行し、BSS_ENTRY_LIST（8 件ずつ）を送ってから、M4 の SCAN_COMPLETE を送る |
| CONNECT | SSID、優先 BSSID、チャンネルを検証し、Open または RSNA-PSK の BCM Join を開始。リンク結果をポーリングして M4 を返す |
| SET_ADD_CIPHER_KEYS / SET_DELETE_CIPHER_KEYS | RSNA-PSK + CCMP の鍵情報を検証し、最大4鍵をpassive workerからBCM鍵APIへ渡す |
| SET_ADAPTER_CONFIGURATION、ABORT_TASK | 成功を返す |
| それ以外 | STATUS_NOT_SUPPORTED（サンプルと同じ） |

- **BSS エントリーの TLV:** 自分で組み立てる（サンプルのテストデータと同じ形）。
  - BSSID（0x02）
  - ビーコンの本体（0x0A）。タイムスタンプは 0。ビーコン間隔、Capability、ファームウェアが返した IE（最大 768 バイト）を入れる。
  - 信号（0x0B）。RSSI と、RSSI から計算したリンク品質。
  - チャンネル（0x3A）。制御チャンネルとバンド ID。
- **データの送受信:** NetAdapterCx の Ethernet Tx/Rx ring と BCM msgbuf の間に、最大32フレームの bounded copy queue を追加した。BCM 層の `BcmTxFrame` / `BcmSetRxHandler` を使う。NetAdapterCx callback 側ではフレームをコピーし、BCM のポーリングや待機は passive worker に限定する。これはソース実装とホストビルドの確認であり、実機でのIP疎通確認ではない。
- **MAC アドレス:** アダプターは BCM 層から読み取った firmware MAC を permanent/current address として通知する。Windows が指定するMACの反映や実機値の一致はまだ確認していない。
- **記録:** `Services\switchwlan\Parameters` に書く。WlanResult、WlanCaps*、WlanAdapter*、WlanScan*、ScanList のほか、switchwifi と同じ Fw*、Msg* など。

## 手順

1. **Switch を R405 で起動する。**
2. **このフォルダーを Switch に移し、ファームウェアのファイルを入れる。**
3. **`Install.cmd` を右クリックして「管理者として実行」する。**
   - 古い switchwifi を外してから、switchwlan を入れる。
   - 15 秒待ってから、結果を表示する。
4. **見るところ:**
   - タスクバーの Wi-Fi のボタンと、その一覧。
   - `netsh wlan show networks` の結果（Status にも出る）。

## 0.5.0 の実機結果（2026-11-07、ユーザー報告）

- **WiFiCx につながった。**
  - WifiDeviceInitialize と、能力の報告 4 つ（device、station、band、phy）が、すべて 0（成功）。
  - OS の WDI バージョンは 0x00020003。
  - ステーションのアダプター（type 1）の作成、初期化、開始も、すべて成功。
- **Windows がスキャンを頼んできた。** 3 回。1 回 1640 ms で、14 件を報告した。
- **`netsh wlan show interfaces`:** 「Nintendo Switch Wi-Fi (BCM4356)」が 1 つ。状態は disconnected、Radio は Hardware On / Software On。
- **`netsh wlan show networks`:** 15 個のネットワークが見えた。認証方式（WPA2-Personal / WPA3-Personal、CCMP）も、IE から正しく読まれている。
- **残り:** 0.6.0 の接続コントロールはホストビルドのみで実機未確認。ここに記す新しい接続・鍵・送受信機能も実機では未確認（既定値の MAC は成果物に記載しない）。

## 0.6.2 の状態

- `WDI_TASK_CONNECT` を受けると SSID と Windows が選んだ BSSID/チャンネルを検証し、BCM 層の Join をワーカーから開始する。
- RSNA-PSK + CCMP の鍵追加/削除コマンド、Open/RSNA-PSK 接続指定、Ethernet Tx/Rx のリングコピー経路を実装した。WPA2 four-way handshake、DHCP、ping/Web は実機未確認で、まだ動作を保証しない。
- 送受信キューの枯渇、再接続、鍵削除、DHCP/IP疎通を含む実機検証が必要。
- `DISCONNECT` と `DOT11_RESET` はワーカー上でデータリング停止と BCM Leave を行う。
- このソースは `build\SwitchWlan-0.6.2-candidate` にARM64テスト署名付きでビルドする。実機に導入する前にテスト証明書の信頼登録とテスト署名設定が必要。これはソース・ビルド成果物の説明であり、Switch上の動作成功を示さない。

## 手元専用キット（-private.zip）と Wlan.ps1（0.6.x）

- **本体の MAC:** キットのフォルダーに `wifi-mac.txt`（1 行、`xx:xx:xx:xx:xx:xx`）があると、Install が `Services\switchwlan\Parameters\MacAddress` に書いてからドライバーを入れる。ドライバーは起動時にこれを `cur_etheraddr` で設定する。ファイルがなければ、ファームウェアの既定のアドレス（Broadcom の 00:90:4C:…）のまま。このファイルとファームウェアは手元専用キットにだけ入れる（リポジトリには入れない、配らない）。
- **Status の表示:** 0.5.0 の表示に加えて、次を出す。
  - `mac`：アドレスの出どころ（0 ファームウェア、1 MacAddress、2 Windows）と、先頭 3 バイトだけ。
  - `join` / `assoc`：Join の結果、リンクの状態（2 = つながった、3 = 失敗、4 = 切れた）、最後のイベント、IE の長さ、鍵。
  - `flow` / `tx` / `rx`：flowring の状態と、送受信の数（EAPOL を含む）。
  - `events`：ファームウェアの接続イベントの最近 16 件（種類/状態/理由/フラグ）。
  - Wi-Fi の IP 設定と、ゲートウェイと 1.1.1.1 への ping。

## 0.6.8 の実機結果（2026-11-08、ユーザー報告）

- Windows の Wi-Fi 一覧から WPA2-Personal の AP（5 GHz、ch 100）に接続できた。DHCP で IPv4 を取得し、ゲートウェイと 1.1.1.1 への ping が通った（約 27 ms）。
- ここまでに直したこと：
  - 0.6.4：Tx/Rx キューの Cancel で、預かったパケットとフラグメントをすべて返す（返さないとデータ経路が止まらない）。
  - 0.6.5：CONNECT_COMPLETE の前に ASSOCIATION_RESULT と LINK_STATE_CHANGE を送る。
  - 0.6.8：WiFiCx のデータ経路は 802.11 形式。受信は Ethernet から 802.11 データフレーム（FromDS、LLC/SNAP 付き）に組み直し、送信は 802.11 から Ethernet に戻してファームウェアに渡す。
  - Install：入れ替えの前に wlansvc を止める（止めないと pnputil が query-remove の時間切れ、exit 480 になる）。
- 診断用：`Trace-Start.cmd` / `Trace-Stop.cmd` で `C:\SwitchWOA-wlan.etl` を記録できる（`netsh trace`）。PC で `netsh trace convert input=... dump=txt` すると読める。
- 残り：本体の MAC の設定（cur_etheraddr を読み返すと変わっていない）、接続中のスキャンで見えるネットワークが 1 つだけになる、初回の接続が時間切れになることがある。

## 0.6.9 の実機結果（2026-11-08、ユーザー報告）

- fast.com で 11 Mbps（0.6.8）→ 36 Mbps（0.6.9）。
- 0.6.8 までは 20 ms（実際は約 31 ms）ごとのポーリングで、1 回に 32 フレームまでしか Windows に渡せず、約 12 Mbps が上限だった。0.6.9 ではキューを 256 フレームにし、通信中は 1 ms ごとにポーリングする（約 0.2 秒通信がなければ 20 ms に戻す）。
- さらに速くするなら、割り込み（MSI）での受信、コピーの削減が次の候補。
