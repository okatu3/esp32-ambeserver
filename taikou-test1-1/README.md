# taikou-test1-1：IC-705 2 台の対向試験（ダミーロード）

IC-705 を 2 台、それぞれダミーロードにつないで並べ、片方（A）が送信、もう片方（B）が受信して、
**USB だけで M17 が通るか**と、**A の送信の音声処理（低域カット・プリエンファシス）の実際**を測るためのプログラムです。
開発中の m17_pc とは別のプログラムです（M17 の電波側の部品と `fm_path.c` だけを共用）。

* B の USB 出力を **12 kHz の IF** にして PC で FM 検波すると、平らな受信機になる。これで A の送信を測る。
* 送信は 1 回 45 秒まで。Ctrl+C・終了・時間切れで、必ず PTT を切る（CI-V で 2 回）。
* **ダミーロード、最小出力、免許の範囲で。** 周波数はアマチュアバンドのシンプレックス。

## ビルド
```
taikou-test1-1\build.bat        → build_taikou\taikou-test1-1.exe
```
1 つの exe だけで動きます。PC を 2 台使うときは、この exe をコピーするだけ。

## 準備（IC-705 の設定）
両方：
* モード **FM** で **DATA** を押して **FM-D**。同じ周波数（シンプレックス）。
* RF 出力は最小。アンテナ端子にダミーロード。2 台は 1〜2 m 離す。
* CI-V：アドレス A4（既定）、CI-V USB Echo Back は OFF（既定）。
* MENU > SET > Connectors > MOD Input > **DATA MOD = USB**（既定）、USB MOD Level は 50 %（既定）から。

受信側（B）：
* MENU > SET > Connectors > USB AF/IF Output > **Output Select = IF**。

## どの番号がどの無線機か
同じ名前の「USB Audio CODEC」が 2 つ出るので、**1 台ずつ USB をつないで** 確かめます。
```
taikou-test1-1 list                 （A だけつないで：A の入力・出力の番号と COM を控える）
taikou-test1-1 list                 （B もつないで：増えたものが B）
taikou-test1-1 civ COM7             （COM ごとに：周波数・モード・データモード・出力設定を読む。無線機は何も変えない）
```
`civ` で「data mode ON」「RF power」が低いことを確かめる（高いと注意が出る）。

## 手順
番号は例（A：出力 3・COM7、B：入力 2）。**受信側（B）を先に起動**してから、送信側（A）を動かします。

1. 自己試験（無線機なし、30 秒ほど）：`taikou-test1-1 selftest`
2. **IF の確認**（B）：`taikou-test1-1 ifcheck --in 2 --sec 10`。その間に A の PTT を手で数秒押す（無変調）。
   「looks like the 12 kHz IF」と、2 台の周波数のずれ（Hz）が出れば OK。「AUDIO」と出たら Output Select を IF に。
3. **A の送信の音声処理を測る**（約 40 秒）
   * B：`taikou-test1-1 tones-rx --in 2 --sec 60 --wav tones_A.wav`
   * A：`taikou-test1-1 tones-tx --out 3 --ptt COM7`
   * B に、周波数ごとの偏移（10 Hz〜5 kHz）、音量の段ごとの偏移（リミッターの効き始め）、
     **推定された低域カット・次数・プリエンファシス**、m17_pc 用の設定、M17 に必要な音量が出る。
   * 1 kHz（-20 dBFS）の偏移が小さすぎる／大きすぎるときは、A の USB MOD Level を変えてやり直す。
4. **M17 の試験送信**（3 回、各 4 秒）
   * B：`taikou-test1-1 m17-rx --in 2 --sec 60 --wav m17_A.wav`
   * A：`taikou-test1-1 m17-tx --out 3 --ptt COM7 --call 自局 --radio-hp 〔3 の値〕 --radio-hp-order 〔3 の値〕 --radio-preemph 〔3 の値〕`
   * B1＝補正なし、B2＝推定値で補正、B3＝3 で測った値で補正。B に、それぞれのフレーム数・ビット誤り・EVM が出る。
   * 偏移が足りないとき：`--level`（補正前の +3 記号の大きさ、既定 -10）、`--pre-gain`（補正後、既定 -36）、
     `--pre-floor`（低域をどこまで戻すか、既定 50 Hz）で調整。補正で音が飽和すると `[W]` が出る。
5. **役を入れ替えて**（B が送信、A が受信）3 と 4 をもう一度。2 台の個体差がわかる。
6. 録音した WAV（`tones_A.wav`・`m17_A.wav` など）は、あとで `taikou-test1-1 analyze tones tones_A.wav --level -20`、
   `taikou-test1-1 analyze m17 m17_A.wav` で解析し直せる。持ち帰ってください。

## 表示の見方
* `tones-rx`：`best fit: high-pass 250 Hz order 2, pre-emphasis none ...` が A の送信の音声処理。
  RMS の誤差が 2 dB を超えるときは、単純なフィルターではない（表をそのまま見る）。
* `m17-rx`：`B2: ... frames 100, frames with errors 0 ...` なら、その補正で M17 が通った。
  B1（補正なし）は、低域カットのある無線機では受からないのが普通。
* B が FM-D でも IF 出力が出ない、偏移が測れないなど、想定外のことは、そのままの表示と WAV を持ち帰ってください。

## オプション
`--in N` `--out N` `--ptt COMx` `--baud 115200` `--addr A4` `--level dBFS`（試験音は既定 -20、M17 は -10）
`--sec S` `--wav FILE` `--rx-af`（B の USB 出力が音声のとき）`--tx-delay MS`（既定 300）`--max-tx S`（既定 45）
`--frames N`（既定 100）`--radio-hp HZ` `--radio-hp-order N` `--radio-preemph US` `--pre-floor HZ` `--pre-gain DB`

## 確かめたこと（2026-11-17、無線機なし）
* `selftest`：模型の無線機（低域カット 250 Hz・2 次、プリエンファシスなし、高域カット 3 kHz、IF の CNR 30 dB）を通した
  試験音から、250 Hz・2 次・なし・3000 Hz を誤差 0.3 dB で推定。M17 は CNR 20 dB で、補正なし 0 フレーム、
  推定値で補正 60 フレーム誤り 0、測った値で補正 60 フレーム（1 フレームに誤り）。
* PC のマイク入力での録音（`ifcheck`・`m17-rx`）、スピーカーへの再生（PTT なし）。
* **まだ**：IC-705 の実機、CI-V（読み取りと PTT）、USB の音声。
