# ESP32 AMBE3000(DV3000)互換 — Wi-Fi サーバ版(BlueDV 対応)

DV3000 プロトコル(`0x61 | len | type | payload`)を Wi-Fi 経由の **UDP と TCP(同じポート、既定 2460)** で提供します。
BlueDV の「AMBE サーバ」接続(UDP)でそのまま使えます。
PC とは COM ポート(UART)で接続し、そこから **SSID / パスワード / IP アドレス(DHCP または固定)/ ポート** を設定します。設定はフラッシュ(NVS)に保存されます。
コーデックは mbelib-neo(D-STAR AMBE 3600x2400 のみ)です。

このフォルダーは**単体でビルドできる完全なプロジェクト**です(元のシリアル版プロジェクトへのコピーは不要です)。

## ファイル

| 区分 | ファイル |
|---|---|
| プロジェクト | `CMakeLists.txt` `partitions.csv` `sdkconfig.defaults*` `idf_env.bat`(ビルド用) |
| ファームウェア | `main/` 一式 |
| ポータブル(PC でも試験可) | `app_config.h` `cfg_parse.c/.h` `cli_core.c/.h` `tcp_server.c/.h` `dv3000_protocol.c/.h` |
| ESP-IDF 専用 | `app_config_nvs.c` `wifi_net.c/.h` `cli_uart.c/.h` `ambe_codec.c/.h` `main.c` |
| コーデック | `components/mbelib_neo/`(mbelib-neo 2.2.0、GPL-2.0-or-later、`LICENSE-NOTICE.txt` 参照) |
| 試験 | `tools/host_test/`(設定コンソールの PC 上の単体試験) |

## ビルドと書き込み(Windows / ESP-IDF 5.1.6)

PowerShell の実行ポリシーで `export.ps1` が動かない環境でも使えるよう、`idf_env.bat` が ESP-IDF の環境を読み込んでから `idf.py` を実行します。

```
.\idf_env.bat build
.\idf_env.bat -p COM12 flash
```

* `idf_env.bat` の中の ESP-IDF の場所(`C:\Users\user\esp516\v5.1.6\esp-idf`)と Python 環境(`idf5.1_py3.11_env`)は、PC に合わせて書き換えてください。
* COM ポート番号はデバイスマネージャーで確認してください。書き込みモードへの切り替えは自動です(ボタン操作は不要)。
* **書き込みの前に Tera Term などのターミナルを切断してください**(COM ポートを占有していると書き込めません)。
* 初回、またはターゲットを変える場合:
  ```
  .\idf_env.bat -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wifi" set-target esp32
  ```
* `main/Kconfig.projbuild` を変更したときは `.\idf_env.bat reconfigure` を先に実行してください(実行しないと新しい設定項目が `sdkconfig` に入りません)。

### ESP32-S3 の場合
ESP32 (無印) とは別のフォルダー(`build_s3`)・別の設定ファイル(`sdkconfig.s3`)でビルドします。両方を並べて使い分けられます。

```
.\idf_env.bat -B build_s3 -D SDKCONFIG=sdkconfig.s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wifi" set-target esp32s3
.\idf_env.bat -B build_s3 -D SDKCONFIG=sdkconfig.s3 -p COM5 flash
```
(1 行目は初回だけ。COM 番号は環境に合わせてください)

* ESP32 無印のボードを COM ポートに書き込もうとすると `This chip is ESP32-S3, not ESP32` などのエラーで止まります(書き込みは行われません)。チップに合ったビルドを使ってください。
* **設定コンソールは、内蔵 USB(USB Serial/JTAG)の COM ポートに出ます**(差し込み口が 1 つだけの小型ボード向け。menuconfig の「Setup console port」で UART にも変更可)。内蔵 USB では通信速度の設定は関係ありません。起動ログも同じポートに出ます。
* `apply` / `reboot` で再起動すると USB がいったん切断されるので、ターミナルを COM ポートにつなぎ直してください。
* 書き込みファイルとアドレス(**ブートローダーが `0x0`**、無印は `0x1000`):

  | ファイル | アドレス |
  |---|---|
  | `build_s3/bootloader/bootloader.bin` | `0x0` |
  | `build_s3/partition_table/partition-table.bin` | `0x8000` |
  | `build_s3/esp32_ambe3000_compat.bin` | `0x10000` |

