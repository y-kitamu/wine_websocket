# kabu STATION on Linux/Wine — 環境構築・再現ガイド

このドキュメント **単体** で、Linux + Wine 上で kabu STATION を動かし、ローカル API
(`http://localhost:18080/kabusapi/`)の **REST + WebSocket push 受信** まで動作する
環境を再現できるようにまとめたものです。

- 最終更新: 2026-07-07
- 対象: au カブコム証券 kabu STATION(Windows 専用 / .NET Framework 4.x 製)
- 成果物一式: `~/wine_websocket_fix/`(本ドキュメント・ソース・ビルド済みバイナリ)
- WebSocket push が Wine で届かない根本原因と修正の詳細: [`INVESTIGATION.md`](INVESTIGATION.md)

> **なぜカスタムビルドが必要か**
> kabu STATION の WebSocket サーバは Windows の HTTP Server API(`http.sys` +
> `httpapi.dll`)と WebSocket Protocol Component(`websocket.dll`)を使う。Wine 標準の
> これらは実装が不完全で、(1) WebSocket 101 後に HTTP パーサへ戻る、(2)
> `HttpCancelHttpRequest` 未実装、(3) `websocket.dll` 不在/不完全、により push が
> 届かない。本環境では **3 つのコンポーネントを自作/修正して差し替える**。

---

## 0. アーキテクチャと差し替え対象

```
kabu STATION (KabuS.exe, .NET Framework 4.x, 64bit)
   └─ System.Net.WebSockets (Microsoft 純正 System.dll)
        └─ websocket.dll  ★自作シム v2.17 (WebSocket Protocol Component)
   └─ HttpListener / HTTP Server API
        ├─ httpapi.dll    ★修正版 (HttpCancelHttpRequest 実装)
        └─ http.sys       ★修正版 (WebSocket upgrade 後の挙動 + IOCTL)
   └─ localhost:18080 で REST + WebSocket を待ち受け
        └─ Python クライアント(websocket-client)が push を受信
```

| コンポーネント | 差し替え先 | 権限 | WINEDLLOVERRIDES |
|---|---|---|---|
| `http.sys`   | `~/.wine/drive_c/windows/system32/drivers/http.sys` | user | 不要(native 優先で自動) |
| `httpapi.dll`| `/opt/wine-stable/lib/wine/{x86_64,i386}-windows/httpapi.dll` | **sudo(システム全体)** | 不要(builtin 経路) |
| `websocket.dll` | `~/.wine/drive_c/windows/{system32,syswow64}/websocket.dll` | user | **`websocket=native`** |

---

## 1. 前提(OS パッケージ・ツール)

Ubuntu/Debian 系を想定。

```bash
# Wine ビルド & mingw クロスコンパイル & 実行時ツール
sudo apt-get update
sudo apt-get install -y \
    build-essential gcc-multilib flex bison \
    gcc-mingw-w64 g++-mingw-w64 \
    winetricks xdotool ibus \
    python3 python3-pip
```

- `gcc-multilib` / `flex` / `bison`: Wine を 32/64bit 両対応でソースビルドするのに必須。
- `gcc-mingw-w64`: `websocket.dll` シムのビルドに使用(`x86_64-w64-mingw32-gcc`,
  `i686-w64-mingw32-gcc`)。
- `xdotool` / `ibus` / `pyautogui`: 自動ログイン(付録 B)で使用。

Python 側(受信テスト・自動ログイン):

```bash
pip install websocket-client pyautogui google-api-python-client \
    google-auth-oauthlib loguru pydantic requests
```

---

## 2. Wine 11.0 のインストール

本環境は **Wine 11.0**(`/opt/wine-stable`)。WineHQ の公式リポジトリから導入する。

```bash
sudo dpkg --add-architecture i386
sudo mkdir -pm755 /etc/apt/keyrings
sudo wget -O /etc/apt/keyrings/winehq-archive.key https://dl.winehq.org/wine-builds/winehq.key
# 例: Ubuntu 24.04 (noble)。ディストリに合わせて URL を変更
sudo wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources
sudo apt-get update
sudo apt-get install -y --install-recommends winehq-stable   # → /usr/bin/wine が 11.0

wine --version   # wine-11.0 であることを確認
```

