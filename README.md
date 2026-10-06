# iiv-server

Windows 10 / 11 用のリモート デスクトップのサーバー(作りかけ)。[iivnc-server](https://github.com/iosxi/iivnc-server)
の後継として、VNC(RFB)の規格を捨てて速さを優先する。相手は [iiv-client](https://github.com/iosxi/iiv-client)。

**今はまだ実験の段階で、使えるアプリはありません。** 下の試験プログラムで、Windows に最初から入っている
Media Foundation のハードウェア動画エンコーダ・デコーダを測ったところです。

## なぜ VNC を捨てるか

iivnc は JPEG / zlib のタイルで画面を送る(VNC の Tight)。v8 で JPEG を SSE2 で速くしたが、それでも

- 動画のような 1280×720 の窓が 60fps で動くと、1 フレーム 約 340KB(60fps で 約 160Mbps)を送る。
- 取り込んだ絵を GPU から CPU へ写して符号化するので、写しを待つ間(約 5ms)と CPU の時間がかかる。
- VNC は「要求 → 更新」の往復が前提。

RustDesk や Moonlight / Sunshine は、GPU の動画エンコーダ(H.264 / HEVC)で画面を圧縮して送る。同じことを
Windows の標準の部品だけでやる。

## 実測(2026-10-06、Core i7-12700KF・RTX 3070 Ti・Windows 11。Windows 10 では確かめていない)

**使える部品(`tools/mfprobe.c`)**

| | H.264 | HEVC | AV1 |
|---|---|---|---|
| エンコーダ(ハードウェア) | NVIDIA H.264 Encoder MFT、Microsoft AVC DX12 Encoder | NVIDIA HEVC Encoder MFT | なし |
| エンコーダ(ソフトウェア) | H264 Encoder MFT(Windows 標準) | なし | なし |
| デコーダ | Microsoft H264 Video Decoder MFT(Windows 標準。DXVA で GPU も使える) | **なし**(ストアの拡張が要る) | AV1VideoExtension(ストアの拡張) |

→ 追加のインストールなしに Windows 10 のどの PC でも復号できるのは **H.264 だけ**。

**H.264 の符号化(`tools/mfenc.c`)と復号(`tools/mfdec.c`)。** 1920×1080 のデスクトップ風の背景の上に、
1280×720 の窓(ふだんの操作 = 文字が流れて四角が動く / 動画 = 全面が毎フレーム変わる)。60fps の間隔で 1 枚ずつ、
D3D11 の NV12 テクスチャで入れる。低遅延モード、キーフレームは最初の 1 枚だけ。

| 絵 | 指定 | 実際の帯域 | PSNR Y / UV | 符号化の遅れ 中央 / 95% | 符号化の CPU |
|---|---|---|---|---|---|
| ふだんの操作 | CBR 10Mbps | 1.8Mbps | 51.6 / 77.1dB | 2.8 / 4.0ms | 3.9% |
| ふだんの操作 | CBR 40Mbps | 10.1Mbps | 65.7 / 83.1dB | 2.8 / 7.8ms | 4.7% |
| 動画 | CBR 5Mbps | 4.8Mbps | 45.1 / 42.7dB | 2.5 / 4.2ms | 4.7% |
| 動画 | CBR 10Mbps | 10.1Mbps | 49.7 / 49.2dB | 2.5 / 3.6ms | 6.3% |
| 動画 | CBR 20Mbps | 27.5Mbps | 52.1 / 57.0dB | 2.6 / 4.2ms | 7.1% |

- CPU は 1 コア = 100%。PSNR は NV12(色差を 4:2:0 に間引いた後)どうしの比較で、**間引きで色付きの文字の縁が
  にじむ分は入っていない**。
- 参考: iivnc v8 の JPEG 95・4:4:4 は、同じ動画の窓で 1 フレーム 342KB(約 160Mbps)、PSNR 45.9dB。
- メモリの NV12 で渡すと、符号化の遅れ 中央 5.3ms・CPU 11.6%(テクスチャより遅い)。
- 復号: CPU で 1 フレーム 1.8ms(CPU 時間 6.2ms、複数スレッド)。DXVA なら CPU 時間 0.4ms。
- NVIDIA の MFT は B フレームの数の設定を受け付けない(0x80070057)が、低遅延モードで B フレームは出ていない
  (1 枚入れると 1 枚出る)。

## 試験プログラムのビルド

VS 2022 Build Tools の x64 開発者コマンド プロンプトで:

```
cl /nologo /utf-8 /O2 /MT tools\mfprobe.c
cl /nologo /utf-8 /O2 /MT tools\mfenc.c
cl /nologo /utf-8 /O2 /MT tools\mfdec.c
```

使い方は各ファイルの先頭に書いた。
