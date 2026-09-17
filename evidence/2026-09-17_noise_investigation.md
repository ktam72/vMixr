# vMixr ノイズ調査（2026-09-17）

## 症状
vMixr デバイス経由の音声が「ケースによって」ノイジーに聞こえる。

## 検証環境
- デプロイ済み `/Library/Audio/Plug-Ins/HAL/vMixr.driver` が作業ツリーの `driver/vMixrDriver/vMixrDriver.c`（verbose ログ既定オフ版）と一致することを `strings`（`mixr_verbose` 文字列の有無）で確認。
- 計測ツール（セッションのスクラッチパッドに作成、未コミット。承認があれば `driver/loopback-test/` に追加する）
  - `glitch_test play|capture|both <dev> <bufFrames> <sec>`: `AudioDeviceIOProc` クライアント。
  - `incap <dev> <bufFrames> <sec>`: AUHAL で出力を無効化した**入力専用**クライアント（QuickTime 等の収録アプリと同形態）。
  - 書き手と読み手は**別プロセス**（＝別 HAL クライアント）で起動。
- 信号と解析
  - 予備計測: 1 kHz / 振幅 0.9 の正弦。二階差分 `> 0.06` で継ぎ目を検出（理論最大 0.0154）。全条件で rms = 0.6364（理論値）・定常 maxD2 = 0.0154 だったので、経路はリサンプル・ゲインなしの **bit-exact** と判断。
  - 本計測（`RAMP=1`）: bit-exact を利用し、書き手がフレーム番号 `(ctr & 0xFFFFF)` をそのままサンプル値に書く。読み手側で `x[n]-x[n-1] != 1` の点を継ぎ目、`(x[n]-x[n-1])-1` を**符号付きの正確なフレームオフセット**とする。先頭 1 バッファ（起動過渡）と無音境界は除外。

## 結果（本計測 RAMP、3 秒、継ぎ目数は起動過渡除外）

| # | 書き手 / 読み手 バッファ | 読み手種別 | run | 継ぎ目数 | オフセット（run 内で一定） |
|---|---|---|---|---|---|
| R-T2 | 512 / 512 | IOProc | 1 | **839** | −8 ×559, +16 ×280 |
| | | | 2, 3 | 0 | — |
| R-T10 | 512 / 512（読み手先行、書き手を d 秒後に開始） | IOProc | d=0.71 | **374** | −12 / +12 を毎バッファ |
| | | | d=0.73〜0.81（5 run） | 0 | — |
| R-T7 | 512 / 512 | 入力専用 | 1〜3 | 0 | — |
| | | | 4 | 2 | −132 / +132 |
| R-T3 | 512 / 1024 | IOProc | 1 | 278 | ±344 |
| | | | 2 | 279 | ±76 |
| | | | 3 | 279 | ±36 |
| R-T4 | 1024 / 256 | IOProc | 1 | 280 | ±216 |
| | | | 2 | 280 | ±240 |
| | | | 3 | 280 | ±120 |

補助計測（正弦）:
- T1: 同一クライアントで入出力（既存 `loopback_test` と同条件）→ 0。問題を**検出できない**条件。
- T5b: 書き手停止後の入力専用読み手 → 停止後は完全無音（古いデータの再生は起きない。HAL がゼロ初期化したミックスを WriteMix に渡し続けるため）。
- T6b: 走行中に同一デバイスへ 2 クライアント目が StartIO → 0（`MixrStartIO` の無条件再アンカーによる実害は観測されず）。

### 継ぎ目の signature
1. バッファ周期（読み手または書き手の大きい方）ごとに **合計がゼロになる複数ステップ**（多くは −N → +N の対、R-T2 run 1 は −8, −8, +16 の 3 ステップ）。落ちも溜まりもせず、**N フレーム分の区間だけ古い／重複したデータが挿入される**。
2. N とバッファ内オフセットは **run 内で固定、run 間で変化**（8/16, 12, 36, 76, 120, 132, 216, 240, 344）。
3. リング 1 周（−8192）は一度も出ない → 安全余裕不足によるリング周回ではない。
4. バッファサイズが異なると **必ず**発生（6/6 run）。同じサイズでも位相次第で発生（IOProc 2/9 run、入力専用 1/4 run）。
   → 「ケースによってノイジー」の正体は、書き手サイクルと読み手サイクルの位相差が読み窓に掛かるかどうかの偶然。

