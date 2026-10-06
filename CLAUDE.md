# iiv-server の作業方針

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: `https://github.com/iosxi/iiv-server.git`(`iosxi/iiv-server`)
- ブランチ: **`master`**
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- リリースの添付物: **`iiv-server.exe`**。改名せず、そのまま `gh release create` に渡す。
- バージョン: タグの `vN` とは別に、`src/iiv-server.rc` の VERSIONINFO、`src/iiv-server.manifest` の
  `assemblyIdentity`、`src/iiv.h` の `APP_VERSION` / `APP_VERSION_A` がある。機能が変わったら全部上げる。
- `src/iiv.h` の `APP_RELEASE`(L"vN")は設定画面のタイトルに出る。**リリースのたびにタグと同じ vN にする。**
- 前身は `../iivnc-server`(VNC のサーバー。そのまま残す)。iiv は 2026-10-06 に VNC を捨てて作り始めた。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**。exe はリポジトリに追跡させている。
アイコンは `python tools/make-icon.py`(Pillow で 1024px に描いて縮める)。サーバーは青いタワー型 PC と外へ出る矢印、接続中(`-active.ico`)は電源ボタンと矢印の縁が緑。クライアントは緑のノート PC と画面へ入る矢印。
Pillow の `polygon(..., outline=, width=)` は縁を内側にしか描かない(白い矢印が細くなる)。縁は `line(..., joint="curve")` で
輪郭をなぞってから塗りを重ねる。

### iiv-client と同じファイル

`src/iivproto.h`(通信の取り決め)、`src/zlite.h` `src/zdeflate.c` `src/zinflate.c`、
`src/theme.c` `src/fwrules.c` `src/filexfer.c`(この 3 つは先頭の `#include` だけ違う)は `../iiv-client/src` と
中身をそろえる。片方を直したらもう片方へ写し、`diff` で確かめる。

## 動作確認について

- **`python tools/test.py`**: サーバーを `-testsrc`(合成した絵。入力はログに書くだけ)と検証用の ini
  (`build/test/test.ini`、127.0.0.1:5999)で動かし、`tools/iivcheck.py`(Python の受け手)で認証・
  動く絵 300 フレーム(`build/mf/mfdec.exe` で全部復号できるか)・止まった絵・大きさの変更・3 人同時・
  返事を返さない相手・でたらめなデータを確かめる。`tools/build-tools.bat` が `tools/mf*.c` を `build/mf` に作る。
- **`python tools/realbench.py --scene office|video|idle [--ini "qmove=70;..."]`**: 実画面(DXGI)を `-dryrun` と
  127.0.0.1:5913 で取り込み、iivnc の `../iivnc-server/tools/srcwin.py` の窓を出して、フレーム/秒・帯域・遅れ・CPU を測る。
- クライアントと組み合わせた検証は `../iiv-client/tools/clientcheck.py`、速さは `../iiv-client/tools/pairbench.py`。
- 試験プログラム: `tools/mfprobe.c`(使えるエンコーダ・デコーダの一覧)、`tools/mfenc.c`(NV12 の連番を符号化。
  `cbr|qp|qp2|qps|q|qdyn`)、`tools/mfdec.c`(復号して PSNR、最後のフレームを書き出す)。
- `.bat` は CRLF(ASCII だけ)。Git Bash の here-doc や sed でバックスラッシュを含む行を書き換えると壊れやすい
  (2026-10-06 に何度か起きた)。Python のスクリプトをファイルに書いて直すほうが確か。

## 実測で分かったこと(2026-10-06、i7-12700KF・RTX 3070 Ti・Windows 11・4K 60Hz)

- 使える部品(`mfprobe`): H.264 のエンコーダは NVIDIA H.264 Encoder MFT(GPU)・Microsoft AVC DX12 Encoder・
  H264 Encoder MFT(CPU)。HEVC は NVIDIA のみ。**デコーダは H.264 だけが Windows 標準**(HEVC・AV1 はストアの拡張)。
  → Windows 10 で追加のインストールなしに動かすため H.264 にした。
- **NVIDIA の MFT は `CODECAPI_AVEncVideoEncodeQP` を無視する**(読み返すと設定した値なのに、16 でも 30 でも同じ大きさ。
  フレームごとの `MFSampleExtension_VideoEncodeQP` も効かない)。**`CODECAPI_AVEncCommonQuality`(0〜100)は効く**
  (1080p の文字の絵で 30 → PSNR 34.2dB、90 → 50.2dB)。**符号化の途中で変えても次のフレームから効く**
  (画質 40 の止まった絵に 95 を 1 枚: 36.95 → 49.71dB、その 1 枚 125KB、以後 113 バイト)。
- CBR は止まった画面でも枠を使い切る(4K・80Mbps の枠で、縞しか変わらない絵に 73Mbps)。上限付き可変でも 54Mbps。
  画質で決めると 0.4〜7Mbps。
- 符号化の時間は画素数に比例: 1080p 2.5ms、4K 9〜11ms。`CODECAPI_AVEncCommonQualityVsSpeed`=0 で 4K 9.2ms。
- NVIDIA の MFT は B フレームの数の設定を受け付けない(0x80070057)が、低遅延モードで 1 枚入れると 1 枚出る。
- 非同期 MFT のイベントは `IMFAsyncCallback`(C で vtable を手書き)で受け、Win32 のイベントで待つ(空回りしない)。
- 色の変換は VideoProcessor(入力 RGB 0〜255、出力 BT.709 16〜235)。クライアントで戻した絵は背景の灰色・色の帯が一致。
  明るさの PSNR は止まった絵で 46dB だが、RGB では 30dB(色差 4:2:0 で ClearType の色付きの縁が崩れる)。
- 返事を返さない相手: 待ち行列があふれたら捨てて `resync`。送り手が追いつくまでキーフレームを求めない
  (すぐ求めると、受け取らない相手のためにキーフレームを作り続け、ほかの相手が 11 フレーム/秒に落ちた)。
- 検証用の絵の速さは `Sleep` で合わせるので、`timeBeginPeriod(1)` が要る(既定の 15.6ms 刻みでは 32fps だった)。

## 確かめていないこと

- Windows 10 の機械、Intel / AMD の GPU、CPU のエンコーダだけの PC、サービス(svc.c)、クリップボード、
  ファイルのコピー＆貼り付け、リモート デスクトップの中、2 台の実機の間、複数の画面。