> ⚠️ パッケージの `winehq-stable` が 11.0 でない場合は、11.0 を明示指定するか、
> ソースからビルドする(付録 A.3 のツリーを流用可)。**httpapi.dll をシステム全体で
> 差し替えるため、Wine のバージョンは 11.0 に固定すること**(ABI 不一致回避)。

---

## 3. Wine プレフィックス作成 + .NET Framework 導入

kabu STATION は 64bit + .NET Framework **4.5 以上**(`System.Net.WebSockets` サーバ側
API が 4.5 で追加)が必要。

```bash
export WINEPREFIX=$HOME/.wine
export WINEARCH=win64
wineboot --init
wineserver -w

# .NET Framework 4.8 を導入(4.5+ であれば可。CLR は v4.0.30319 として現れる)
winetricks -q dotnet48
# 必要に応じてフォント等
winetricks -q corefonts

# 確認: 4.x CLR と csc.exe が入っていること
ls "$WINEPREFIX/drive_c/windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
```

---

## 4. kabu STATION のインストール

1. au カブコム証券サイトから kabu STATION インストーラを入手。
2. Wine で実行してインストール:
   ```bash
   WINEPREFIX=$HOME/.wine wine /path/to/kabuStationSetup.exe
   ```
3. 実行ファイルが以下に入ることを確認:
   ```
   ~/.wine/drive_c/users/$USER/AppData/Local/kabuStation/KabuS.exe
   ```
   > ⚠️ ディレクトリ名は **`kabuStation`(先頭小文字)**。`KabuStation` ではない。
   > `find ~/.wine -name "KabuS.exe"` で実際のパスを確認すること。

4. Wine の Windows バージョンを **Windows 10** に設定する(WebView2 の動作要件):
   ```bash
   winetricks -q win10
   ```

5. **WebView2 Runtime** をインストールする(ログイン画面が WebView2 を使用):
   ```bash
   # システムの winetricks (apt 版)は webview2 verb を持たないため最新版を取得
   wget -O ~/winetricks_latest \
     https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks
   chmod +x ~/winetricks_latest

   WINEPREFIX=$HOME/.wine ~/winetricks_latest -q webview2
   ```
   インストール済みの確認:
   ```bash
   wine reg query \
     "HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" \
     2>/dev/null | grep pv   # バージョン文字列が出れば OK
   ```

6. **日本語フォント**を設定する(未設定だと UI の日本語が文字化けする):
   ```bash
   # Source Han Sans を Wine に導入してフォントファイル(sourcehansans.ttc)を配置
   WINEPREFIX=$HOME/.wine ~/winetricks_latest -q fakejapanese

   # FontSubstitutes: 日本語 Windows フォント名 → Source Han Sans にマッピング
   # (Wine の Fonts ディレクトリに MS UI Gothic 等が存在しないための代替設定)
   for font in "MS UI Gothic" "MS Gothic" "MS PGothic" "MS Mincho" "MS PMincho" \
               "Meiryo" "Meiryo UI" "Yu Gothic" "Yu Gothic UI"; do
     wine reg add \
       "HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes" \
       /v "$font" /t REG_SZ /d "Source Han Sans" /f
   done

   # 追加: 欧文フォント(Tahoma / Arial 等)経由の豆腐化対策
   # Wine 組み込みの tahoma.ttf 等は日本語グリフを持たず、FontLink 先の
   # MSGOTHIC.TTC も既定では存在しないため、Tahoma / Arial を直接
   # Noto Sans CJK JP(システム日本語フォント)に差し替える。
   # MS Shell Dlg 2 は WinForms のデフォルトフォントで既定では Tahoma に
   # 解決されるが、これも同様に差し替える。
   # Source Han Sans も直接 Noto Sans CJK JP に繋げて FontLink チェーンを補強。
   for font in "Tahoma" "Tahoma Bold" "Arial" "Arial Bold" \
               "Microsoft Sans Serif" "MS Shell Dlg 2" "Source Han Sans"; do
     wine reg add \
       "HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes" \
       /v "$font" /t REG_SZ /d "Noto Sans CJK JP" /f
   done

   # FontLink フォールバック用: MSGOTHIC.TTC が存在しないと Tahoma 等の
   # FontLink チェーンが完全に切断される。NotoSansCJK にシンボリックリンクして補完。
   ln -sf /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
       ~/.wine/drive_c/windows/Fonts/MSGOTHIC.TTC
   ```
   確認:
   ```bash
   wine reg query \
     "HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes" \
     /v "MS UI Gothic" 2>/dev/null | grep -v fixme
   # → MS UI Gothic    REG_SZ    Source Han Sans
   wine reg query \
     "HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes" \
     /v "Tahoma" 2>/dev/null | grep -v fixme
   # → Tahoma    REG_SZ    Noto Sans CJK JP
   ls -la ~/.wine/drive_c/windows/Fonts/MSGOTHIC.TTC
   # → MSGOTHIC.TTC -> /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
   ```