### 書き込みファイル(ESP32 無印)
| ファイル | アドレス |
|---|---|
| `build/bootloader/bootloader.bin` | `0x1000` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/esp32_ambe3000_compat.bin` | `0x10000` |

書き込み設定:DIO / 40 MHz / 4 MB。ESP32 は USB メモリとしては見えないので、エクスプローラーでのコピーでは書き込めません(`idf_env.bat` の `flash`、esptool、Flash Download Tool、ブラウザの Web 書き込みツールなどを使います)。

## 設定コンソール(COM ポート)

ESP32 無印は UART(USB-UART ブリッジ)、ESP32-S3 は内蔵 USB の COM ポートです。
UART の場合は **115200 bps / 8N1 / フロー制御なし**。Tera Term・PuTTY 等で開き、Enter(CR)を送ります。入力は ASCII のみです。
`help` で一覧が出ます。初回は `setup`(対話ウィザード)が簡単です。

* 文字化けする場合は、ターミナルの**漢字コード(文字コード)設定**を見直してください。Tera Term は「設定」→「設定の保存」で保存しないと、次回元に戻ります。
* 起動ログの途中に `ambe>` が出ることがありますが、Enter を押せば新しいプロンプトが出ます。

```
ambe> setup
--- Setup wizard (Enter = keep the value in [ ], Ctrl-C = cancel) ---
Wi-Fi SSID [(none)]: MyHome-2G
Wi-Fi password [keep] (8-64 chars, '-' = open network): ********
Get the IP address by DHCP? (y/n) [y]: y
TCP port [2460]:
Save and reboot now? (y/n) [n]: y
```

| コマンド | 内容 |
|---|---|
| `setup` | 対話ウィザード(Enter=現在値のまま、Ctrl-C=中止) |
| `show` / `status` | 設定の表示 / 接続状態(IP・RSSI・最後のクライアント・送受信バイト数) |
| `scan` | アクセスポイント一覧(未接続のときも使えます) |
| `wifi ssid <名前>` / `wifi pass <パスワード>` | スペースを含む場合は `"..."`。パスワードは 8〜64 文字、`""` で暗号化なし |
| `ip dhcp` / `ip static <ip> <mask> <gw> [dns]` | 例: `ip static 192.168.0.100 /24 192.168.0.1` |
| `port <1-65535>` / `hostname <名前>` | ポート(既定 2460、UDP/TCP 共通) / ホスト名 |
| `save` / `apply` / `reboot [force]` | 保存 / 保存して再起動 / 再起動(未保存だと拒否、`force` で破棄) |
| `factory-reset` | 設定を全消去(`YES` と入力して確認) |

* 変更は RAM 上で行われ、`save` で保存、**再起動後に反映**されます。
* ESP32 は **2.4 GHz 帯の SSID にしか接続できません**(5 GHz の SSID は `scan` に出ません)。
* 固定 IP は入力時に検証されます(ネットマスクの形式、ゲートウェイが同一サブネット、ネットワーク/ブロードキャストアドレスでないこと等)。ゲートウェイ `0.0.0.0` は「なし」(LAN 内専用)の意味です。
* IP を固定したい場合は、ルーター側の DHCP 予約(ESP32 の MAC アドレスは `status` で表示)も確実です。
* 設定を間違えてネットワークに繋がらなくなっても、COM ポートのコンソールは常に使えるので、`ip dhcp` や `factory-reset` で復旧できます。

## ネットワーク(UDP / TCP)

* 既定ポート **2460**(`port` で変更)。**UDP と TCP の両方**で待ち受けます。
* **UDP**(BlueDV の AMBE サーバ方式):1 データグラム = 1 パケット。応答は要求の送信元に返します。
* **TCP**:シリアル接続と同じバイトストリーム(パケットが複数・分割されて届いてもかまいません)。同時接続は 1 つで、新しい接続が来ると古い接続は切断されます。応答を読まなくなったクライアントは 2 秒で切断、無通信でも TCP keep-alive で消えたクライアントを検出します。
* コーデックは 1 つなので、**同時に使うクライアントは 1 つ**にしてください。
* 対応コマンド:PRODID / VERSTRING / RESET / RESETSOFTCFG / RATEP(D-STAR 2400+1200 FEC のみ)/ **GAIN(0x4B)** / AUDIO↔AMBE 9 バイト。DMR・P25・NXDN 等は未対応です。
* 認証・暗号化はありません。**信頼できる LAN 内だけで使い、インターネットに公開しないでください。**
* Nagle アルゴリズムは無効(`TCP_NODELAY`)、Wi-Fi の省電力モードも無効にしてあります。

### BlueDV の設定
1. 「AMBE」メニューで AMBE サーバの接続先を **ESP32 の IP アドレス**、ポート **2460** にします。
2. 接続すると、BlueDV の画面の「ファームウェア」欄に **AMBE3000R** と表示されます。
3. ESP32 側では `status` の `Client:` に PC の IP が表示され、`rx` / `tx` が増えます。

## 音声処理(送信側)

符号化(音声→AMBE)の前に次の順で処理します。

1. **入力ゲイン**:BlueDV が送る GAIN コマンド(例 `4B 05 05` = 入力 +5 dB / 出力 +5 dB)を反映します。
2. **150 Hz ハイパスフィルター**:電源ハムや低域の雑音を取り除きます。
3. **ノイズゲートの判定**(menuconfig の「Noise gate in front of the AMBE encoder」、既定で有効):**雑音抑圧の前の信号**で、背景雑音より 6 dB 以上大きいかを判定します(雑音抑圧の後は雑音が大きく上下して判定が狂うため)。200 ms 保持して語尾を残します。
4. **雑音抑圧(NR、`main/noise_reduce.c`)**:周波数ごとに背景雑音を追従して差し引きます(最大 -25 dB、雑音を 1.5 倍に見積もる)。抑圧量は隣の周波数とならします(ミュージカルノイズ対策)。遅れが 20 ms 増えます。
5. **高域強調**:1.5 kHz 以上を +6 dB(こもったマイク向け)。
6. **ゲート適用**:3 の判定が「閉」なら、符号化器を通さずに**すぐ** D-STAR 標準の無音フレームを送ります。
7. **固定ゲイン +6 dB(ピーク リミッター付き)**:符号化器内部の AGC は**止めて**あります(言葉の間の雑音まで声と同じ大きさに持ち上げ、「声の前に音が聞こえる」原因になっていたため)。ゲインは Peanut での聞き比べで決めました(音質は 0 dB > +6 dB > +12 dB。+12 dB は過入力で音質が落ちる。+6 dB でほかの局とほぼ同じ音量)。
8. **符号化器(mbelib-neo、改造あり)**:
   * 有声とする帯域の上限を **1.5 kHz(周期性が中くらいなら 1 kHz)**にし、それより上は無声(息の音)で合成。元は 3.75 kHz までほぼ全部有声で、金属的なブザー音・うなりの原因だった(本物の AMBE チップは有声フレームでも倍音の約半分を無声にしている)。
   * **周期性 0.35 未満のフレームは全帯域を無声**にする(大きな摩擦音やかすれ声を、でたらめなピッチのブザー音にしない=「ケロ」対策)。
   * **ピッチのオクターブ下誤りの修正**:周期性 0.7 以上のとき、半分の周期でも同じくらい周期的なら(0.95 倍以上)選び直す。きれいな音声で、ピッチの正解率(±10%)が 75% → 88%。
   * 倍音の強さを山の周り 3 ビンで測る/有声判定の基準 0.5・0.3(元は 0.6・0.4)/スペクトルの時間平滑化 0.5(話し始めや 6 dB 以上の変化では平滑化しない)。

**音源の品質について**:PC のマイク経路の雑音(電源のハム、PC 内部の電子ノイズ)が「ギューギュー」の主な原因でした。雑音のない音源(iPhone 録音、S/N 45 dB)では元の設定でもギュー音は出ません。**送信時は PC をバッテリー駆動にする**(アースループ由来の雑音が減り、S/N が約 5 dB 改善)、できれば **USB ヘッドセット**を使うのがおすすめです。

### 調整用コマンド(上級者向け)
UDP/TCP で `61 00 LL 00 F0 ...` を送ると、上記の各処理を再起動なしで切り替えられます(BlueDV は使いません。設定は再起動で既定値に戻ります)。

```
F0 <ピッチ追従の周期性しきい値 x100> <ピッチ変化制限 x100> <連続性の重み x100>
   <フラグ: 1=HPF 2=ゲート 4=フレーム情報(F1)を返す 8=NR>
   [<有声判定ヒステリシス 0/1> [<NR 下限 -dB> <NR 雑音見積り x10>
   [<AGC 平滑 x100, 100=AGC 固定> [<帯域の重なり修正 0/1> [<スペクトル平滑 x100>]]]]]