## 原因（コード）
`driver/vMixrDriver/vMixrDriver.c` `MixrDoIOOperation`（ReadInput）:

```c
uint32_t pos = (obj->ringHead[dev] + R - frames + f) % R;
```

- 読み位置を、HAL が渡す `ioCycleInfo->mInputTime` ではなく、**書き手が更新する共有変数 `ringHead` と読み手自身の `frames`** から決めている。
- これは「読み手と書き手が同じサイクル・同じフレーム数で、WriteMix の直後に ReadInput が呼ばれる」ことを前提にしているが、別クライアントは別の IO サイクルで動くため成立しない。両サイクルの位相差がそのまま N として現れる。
- `ringHead` は書き手がフレーム毎に更新し、読み手はループ内で毎回読み直しているため、読み窓が更新途中でずれる（R-T2 run 1 の −8/+16 のように 1 バッファ内で複数回ずれるのはこれと整合）。

## HAL の契約（一次情報）
`AudioServerPlugIn.h`（Xcode SDK, `CoreAudio.framework/Headers`）`AudioServerPlugInIOCycleInfo`:
- `mInputTime`: "The time stamp that indicates from where in the device's time line the input data for the new IO cycle will start at."
- `mOutputTime`: "The time stamp that indicates from where in the device's time line the output data for the new IO cycle will start at."

つまり読み書き位置は、`GetZeroTimeStamp` が張るデバイス共通タイムライン上の `mSampleTime` で与えられ、クライアント種別やバッファサイズに依らず整合する。現行実装はこれを使っていない。

## 対策案（未実装・要承認）
- WriteMix: `pos = ((UInt64)ioCycleInfo->mOutputTime.mSampleTime + f) % R` に書く。
- ReadInput: `pos = ((UInt64)ioCycleInfo->mInputTime.mSampleTime + f) % R` から読む。
- `ringHead` は廃止（読み手／書き手間の共有可変状態がなくなる）。
- 読み後のゼロクリアは**しない**（T5b で不要と確認済み。また同一デバイスを複数アプリが同時キャプチャすると後続が無音になる後退を招く）。
- **`MixrStartIO` の再アンカーを参照カウント化（必須）**: 現行は StartIO のたびに `timeStampCount[dev] = 0` に戻すため、サンプルタイム索引に切り替えるとクライアントが 1 本増えるたびに `pos` が不連続にジャンプする。T6b の「0 件」は現行の `ringHead` 方式がサンプルタイムから独立しているから成立しているだけで、修正後は成立しない。参照実装 NullAudio の `gDevice_IOIsRunning`（初回のみアンカー、最後の StopIO で解除）に合わせる。
- **前提確認（実装前に必要）**: `mInputTime.mSampleTime − mOutputTime.mSampleTime` の実測。差が 0 に近いと、同一サイクルで書き込み中の領域を読むことになる（現行の `ringHead − frames` は「書き終わった領域」を暗黙に保証していた）。ユーザーランドからは観測できないため、`DoIOOperation` で `mIOCycleCounter % 512 == 0` のときだけ `operationID / frames / mInputTime / mOutputTime`、StartIO/StopIO で `clientID / timeStampCount` をログする**一時計測ビルド**を入れて R-T3 と R-T7 を 1 回ずつ回す（ドライバ編集のため要承認）。差が不足する場合は `kAudioDevicePropertySafetyOffset`（現在 0）で余裕を確保する。

受け入れ基準: 同ツール（RAMP）で R-T2 / R-T3 / R-T4 / R-T7 / R-T10 を各 10 run 以上回し、継ぎ目 0。加えて同一デバイスへ入力専用読み手 2 本同時で両方が信号を受け取ること。

---

## 追記: 計測ビルドと修正後の検証（同日）