7. 初回は GUI からログイン(ID/パスワード/ワンタイムパスワード)し、
   設定でローカル API を有効化(ポート **18080**、本番)。API パスワードを控える。

> GUI 表示には X ディスプレイが必要(本番環境では `:20` 等の仮想ディスプレイを使用)。

---

## 5. ★カスタムバイナリの導入(最重要)

`~/wine_websocket_fix/` に **ビルド済みバイナリ** が揃っている。これをコピーするのが
最短の再現手順(ソースからの再ビルドは付録 A)。

```bash
cd ~/wine_websocket_fix
```

### 5.1 http.sys(WebSocket upgrade 後の HTTP パーサ復帰バグ修正 + キャンセル IOCTL)

```bash
cp http_ws_v4_cancelreq.sys ~/.wine/drive_c/windows/system32/drivers/http.sys
```
- Wine は `system32/drivers/` に置いた native ドライバを優先ロードするため
  **`WINEDLLOVERRIDES` 不要**。
- md5: `11f0906a976ad3d3a8eedf0d4e2ddd0d`

### 5.2 httpapi.dll(`HttpCancelHttpRequest` 実装)

> ⚠️ **これだけは Wine 本体のインストール先を直接置換する(sudo・システム全体・
> 他プレフィックスにも影響)**。`WINEDLLOVERRIDES=httpapi=native` で
> `system32` に置く方式は **動作しない**(wine-builtin 形式 PE は native ロード経路で
> `c0000135` になる)。詳細は INVESTIGATION.md §3.2。

```bash
# 元の Wine 純正をバックアップ(初回のみ)
sudo cp /opt/wine-stable/lib/wine/x86_64-windows/httpapi.dll ~/wine_websocket_fix/httpapi_ORIGINAL_backup_x86_64.dll
sudo cp /opt/wine-stable/lib/wine/i386-windows/httpapi.dll   ~/wine_websocket_fix/httpapi_ORIGINAL_backup_i386.dll

# 修正版を配置
sudo cp httpapi_v1_cancelreq.dll   /opt/wine-stable/lib/wine/x86_64-windows/httpapi.dll
sudo cp httpapi32_v1_cancelreq.dll /opt/wine-stable/lib/wine/i386-windows/httpapi.dll
```
- md5: x86_64=`818d161c87a7d7cd9efe2ea02c32a914`, i386=`c509f92f6172527ecb1c32d8e569f816`

### 5.3 websocket.dll(★WebSocket push を届かせる v2.17 本体)

```bash
cp websocket_v217.dll   ~/.wine/drive_c/windows/system32/websocket.dll   # 64bit (KabuS.exe 用)
cp websocket32_v217.dll ~/.wine/drive_c/windows/syswow64/websocket.dll   # 32bit
```
- 起動時に **`WINEDLLOVERRIDES="websocket=native"`** が必要(§6 のランチャで指定)。
- md5: 64bit=`78cc06bdd58c67845abfbf376a1d36c2`, 32bit=`a9638a8bc0f4253571f58af638c9df10`
- **v2.17 の要点**: `ALLOCATED_BUFFER` プロパティの `pvValue` を **参照外し**して
  ネイティブバッファ実体アドレスを得る(1 行修正)。これを怠るとフレームヘッダーが
  `.NET` の `ValidateNativeBuffers`/`IsNativeBuffer` に失敗し `AccessViolationException`
  で push が毎回中断する。詳細は INVESTIGATION.md 冒頭「解決済み」節。