既定値: F0 00 19 50 0B 00 19 0F 64 00 32   (= 0, 25, 80, HPF+ゲート+NR, 0, -25 dB, 1.5, AGC 固定, 0, 0.50)
```
個別の項目は `61 00 04 00 F2 <id> <値 int16 ビッグエンディアン>` で変えられます:

| id | 項目 | 既定 |
|---|---|---|
| 1 | 倍音の強さの測り方(0=帯域全体 / 1=山の周り 3 ビン) | 1 |
| 2 / 3 | 有声判定の基準 上 / 下 (x100) | 50 / 30 |
| 4 | 高域強調 dB | 6 |
| 5 | NR の周波数方向の平滑化 0/1 | 1 |
| 6 | スペクトルの時間平滑化 (x100) | 50 |
| 7 | 符号化前の固定ゲイン dB | 6 |
| 8 | ゲートの基準 (雑音の何倍, x10) | 20 |
| 9 | 符号化器 AGC の平滑 (x100、100=停止) | 100 |
| 10 | ゲートの保持フレーム数 | 10 |
| 11 | ゲートが閉じたら無音フレームを即送信 0/1 | 1 |
| 12 | 復号ごとにパラメーター(F3)を返す 0/1(解析用) | 0 |
| 13 / 14 | 有声の上限(500 Hz 単位、7=3.75 kHz)周期性 強 / 中 | 2 / 1 |
| 15 | 上限より上を強制的に無声 0/1 | 1 |
| 16 | オクターブ修正の基準 (x100、0=なし) | 95 |
| 17 | この周期性 (x100) 未満は全帯域無声 | 35 |
| 18 | オクターブ修正を行う最小の周期性 (x100) | 70 |

検証の結果、ピッチ追従の変更・有声判定のヒステリシス・帯域の重なり修正は異音に効果がなかったため、既定では無効(元の動作)です。ゲートの保持は 5 や 3 フレームも試しましたが、声が聞き取りにくくなったため 10 にしています。

**マイク入力の質が音質を大きく左右します。** mbelib-neo の符号化器には本物の AMBE チップのような雑音抑圧がないため、声と雑音の差(S/N)が小さいと話している最中に異音が出ます。
* 雑音のない合成音声では異音は出ません(符号化器そのものは正常です)。
* 目安:話したときのピークが -10〜-6 dBFS、S/N 30 dB 以上。マイクを口元に近づける(ヘッドセット等)のが最も効果的です。
* マイクブーストは雑音も増えるので上げすぎないでください。USB ハブや電源の近くはハムを拾いやすいです。

## トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| 起動直後に `assert failed: esp_ipc_init` で再起動を繰り返す | mbelib-neo のスレッドローカル変数(約 52 KB)が全タスクのスタックに確保されるため。`components/mbelib_neo/CMakeLists.txt` の `MBE_THREAD_LOCAL=` と、`vendor/.../src/internal/mbe_compiler.h` のガード 1 行で対処済み。**`tools/get_mbelib.sh` で mbelib を取り直すとこの 1 行が消える**ので入れ直してください。 |
| `wifi: disconnected (reason 201)` | 指定した SSID が見つからない。`scan` で一覧を確認(5 GHz の SSID・ステルス SSID・綴り違い)。 |
| `scan` で `ESP_ERR_WIFI_STATE` | 旧版の不具合(接続リトライ中はスキャン不可)。現版では修正済み。 |
| BlueDV が接続できない(Wireshark で ICMP Port unreachable) | 旧版は TCP のみだった。現版は UDP にも対応。 |
| BlueDV で DSTAR を有効にすると `IndexOutOfRangeException`(`parseREFHostFile` / `parseDCSHostFile`) | BlueDV の D-STAR ホスト一覧のダウンロード先(arrg.us)が一覧の代わりに Web ページを返すため、`Documents\BlueDV\DPlus_Hosts.txt` 等が HTML になっている。Pi-Star の一覧(`http://www.pistar.uk/downloads/DPlus_Hosts.txt` 等、コメント行を除いた「名前<TAB>IP」)に置き換え、BlueDV の DSTAR ホスト更新は実行しない。 |
| BlueDV の起動時にインストーラー(修復)が動いてエラー | ホスト一覧ファイルの名前を変える・消すと、MSI の修復が走る。ファイルを元の名前で置いておき、`C:\Program Files (x86)\BlueDV\BlueDVWindows\BlueDV.exe` を直接起動する。 |
| 送信時に BlueDV が `NullReferenceException`(`DVMEGASerial.procesMMDVM`) | BlueDV 側の不具合。BlueDV の再起動で解消した例あり。 |
| 送信音声にギューギューという異音 | マイク入力の S/N 不足。上の「音声処理」を参照。 |
| 書き込みで `Detected crystal freq 41.01MHz ...` の警告 | 互換ボードでよくある表示で、書き込みは成功しています。 |