### 計測ビルド（一時ログ、承認済み・修正時に撤去）
- 同一サイクル内の `mOutputTime − mInputTime = 1024`（512 フレームバッファのクライアント）。入力窓は 1 バッファ過去、出力窓は 1 バッファ未来。
- ReadInput では `mOutputTime = 0`、WriteMix では `mInputTime = 0`（HAL は該当側のみ埋める）。
- HAL はリング境界（ZeroTimeStamp 周期 8192）で IO を `frames=60 / 452` のように分割する。
- 2 クライアント目を追加しても `startio` / `stopio` は各 1 回。**HAL がデバイス単位で参照カウントしている**ため、`MixrStartIO` の再アンカーは実害なし（参照カウント化は不要と判明。前節の「必須」は撤回）。

### 修正内容（`driver/vMixrDriver/vMixrDriver.c`）
- `ringHead` を廃止し、WriteMix は `mOutputTime.mSampleTime % R`、ReadInput は `mInputTime.mSampleTime % R` から索引。
- ゼロクリアなし。読み手 2 本同時でも両方が受信できることを確認。

### 修正後の受け入れ結果（RAMP、各 10 run × 3 秒、起動過渡除外）

| 書き手 / 読み手 | 読み手種別 | 継ぎ目合計 |
|---|---|---|
| 512 / 512 | IOProc | **0** |
| 512 / 1024 | IOProc | **0**（修正前: 毎回約 279） |
| 1024 / 256 | IOProc | **0**（修正前: 毎回約 280） |
| 512 / 512 | 入力専用 | **0** |
| 512 / 512 読み手先行 d=0.71/0.73/0.75 | IOProc | **0**（修正前: d=0.71 で 374） |
| 512 / 512 入力専用 2 本同時 | 入力専用 | A: 0, B: 0（両方 143872 フレーム受信） |
| 書き手停止後 | 入力専用 | 停止後は無音（古いデータ再生なし） |

`incap` の 1024 バッファは AUHAL 側の制約で 0 フレーム（未計測）。異サイズ条件は IOProc 版で担保。

### 副作用と留意点
- **ループバック遅延は 2 × IO バッファ**（256 フレームで 512 = 10.7 ms、512 フレームで 1024 = 21.3 ms）になる。HAL が入力窓を 1 バッファ過去・出力窓を 1 バッファ未来に置くため。ドライバの申告値は `kAudioDevicePropertyLatency = 0`、`kAudioStreamPropertyLatency = 0`、`SafetyOffset = 0` なので、申告値による上乗せはなく、この 2 バッファは HAL の IO スケジュール由来。修正前の「1 バッファ」は同一クライアント・同一サイズの前提でしか成立しない値だった。REQ-105（スパイク遅延 ≤ 1000 フレーム）は IO バッファ 256 なら満たし、512 では満たさない。
- **リング容量との制約（重要）**: 読み窓と書き窓の分離が `2 × IO バッファ` なので、サンプルタイム索引ではリング容量 R が `R ≥ 4 × IO バッファ` を満たす必要がある（読み窓・書き窓の各長さ＋分離）。`backup/req-107` ブランチ（REQ-105〜107 実装済み）は `kMixrRingFrameCount = 1024` で、512 フレームクライアントでは分離 1024 = R となり**読み窓と書き窓が同じ位置に重なる**。main の R = 8192 では問題ないが、ZeroTimeStampPeriod = 8192（170 ms）は REQ-105 の「100 ms 以下」を満たしていない。→ 実装対象ブランチと R の値（2048 以上、例: 4096 = 85 ms）を要判断。
- `loopback_test` の vMixr 2 LEAK は稼働中のミキサーアプリ（`vMixrInte…`）が vMixr 1 → vMixr 2 を中継しているため（ゲイン 0.96）。ドライバ由来ではないと判断しているが、確定にはミキサー停止での再実行が必要。
- `driver/loopback-test/loopback_test` バイナリは Sep 10 の `backup/req-107` 系（REQ-105 遅延チェック入り）で、main の `loopback_test.c` にはそのチェックがない。再ビルドにより **main のテストからは遅延回帰が見えなくなる**点に注意。
- 計測ツール `glitch_test.c` / `incap.c` / `analyze.h` を `driver/loopback-test/` に追加、`build.sh` に組み込み。未コミット。