### 5.4 導入確認

```bash
md5sum ~/.wine/drive_c/windows/system32/drivers/http.sys \
       /opt/wine-stable/lib/wine/x86_64-windows/httpapi.dll \
       /opt/wine-stable/lib/wine/i386-windows/httpapi.dll \
       ~/.wine/drive_c/windows/system32/websocket.dll \
       ~/.wine/drive_c/windows/syswow64/websocket.dll
# 上記 §5.1〜5.3 の md5 と一致すること
strings ~/.wine/drive_c/windows/system32/websocket.dll | grep -o "websocket.dll v2.17"
```

---

## 6. 起動ランチャ `~/bin/kabustation`

```bash
#!/bin/bash
# DISPLAY 未設定なら X11 ソケットを自動検出
if [ -z "$DISPLAY" ]; then
    for sock in /tmp/.X11-unix/X*; do
        [ -e "$sock" ] && export DISPLAY=":${sock##*X}" && break
    done
fi
# websocket.dll = 自作 native シム(v2.17)を使用。
# http.sys / httpapi.dll は差し替え済みのため override 不要。
WINEDLLOVERRIDES="websocket=native" \
    wine ~/.wine/drive_c/users/$USER/AppData/Local/kabuStation/KabuS.exe "$@"
```

```bash
chmod +x ~/bin/kabustation
```

起動:

```bash
DISPLAY=:20 XAUTHORITY=~/.Xauthority nohup ~/bin/kabustation > /tmp/kabu.log 2>&1 &
# → GUI からログイン(または付録 B の自動ログイン)
```

---

## 7. 動作確認

### 7.1 REST(トークン取得 → 板情報)

```bash
python3 - <<'PY'
import json, requests
PW = "＜kabu STATION API パスワード＞"
t = requests.post("http://localhost:18080/kabusapi/token",
                  json={"APIPassword": PW}, timeout=5).json()["Token"]
h = {"X-API-KEY": t}
requests.put("http://localhost:18080/kabusapi/register",
             headers={**h, "Content-Type": "application/json"},
             json={"Symbols": [{"Symbol": "1458", "Exchange": 1}]}, timeout=5)
d = requests.get("http://localhost:18080/kabusapi/board/1458@1", headers=h, timeout=5).json()
print("REST OK:", d.get("Symbol"), d.get("CurrentPrice"))
PY
```

### 7.2 WebSocket push(★本命)

kabu STATION は **起動直後に登録銘柄の直近価格を一度だけ** push する(再配信には
再起動が必要)。以下で受信できれば成功。

```bash
python3 - <<'PY'
import json, requests, websocket   # pip install websocket-client
PW = "＜API パスワード＞"
t = requests.post("http://localhost:18080/kabusapi/token",
                  json={"APIPassword": PW}, timeout=5).json()["Token"]
requests.put("http://localhost:18080/kabusapi/register",
             headers={"X-API-KEY": t, "Content-Type": "application/json"},
             json={"Symbols": [{"Symbol": "1458", "Exchange": 1}]}, timeout=5)
def on_message(ws, msg):
    d = json.loads(msg); print("PUSH OK:", d.get("Symbol"), d.get("CurrentPrice")); ws.close()
ws = websocket.WebSocketApp("ws://localhost:18080/kabusapi/websocket",
                            header={"X-API-KEY": t}, on_message=on_message)
ws.run_forever()
PY
```

期待出力: `PUSH OK: 1458 89940.0` のように価格が届く。
`~/.wine/drive_c/ws_shim.log` に
`SEND_TO_NETWORK 2bufs ... → CompleteAction ... → COMPLETE` が出れば送信成功
(`WebSocketAbortHandle` が出る場合は websocket.dll が v2.17 でない/未参照外し版)。

---

## 付録 A. ソースから再ビルドする

すべてのソースは `~/wine_websocket_fix/` にある。

### A.1 websocket.dll(v2.17)— mingw で単独ビルド(最も手軽)