## このフォルダーで行った変更(元のシリアル版からの差分)

* `main/CMakeLists.txt`:ESP-IDF は `REQUIRES` を `sdkconfig` の読み込み前に解決するため、Wi-Fi 関連コンポーネントを条件なしで指定(`nvs.h: No such file` の対策)。
* `main/main.c`:`CONFIG_AMBE_BOOT_DIAG` 有効時に IDF 5.1 でもビルドできるよう `esp_rom_uart_putc` を使い分け。
* `main/cli_core.c`:`help` の表示が途中で切れる不具合を修正。
* `main/wifi_net.c`:未接続時の `scan` で再接続を一時停止。
* `main/tcp_server.c`:UDP 対応。
* `main/dv3000_protocol.c`:GAIN コマンド、150 Hz ハイパスフィルター、雑音抑圧の呼び出し、ノイズゲート、調整用コマンド(F0/F1)。
* `main/noise_reduce.c/.h`:雑音抑圧(新規)。
* mbelib-neo:スレッドローカル変数を無効化(上記)。`src/ambe/ambe3600x2400_enc.c` に調整用の変数とスペクトル平滑化を追加(`tools/get_mbelib.sh` で取り直すと消えます)。

## 試験

PC 上(ESP-IDF 不要): 設定コンソールと入力検証の単体試験
```
make -C tools/host_test -f Makefile.cli test
```

## 検証状況
* 実機(ESP32 rev 3.1、IDF 5.1.6)で確認済み:ビルド、起動、設定コンソール、Wi-Fi 接続(DHCP)、`scan`、UDP/TCP での PRODID・VERSTRING・GAIN、AMBE↔音声の変換(1 フレームあたり UDP で約 6〜8 ms、TCP で約 8〜11 ms、Wi-Fi の往復込み)、BlueDV での受信(復号)と送信(符号化)。
* 合成音声(雑音なし)の符号化→復号で異音がないことを確認。
* ESP32-S3(QFN56, rev 0.2)で確認済み:ビルド、書き込み、内蔵 USB での設定コンソール(`show` / `status`)。
* 未確認:ESP32-S3 での Wi-Fi 接続と送受信、固定 IP での動作、長時間の連続運用、BlueDV 以外のクライアント、DVSI 製 AMBE チップでの復号との相互運用性の詳細。
