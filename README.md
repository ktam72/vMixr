# Mixr

macOS で「アプリの音声を画面収録やビデオ会議に載せる」ための仮想オーディオデバイス。

macOS の画面収録はマイク入力しか録音できないため、再生中の音をそのまま録画に含める
ことができない。Mixr は 4 台の仮想オーディオデバイス（vMixr 1〜4）を提供し、
ミキサーアプリと組み合わせてこれを実現する。

```
アプリの音声
   │  システム出力 = vMixr 1
   ▼
vMixr 1 ──[ループバック]──▶ LadioCast ──┬─ メイン ──▶ vMixr 2 ──▶ 画面収録のマイク
                                        └─ Aux 1 ──▶ スピーカー（自分で聞く用）
```

ドライバは各デバイスの内部で音を折り返すだけで、デバイス間の結線は行わない。
結線はミキサーアプリ（LadioCast）が担当する。

## 動作環境

- Apple Silicon 搭載 Mac
- macOS 26 (Tahoe)
- Xcode Command Line Tools（`xcode-select --install`）
- LadioCast（Mac App Store で入手・無料）

## インストール

### 1. ドライバをビルドする

```bash
git clone https://github.com/ktam72/vMixr.git
cd vMixr
zsh driver/build.sh
```

`driver/Mixr.driver` が生成される。

### 2. ドライバをインストールする

管理者権限が必要。CoreAudio デーモンの再起動を伴うため、**実行すると再生中の音声が
一度止まり、起動中のオーディオアプリは再起動が必要になる**。

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mixr.driver
sudo cp -R driver/Mixr.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

### 3. 確認する

システム設定 > サウンド に `vMixr 1` 〜 `vMixr 4`（種類: 仮想）が表示されれば成功。

コマンドラインでも確認できる。

```bash
zsh driver/loopback-test/build.sh
driver/loopback-test/list_devices
```

## 使い方

### 1. システムの出力先を vMixr 1 にする

システム設定 > サウンド > 出力 で `vMixr 1` を選ぶ。

この時点でスピーカーからは音が聞こえなくなる。音は vMixr 1 に流れており、
次の手順でスピーカーに戻す。

### 2. LadioCast を設定する

| 欄 | 設定 |
|---|---|
| 入力 1 | vMixr 1 |
| 入力 1 の送り先 | 「メイン」と「Aux 1」を両方オン（文字が赤くなる） |
| 出力 メイン | vMixr 2 |
| 出力 Aux 1 | MacBook Pro のスピーカー（普段使っている出力先） |

音を鳴らすと、入力 1・出力メイン・出力 Aux 1 のレベルメーターが振れ、
スピーカーからも音が聞こえるようになる。

### 3. 画面収録のマイクを vMixr 2 にする

画面収録（⌘⇧5）のオプションでマイクに `vMixr 2` を選ぶ。
これでアプリの音声が録画に含まれる。

自分の声も入れたい場合は、LadioCast の入力 2 に内蔵マイクを割り当て、
送り先を「メイン」にする。

## 動作確認

ドライバ単体の動作を検証する。**実行前に LadioCast を終了させること**
（稼働していると、その中継がデバイス間の音漏れとして検出される）。

```bash
driver/loopback-test/loopback_test
```

各デバイスが自分の出力だけを受け取り、他のデバイスには漏れていなければ PASS。

その他のツール:

```bash
driver/loopback-test/list_devices   # デバイスと UID の一覧
driver/loopback-test/defdev         # 現在の既定入出力デバイス
```

## アンインストール

```bash
sudo rm -rf /Library/Audio/Plug-Ins/HAL/Mixr.driver
sudo killall coreaudiod
```

システムの出力先を元のデバイス（スピーカー等）に戻すのを忘れずに。

## トラブルシューティング

**音が全く聞こえない**

システム出力が vMixr 1 のままで、LadioCast がスピーカーへ送っていない状態。
LadioCast の出力 Aux 1 にスピーカーを設定し、入力 1 の「Aux 1」をオンにする。

**LadioCast のレベルメーターが動かない**

ドライバを入れ直した直後は、`killall coreaudiod` によって LadioCast の
デバイス接続が切れている。LadioCast を再起動する。

改善しない場合は、システム設定 > プライバシーとセキュリティ > マイク で
LadioCast が許可されているか確認する（仮想デバイスの入力もマイク扱いになる）。

**音が割れる・爆音になる**

デバイスの出力を同じデバイスの入力へ戻す経路をミキサー側でも作ると、
ループが閉じて音量が発散する。LadioCast の入力と出力に同じ vMixr デバイスを
指定していないか確認する。

**サウンド設定に vMixr が出ない**

```bash
ls -d /Library/Audio/Plug-Ins/HAL/Mixr.driver   # 配置されているか
sudo killall coreaudiod                          # 再読み込み
```

## 制約

- ad-hoc 署名のため、**手元でビルドしたものだけが動作する**。ビルド済みバイナリを
  配布する場合は Developer ID 署名と公証が別途必要
- フォーマットは 48kHz / float32 / ステレオ固定
- デバイス数は 4 台固定

## ライセンス

[Apache License 2.0](LICENSE)