```bash
cd ~/wine_websocket_fix
# 64bit
x86_64-w64-mingw32-gcc -shared -O2 -o websocket_v217.dll \
    websocket_shim_v217.c websocket.def -lkernel32 -ladvapi32 -lcrypt32
# 32bit
i686-w64-mingw32-gcc  -shared -O2 -o websocket32_v217.dll \
    websocket_shim_v217.c websocket.def -lkernel32 -ladvapi32 -lcrypt32
```
- `websocket.def` はエクスポート定義(WebSocketCreate/Send/GetAction 等)。
- 生成物を §5.3 の場所へコピー。

### A.2 http.sys / httpapi.dll — Wine 公式ビルドシステムで再構築

Wine 11.0 のソースを展開・configure する。

```bash
# ソース取得(例)
cd /tmp
wget https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz
tar xf wine-11.0.tar.xz && mv wine-11.0 wine-wine-11.0
cd wine-wine-11.0
./configure --enable-archs=i386,x86_64 \
    --without-x --without-freetype --without-gstreamer --without-vulkan  # GUI/media は不要
```

**http.sys**(WebSocket upgrade 後の HTTP パーサ復帰バグ修正 + `IOCTL_HTTP_CANCEL_REQUEST`):

```bash
cp ~/wine_websocket_fix/http_ws.c dlls/http.sys/http.c
cp ~/wine_websocket_fix/wine_http_h_v1_cancelreq.h include/wine/http.h   # IOCTL/構造体定義
make -C dlls/http.sys -j$(nproc)
cp dlls/http.sys/x86_64-windows/http.sys ~/.wine/drive_c/windows/system32/drivers/http.sys
```

**httpapi.dll**(`HttpCancelHttpRequest` を DeviceIoControl 経由で実装):

```bash
cp ~/wine_websocket_fix/httpapi_main_v1_cancelreq.c dlls/httpapi/httpapi_main.c
cp ~/wine_websocket_fix/httpapi_v1_cancelreq.spec   dlls/httpapi/httpapi.spec
# include/wine/http.h は上の http.sys 手順で差し替え済み
make -C dlls/httpapi -j$(nproc)
sudo cp dlls/httpapi/x86_64-windows/httpapi.dll /opt/wine-stable/lib/wine/x86_64-windows/httpapi.dll
sudo cp dlls/httpapi/i386-windows/httpapi.dll   /opt/wine-stable/lib/wine/i386-windows/httpapi.dll
```

httpapi の変更点(参考・3 か所):
- `httpapi.spec`: `@ stub HttpCancelHttpRequest` → `@ stdcall HttpCancelHttpRequest(ptr int64 ptr)`
- `httpapi_main.c`: `HttpCancelHttpRequest()` を実装
  (`DeviceIoControl(queue, IOCTL_HTTP_CANCEL_REQUEST, &params, ...)` を呼ぶ)
- `include/wine/http.h`: `#define IOCTL_HTTP_CANCEL_REQUEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, ...)`
  と `struct http_cancel_request_params { HTTP_REQUEST_ID id; };` を追加

### A.3 高速デバッグ用の C# 再現環境(kabu STATION 不要)

`~/wine_websocket_fix/repro/` に、kabu STATION と同じ
`HttpListener → AcceptWebSocketAsync → SendAsync` 経路を数秒で再現する最小 C# が
ある(push の一度きり制約・ログイン不要)。websocket.dll を弄って挙動確認する際に有用。

```bash
cd ~/wine_websocket_fix/repro
CSC="$HOME/.wine/drive_c/windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
wine "$CSC" /nologo /out:ReproServer.exe /r:System.dll ReproServer.cs
# サーバ起動(自作 websocket.dll を使用)
WINEDLLOVERRIDES="websocket=native" wine ReproServer.exe &
# Python クライアントで受信確認
python3 pyclient.py     # → "RECEIVED 1900 bytes" が出れば OK
```

---

## 付録 B. 自動ログイン(任意)

`stock` プロジェクトの `KabuStationWatcher`(`src/stock/autotrade/kabu_station_watcher.py`)
が、API 健全性監視 + ログアウト検知 + GUI 自動ログインを行う。
- 依存: `xdotool`(ウィンドウ操作)、`pyautogui`(座標クリック/入力)、`ibus`(IME を
  英数に固定)、Gmail API(ワンタイムパスワードをメールから取得)。
