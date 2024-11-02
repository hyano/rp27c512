# RP27C512

**RP27C512**は、秋月電子通商の[RP2040マイコンボードキット(AE-RP2040)](https://akizukidenshi.com/catalog/g/g117542/)を使用して、EP-ROM(27C512)のエミュレーションを行うデバイスです。このデバイスは、AE-RP2040のGPIOを27C512互換のDIP-28ピンアサインに変換する基板と、GPIOを制御するためのファームウェアで構成されています。

また、ROMのエミュレーションに加え、同一バス上のメモリやデバイスへのアクセスを監視・解析する機能も備えています。

## 概要

* 27C512の代わりにRP27C512を装着して、ROM機能をエミュレーションすることができます。
* RP2040のPIOおよびDMA機能を活用し、/OE信号がアサートされてから約40nsでデータ出力が可能です。
* **RP27C512**をホスト(Windows PCやMac)に接続すると、USBシリアルポートとして認識されます。
* UARTターミナルを使って、コマンドラインインターフェースから各種操作が可能です。
* ROMデータはAE-RP2040のフラッシュメモリに最大4つまで保存でき、切り替えて使用できます。
* ホストからROMデータを**RP27C512**に転送できます。転送にはXMODEMを使用します。
* **RP27C512**に保存されたROMデータをホストに転送することも可能です。こちらもXMODEMを使用します。
* **RP27C512**を27C512に接続すると、実チップからデータを直接読み出すことができます。
* バスアクセスの監視機能を備えており、同一バス上に接続されたメモリやデバイスへのアクセスを解析できます。
  * 書き込みデータを記録して確認でき、必要に応じてXMODEMでホストと転送することも可能です。
  * 指定した領域へのアクセスを時系列で表示する機能も備えています。


## ハードウェア

* AE-RP2040のピンアサインを27C512に変換するシンプルな基板です。ピンヘッダを用いてAE-RP2040と接続します。
* 回路図や基板レイアウトはKiCadで作成しており、`hardware/`ディレクトリに格納されています。
* AE-RP2040のGPIOは3.3Vで駆動し、27C512は5Vで駆動しますが、レベル変換回路は入れていません。作者の環境では正常に動作していますが、故障や想定外の動作について作者は責任を負いかねます。
* ファームウェアにより、RP2040を400MHzまでオーバークロックして使用しています。
* 27C512側の5V電源をAE-RP2040に供給することが可能です。ショットキーバリアダイオードで逆流防止を行っています。

### 部品一覧
* [RP27C512/AE-RP2040変換基板](hardware/)
* [RP2040マイコンボードキット](https://akizukidenshi.com/catalog/g/g117542/)
* [ロープロファイルピンヘッダー(低オス) 1×40(40P) 7.7mm](https://akizukidenshi.com/catalog/g/g102900/)
* [丸ピンIC用連結ソケット 1×14 MH-1X14-L2](https://akizukidenshi.com/catalog/g/g116982/)
* [ショットキーバリアダイオード 45V2A SBM245L](https://akizukidenshi.com/catalog/g/g117439/)


## ファームウェア

* このプログラムは[Raspberry Pi Pico-series C/C++ SDK](https://www.raspberrypi.com/documentation/pico-sdk/)でビルド可能です。
* ソースコードは`firmware/`ディレクトリに格納されています。
* ビルド後に生成される`rp27c512.uf2`を所定の方法で**RP27C512**に書き込み、使用します。
* ホストとUSBケーブルで接続すると、USBシリアルポートとして認識され、コマンドラインインターフェース経由で各種操作が可能です。

## ファームウェアのビルド

事前に Raspberry Pi Pico SDK をインストールし、下記のコマンドでビルドすることができます。

```sh
git clone https://github.com/hyano/rp27c512.git
cd rp27c512
mkdir build
cd build
cmake ..
make
cp rp27c512.uf2 /Volumes/RPI-RP2
```

## 使用方法

### コマンドラインインタフェース

|コマンド|引数|説明|モード|
|-|-|-|-|
|help, ?|-|ヘルプを表示する。|e/c|
|hello|any...|コマンドラインの動作確認用。与えられた引数を表示する。|e/c|
|reboot|[delay]|再起動する。delayで再起動までの秒数を指定可能。|e/c|
|mode|emulator\|clone|ROMエミュレーションを行うemulatorモードとROMの読み出しを行うcloneモードを切り替える。modeはFLASH ROMに保存される。コマンド実行完了後、自動的に再起動する。|e/c|
|gpio|-|GPIOの状態を表示する。|e/c|
|device|rom\|ram|操作対象のデバイスをROMかRAMで切り替える。ダンプ、データ転送に影響する。|e/-|
|d|[addr]|デバイス上のデータを指定したアドレスから16進ダンプする。アドレスを省略すると前回の続きをダンプする。|e/c|
|dw|[addr]|何かキーが押されるまで、16進ダンプを繰り返す。RAM上のデータの変化を目視するために使う。|e/-|
|cap|-|設定したアドレス領域へのアクセスを時系列に従って表示する。|e/-|
|recv|-|ホストからデバイスにデータを転送する。64KiBのバイナリデータをXMODEM(CRC)で転送する。|e/c|
|send|-|デバイスからホストにデータを転送する。64KiBのバイナリデータをXMODEM(1K)で転送する。|e/c|
|bank|0,1,2,3|使用するFLASH ROMのバンクを指定する。バンクの指定はFLASH ROMに保存され、次回起動時はそのバンクからROMデータを読み出す。|e/c|
|load|-|FLASH ROMからデータを読み出す。bankコマンドで指定したバンクを使用する。|e/c|
|save|-|FLASH ROMにデータを保存する。bankコマンドで指定したバンクを使用する。|e/c|
|clone|[wait [verify]]|直接接続した27C512からデータを読み出す。読み出し開始までの秒数(wait)と、ベリファイ回数(verify)を指定できる。|-/c|

* モードはe(emulatorモード)、c(cloneモード)を示します。
* [arg]は省略可能な引数を表します。
* 引数のチェックはほとんどしていないので、不正な引数を指定するとすぐに暴走します。

### cloneモード

cloneモードは、**RP27C512**を直接ROM(27C512)の実チップに接続して、データを読み出すモードです。
`mode clone`コマンドでcloneモードに切り替わります。
ROMを基盤から取り外し、上から被せる形で**RP27C512**を乗せ、ピン同士を接触させます。
**RP27C512**を直接載せるのではなく、平ピン型のICソケットを間に挟むとより接触しやすいです。
ROMの電源は外部から供給しても良いですし、**RP27C512**のショットキーバリアダイオードをバイパスしてUSBポートから供給しても良いでしょう。
`clone`コマンドで実チップからデータを読み出します。
読み出しに成功したら、`save`コマンドでFLASH ROMに保存して、`mode emulator`でemulatorモードに戻しておきます。

```sh
> mode clone
mode: clone
rebooting...

connected.
RP27C512 VER1.00
rom bank: 0
mode: clone
> clone
read ROM
 wait  : 5 s
 verify: 2 times
read start ... done.
verify (1/2) ... OK
verify (2/2) ... OK
clone: OK
> bank 0
current rom bank: 0
> save
save: OK
> mode emulator
mode: emulator
rebooting...


connected.
RP27C512 VER1.00
rom bank: 0
mode: emulator
> 
```

## 利用しているオープンソースソフトウェア
* [XMODEM implementation](https://github.com/Thuffir/xmodem)
  * Copyright 2001-2019 Georges Menie (www.menie.org)
  * Modified by Thuffir in 2019
* [Micro Read Line library for small and embedded devices with basic VT100 support](https://github.com/dimmykar/microrl-remaster)
  * Author: Eugene Samoylov aka Helius (ghelius@gmail.com)
01.09.2011
  * Remastered by: Dmitry Karasev aka dimmykar (karasevsdmitry@yandex.ru)
27.09.2021

## ライセンス

このソフトウェアは[MITライセンス](LICENSE)にて提供しています。  
Copyright (c) 2024 Hirokuni Yano

利用しているOSSは、そのライセンスに従います。