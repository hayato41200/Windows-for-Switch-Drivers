# Wi-Fi 段階 4b：switchwlan（WiFiCx、Windows の Wi-Fi 一覧に出す）

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
- **対応しているコマンド:**

| コマンド | 0.5.0 での扱い |
|---|---|
| SET_RADIO_STATE、DOT11_RESET、DISCONNECT | M3 と M4 で、すぐに成功を返す |
| SCAN | M3 で成功を返す。ワーカーが escan を実行し、BSS_ENTRY_LIST（8 件ずつ）を送ってから、M4 の SCAN_COMPLETE を送る |
| CONNECT | まだ作っていない。M4 で失敗を返す |
| SET_ADAPTER_CONFIGURATION、ABORT_TASK | 成功を返す |
| それ以外 | STATUS_NOT_SUPPORTED（サンプルと同じ） |

- **BSS エントリーの TLV:** 自分で組み立てる（サンプルのテストデータと同じ形）。
  - BSSID（0x02）
  - ビーコンの本体（0x0A）。タイムスタンプは 0。ビーコン間隔、Capability、ファームウェアが返した IE（最大 768 バイト）を入れる。
  - 信号（0x0B）。RSSI と、RSSI から計算したリンク品質。
  - チャンネル（0x3A）。制御チャンネルとバンド ID。
- **データの送受信:** まだ作っていない。送信のパケットは送らずに返す。受信は何も上げない。
- **MAC アドレス:** 今はファームウェアの `cur_etheraddr`（Broadcom の既定の値）をそのまま使う。本体の MAC は段階 4c で設定する。
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

## 0.5.0 の結果（2026-11-07、ユーザー報告）

- **WiFiCx につながった。**
  - WifiDeviceInitialize と、能力の報告 4 つ（device、station、band、phy）が、すべて 0（成功）。
  - OS の WDI バージョンは 0x00020003。
  - ステーションのアダプター（type 1）の作成、初期化、開始も、すべて成功。
- **Windows がスキャンを頼んできた。** 3 回。1 回 1640 ms で、14 件を報告した。
- **`netsh wlan show interfaces`:** 「Nintendo Switch Wi-Fi (BCM4356)」が 1 つ。状態は disconnected、Radio は Hardware On / Software On。
- **`netsh wlan show networks`:** 15 個のネットワークが見えた。認証方式（WPA2-Personal / WPA3-Personal、CCMP）も、IE から正しく読まれている。
- **残り:** 接続、データの送受信、本体の MAC アドレス（今は 00:90:4c:1a:a0:01 で、Broadcom の既定の値）。