- 座標は kabu STATION のログインダイアログレイアウトに依存(定数
  `_INPUT_ID_OFFSET` 等)。解像度/レイアウト変更時は要調整。
- Gmail OAuth トークン(`cert/gmail_token.json`)が必要。

---

## 付録 C. ファイル一覧(`~/wine_websocket_fix/`)

| ファイル | 用途 |
|---|---|
| `websocket_shim_v217.c` | ★websocket.dll シム最新ソース(push 修正版) |
| `websocket.def` | websocket.dll エクスポート定義 |
| `websocket_v217.dll` / `websocket32_v217.dll` | ビルド済み(64/32bit) |
| `http_ws.c` | 修正版 `dlls/http.sys/http.c` ソース |
| `http_ws_v4_cancelreq.sys` | ビルド済み http.sys(導入中) |
| `httpapi_main_v1_cancelreq.c` / `httpapi_v1_cancelreq.spec` | 修正版 httpapi ソース |
| `wine_http_h_v1_cancelreq.h` | 修正版 `include/wine/http.h` |
| `httpapi_v1_cancelreq.dll` / `httpapi32_v1_cancelreq.dll` | ビルド済み httpapi(導入中) |
| `httpapi_ORIGINAL_backup_*.dll` | Wine 純正 httpapi のバックアップ(復旧用) |
| `repro/` | 高速再現用 C#(付録 A.3) |
| `INVESTIGATION.md` | 根本原因の調査記録 |
| `SETUP.md` | 本ドキュメント |

### 導入中バイナリの md5(照合用)

```
11f0906a976ad3d3a8eedf0d4e2ddd0d  http.sys            (= http_ws_v4_cancelreq.sys)
818d161c87a7d7cd9efe2ea02c32a914  httpapi.dll x86_64  (= httpapi_v1_cancelreq.dll)
c509f92f6172527ecb1c32d8e569f816  httpapi.dll i386    (= httpapi32_v1_cancelreq.dll)
78cc06bdd58c67845abfbf376a1d36c2  websocket.dll x64   (= websocket_v217.dll)
a9638a8bc0f4253571f58af638c9df10  websocket.dll x86   (= websocket32_v217.dll)
```

---

## 付録 D. トラブルシューティング

| 症状 | 原因/対処 |
|---|---|
| REST は動くが push が来ない | websocket.dll が v2.17 か確認(`strings ... | grep v2.17`)。`ws_shim.log` に `WebSocketAbortHandle` が出るなら未修正版。 |
| API ステータスが赤/`c0000135` | httpapi.dll を **`/opt/wine-stable/lib/...` に直接置換**したか確認(override 方式は不可)。 |
| 101 後に接続が切れる/400 が返る | http.sys が修正版か確認(§5.1)。 |
| push が一度きりで再現しない | 仕様。再テストは kabu STATION を完全再起動(`pgrep -x KabuS.exe` を kill → `wineserver -k` → 再起動)。 |
| Wine 更新で httpapi が純正に戻った | Wine を再インストールすると `/opt/wine-stable/...` が上書きされる。§5.2 を再実行。 |
| 特定の UI パーツだけ日本語が豆腐化する | Wine 組み込みの Tahoma / Arial は日本語グリフなし、かつ FontLink 先の `MSGOTHIC.TTC` が既定で存在しないため。§4.6 の欧文フォント向け FontSubstitutes と `MSGOTHIC.TTC` シンボリックリンクを設定済みか確認。未設定なら §4.6 の追加コマンドを実行後 `wineserver -k` で再起動。 |

### クリーン再起動(検証時)

```bash
KPID=$(pgrep -x KabuS.exe); [ -n "$KPID" ] && kill $KPID
sleep 2; wineserver -k; sleep 2
DISPLAY=:20 XAUTHORITY=~/.Xauthority nohup ~/bin/kabustation > /tmp/kabu.log 2>&1 &
```
> `pkill -f KabuS.exe` は使わない(自分自身のコマンドラインにマッチし得るため
> `pgrep -x` を使う)。
