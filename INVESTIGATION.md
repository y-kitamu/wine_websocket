# kabu STATION WebSocket受信 in Wine — 調査記録

最終更新: 2026-07-07

---

## ★★★ 解決済み (SOLVED) — 2026-07-07 ★★★

**根本原因**: websocket.dll シムの `extract_allocated_buffer()` が、
`WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE` プロパティの `pvValue` を
「ネイティブバッファのアドレスそのもの」として扱っていた。しかし実際には
**`pvValue` は「バッファへのポインタが格納されている場所」を指す**
(受信/送信バッファサイズ等の他プロパティと同じく、`pvValue` は値の格納位置であり、
このプロパティの値は "ポインタ")。そのため `native_buf` が
`[m_StartAddress, m_EndAddress]` の外(m_PropertyBuffer 付近、+約33KB)を指していた。

送信時に返す2バッファ `[header(native_buf内), payload(ピン留めpBuffer)]` のうち、
**header が `IsNativeBuffer` に失敗**し `ValidateNativeBuffers` が
`AccessViolationException` を投げ、`WebSocketAbortHandle` で毎回 push が中断していた。
(4.4/4.5 の「payload 側/m_SendBufferState が原因」という推定は**誤り**だった。
実際は payload は常に検証を通過しており、header が原因。)

**修正 (1行)**: `websocket_shim_v217.c` の `extract_allocated_buffer()`:
```c
s->native_buf = *(BYTE **)pProperties[i].pvValue;   /* 参照外し */
```

**特定方法(高速再現環境の構築が決定打)**:
Wine 上の .NET Framework 4.x で `HttpListener.AcceptWebSocketAsync` →
サーバ側 `WebSocket.SendAsync` を行う**最小 C# 再現プログラム**を作成
(`~/wine_websocket_fix/repro/`)。kabu STATION の再起動・ログイン・一度きり push が
不要になり、数秒でイテレーション可能に。さらに reflection で
`WebSocketBuffer` の内部フィールド(`m_SendBufferState`, `m_StartAddress`,
`m_PinnedSendBufferStartAddress` 等)と `ValidateNativeBuffers` を直接叩いて
「header 単独で AVE / payload 単独は通過」「`*(pvValue)==m_StartAddress`」を実測し確定。

**検証**:
- 再現環境: サーバ SendAsync → Python websocket-client が 1900B 受信。
- **実 kabu STATION: 銘柄1458 の価格 89940.0 を data_feed が WebSocket push 経由で受信成功**
  (`SEND_TO_NETWORK 2bufs → CompleteAction bytes=1616/1893 → COMPLETE` を複数回確認)。

**導入済み**: `~/.wine/drive_c/windows/{system32,syswow64}/websocket.dll` ← v2.17
(64/32bit)。`data_feed.py` は変更不要(RSV1 パッチは実フレームが非圧縮のため
不要だが無害、残置)。

以下は解決前の調査記録(経緯・誤った推定を含む。歴史的資料として残す)。

---

## 1. ゴール

kabu STATION(au Kabucom証券のトレーディングアプリ、Windows専用、.NET Framework 4.x製)を
Linux + Wine 環境で動かし、そのローカルAPI(`http://localhost:18080/kabusapi/`)の
WebSocketエンドポイント(`ws://localhost:18080/kabusapi/websocket`)経由で配信される価格pushを受信できるようにする。

REST API(`/kabusapi/token`, `/kabusapi/register`, `/kabusapi/positions` 等)は
問題なく動作している。**問題はWebSocket pushの受信のみ**。

検証方法: kabu STATION起動直後、登録銘柄の直近価格が一度だけWebSocket経由で
配信される(この「一度きりのpush」が来るかどうかで動作確認する)。
**この配信はkabu STATION起動ごとに一回しか発生しない**ため、検証には毎回
kabu STATIONの完全な再起動+ログインが必要。

## 2. 環境

- Wine 11.0 (`/opt/wine-stable`)、パッケージは通常の wine-stable
- kabu STATION は .NET Framework 4.0.30319 (`Microsoft.NET\Framework64\v4.0.30319`)で動作する64bitプロセス
- kabu STATION 実行ファイル: `~/.wine/drive_c/users/kitamura/AppData/Local/kabuStation/KabuS.exe`
- 起動スクリプト: `~/bin/kabustation`
- Wineソースツリー(ビルド用、公式ビルドシステム経由でDLL/ドライバを再構築するために展開済み): `/tmp/wine-wine-11.0`
  (`WINEDLLOVERRIDES` 抜きで `wine --version` と一致する 11.0 を `./configure` 済み。
  32bit部分ビルドに `gcc-multilib`, `flex`, `bison` が必要だった)

## 3. 確定して修正済みの根本原因(2件)

### 3.1 `http.sys`: WebSocketアップグレード後にHTTPパーサーへ戻ってしまうバグ

**症状(修正前)**: WebSocketハンドシェイク(101 Switching Protocols)は成功するが、
クライアントが最初のフレーム(PINGなど)を送った瞬間、Wineの`http.sys`がそれを
「次のHTTPリクエスト」として構文解析しようとして失敗し、生の`HTTP/1.1 400 Bad Request`
テキストをソケットに書き戻す。または `HttpCancelHttpRequest` 呼び出し失敗と
組み合わさってkabu STATIONプロセスがクラッシュすることもあった。

**原因**: Wine純正の `dlls/http.sys/http.c` は、101レスポンス送出後も
`conn->available`を通常のHTTPリクエスト待ち状態のままにしてしまい、
`receive_data()` が届いたバイト列を常に `parse_request()` に渡してしまう。

**修正**: `conn` 構造体に `is_websocket` フラグと `body_irp_queue` を追加。
`http_send_response()` で101レスポンス検出時に `is_websocket=true` とし、
以後 `receive_data()` はHTTPパーサーを経由せず、受信データをそのまま
`HttpReceiveRequestEntityBody`/`HttpSendResponseEntityBody` の IRP キュー経由で
やり取りするようにした。

**該当ソース**: `~/wine_websocket_fix/http_ws.c`(Wine公式ビルドシステムで
`/tmp/wine-wine-11.0/dlls/http.sys/http.c` を差し替えてビルド)

**ビルド方法**:
```bash
cp ~/wine_websocket_fix/http_ws.c /tmp/wine-wine-11.0/dlls/http.sys/http.c
cd /tmp/wine-wine-11.0
make -C dlls/http.sys -j$(nproc)
# 成果物: dlls/http.sys/{x86_64,i386}-windows/http.sys
```

**導入場所**: `~/.wine/drive_c/windows/system32/drivers/http.sys`
(Wineの「native優先」検索により、ここに置くだけで自動的に使われる。
`WINEDLLOVERRIDES` の指定は不要)

**現在導入中のバイナリ**: `~/wine_websocket_fix/http_ws_v4_cancelreq.sys`
(md5: `11f0906a976ad3d3a8eedf0d4e2ddd0d`) — 3.2節のIOCTLも含む版

**検証結果**: WebSocket接続は安定して確立・維持されるようになり、
以前観測された400誤応答・クラッシュは完全に解消。これは確実な成果。

### 3.2 `httpapi.dll`: `HttpCancelHttpRequest` が stub のまま未実装

**症状(修正前)**: kabu STATIONがWebSocket送信を試みた直後、`HttpCancelHttpRequest`を
呼び出すが、Wineの`httpapi.dll`はこれを`stub`のまま実装しておらず、
`wine: Call from ... to unimplemented function httpapi.dll.HttpCancelHttpRequest, aborting`
というエラーになる(状況によりkabu STATIONプロセスがクラッシュすることもあった)。

**修正**:
- `dlls/http.sys/http.c` に新しいIOCTL `IOCTL_HTTP_CANCEL_REQUEST`(`0x805`)と
  `http_cancel_request()` を追加(該当コネクションの `body_irp_queue` をキャンセルする実装)。
- `include/wine/http.h` に `struct http_cancel_request_params { HTTP_REQUEST_ID id; }` を追加。
- `dlls/httpapi/httpapi_main.c` に `HttpCancelHttpRequest()` の実装を追加
  (`DeviceIoControl(queue, IOCTL_HTTP_CANCEL_REQUEST, ...)` を呼ぶだけ)。
- `dlls/httpapi/httpapi.spec` の `@ stub HttpCancelHttpRequest` を
  `@ stdcall HttpCancelHttpRequest(ptr int64 ptr)` に変更。

**ハマったポイント(重要)**: `WINEDLLOVERRIDES="httpapi=native"` で
`~/.wine/drive_c/windows/system32/httpapi.dll` に置く方式は**動作しない**。
Wine公式ビルドシステムで `-Wl,--wine-builtin` 付きでビルドしたPEは
「wine-builtin」形式であり、`native` ロード経路では
`status=c0000135 (STATUS_DLL_NOT_FOUND)` で読み込みに失敗する
(この状態でも `websocket.dll` だけは動く。websocket.dllは公式ビルドシステムでは
なく単独の `x86_64-w64-mingw32-gcc` で作っているため、genuineな native PE として
振る舞うから)。この状態でAPIステータスが赤(エラー)になった。

**正しい導入方法**: **Wine本体のインストールディレクトリ自体を置き換える**必要がある
(sudo権限が必要、システム全体・他のWineプレフィックスにも影響することに注意):
```bash
sudo cp ~/wine_websocket_fix/httpapi_v1_cancelreq.dll  /opt/wine-stable/lib/wine/x86_64-windows/httpapi.dll
sudo cp ~/wine_websocket_fix/httpapi32_v1_cancelreq.dll /opt/wine-stable/lib/wine/i386-windows/httpapi.dll
```
オリジナルのバックアップ:
`~/wine_websocket_fix/httpapi_ORIGINAL_backup_x86_64.dll` / `httpapi_ORIGINAL_backup_i386.dll`

この方式であれば `WINEDLLOVERRIDES` の指定は一切不要(デフォルトのbuiltin検索経路で
自動的に使われる)。

**検証結果**: `HttpCancelHttpRequest`は正常に呼び出され例外なく完了するようになった。
APIステータスは緑のまま安定。プロセスクラッシュも解消。

## 4. 未解決の問題: push送信が届かない

### 4.1 何を確定できたか

kabu STATION(`vk.cs`、ilspycmdで逆コンパイル)のWebSocket push送信は、
アプリ全体で**たった1箇所**しかない:

```csharp
// vk.cs 内、private void a(auu A_0, au2 A_1, SymbolDetail A_2, SymbolInfo A_3) メソッド内
ak.SendAsync(buffer, WebSocketMessageType.Text, endOfMessage: true, CancellationToken.None);
```

`ak` は `System.Net.WebSockets.WebSocket` 型のインスタンスフィールドで、
`AcceptWebSocketAsync` で確立された接続を保持する。この`SendAsync`は
`await` されない「fire-and-forget」呼び出し。

この呼び出しは.NET Framework自身の `System.Net.WebSockets.WebSocketBase`
(`System.dll` 内、Microsoft純正のマネージドコード)を経由し、最終的に
ネイティブの `websocket.dll`(WebSocket Protocol Component API、Wine上では
我々の自作シム `~/wine_websocket_fix/websocket_shim_v2*.c`)を呼び出す。

流れ:
1. `WebSocketProtocolComponent.WebSocketSend()` (native) — ペイロードをキューに積む
2. `WebSocketProtocolComponent.WebSocketGetAction()` (native) — 送信すべきバッファ情報を返す
3. **.NET側のマネージドラッパーが、GetActionの戻り値に対して直後に
   `WebSocketBuffer.ValidateNativeBuffers()` を呼び、ここで必ず
   `System.AccessViolationException` を投げる**(`COMPlus_ThrowUnobservedTaskExceptions=1`
   で例外を強制的に表面化させて確認済み。以下が実際のスタックトレース):

```
System.AccessViolationException: Attempted to read or write protected memory.
   at System.Net.WebSockets.WebSocketBuffer.ValidateNativeBuffers(Action, BufferType, Buffer[], UInt32)
   at System.Net.WebSockets.WebSocketProtocolComponent.WebSocketGetAction(WebSocketBase, ActionQueue, Buffer[], UInt32&, Action&, BufferType&, IntPtr&)
   at System.Net.WebSockets.WebSocketBase.WebSocketOperation.<Process>d__19.MoveNext()
   ...
   at System.Net.WebSockets.WebSocketBase.<SendAsyncCore>d__47.MoveNext()
```

このため、`ak.SendAsync()` のTaskは常にfaultedになり、await されていないため
例外は握りつぶされ、kabu STATIONのアプリ側ログにも何も残らない
(通常運用ではプロセスもクラッシュしない。`COMPlus_ThrowUnobservedTaskExceptions=1`
を設定した場合のみ、GCのファイナライザースレッドが未observedな例外を再送出し、
プロセスが終了する)。

### 4.2 `ValidateNativeBuffers` の実際の契約(Microsoft公式 referencesource で確認済み)

ソース: https://github.com/microsoft/referencesource/blob/main/System/net/System/Net/WebSockets/WebSocketBuffer.cs

各バッファについて、以下のいずれかを満たさないと `AccessViolationException`:
- **`IsPinnedSendPayloadBuffer`**: `m_SendBufferState == SendBufferState.SendPayloadSpecified`
  (0=None, 1=SendPayloadSpecified) **かつ** バッファのアドレス範囲が
  `WebSocketSend`に渡された `pBuffer`(.NETがピン留めした配列)の範囲内と完全一致。
- **`IsNativeBuffer`**: バッファのアドレス範囲が、`WebSocketCreateServerHandle`の
  `pProperties`中の `WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE` (`Type=3`)で
  渡される「ネイティブバッファ」領域(`m_StartAddress`〜`m_EndAddress`)の範囲内。

`WEB_SOCKET_BUFFER` の実体は union:
```csharp
union WEB_SOCKET_BUFFER {
    struct { PBYTE pbBuffer; ULONG ulBufferLength; } Data;
    struct { PBYTE pbReason; ULONG ulReasonLength; USHORT usStatus; } CloseStatus;
}
```
(我々のフラットな3フィールド構造体 `{pbBuffer, ulBufferLength, usStatus}` と
メモリレイアウトは偶然完全一致するため、これ自体はバグではなかった)

### 4.3 試した修正と実験結果

| バージョン | 変更内容 | 結果 |
|---|---|---|
| v2.8以前 | ヘッダー(自前malloc)+ペイロード(コピー)の2バッファ | Abort |
| v2.9〜v2.11 | ヘッダー+ペイロードを自前mallocで連結した単一バッファ | Abort (AccessViolationException 確認) |
| v2.12 | `pBuffer->pbBuffer` の直前にヘッダーを書き込み単一バッファ化 | Abort (同一例外) |
| v2.13 | `WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE` で渡される正しい
  native_buf にヘッダーを書き込み、ペイロードは`pBuffer`を完全に無改変で
  2バッファ返却(仕様書通りの正しい実装) | **まだAbort、同一例外** |
| v2.14 | v2.13 + 詳細アドレスログ追加 | 同上。診断用 |
| v2.15 | [診断用] ヘッダーのみ1バッファ返却(ペイロードを含めない) | **クラッシュしない**(プロセス生存)。ただし当然pushは届かない |
| v2.16 | ヘッダー+ペイロードを **native_buf 内に連結コピー** した単一バッファを返却(IsNativeBuffer で通す狙い) | **NG。GetAction 直後に AbortHandle、その後 KabuS.exe がクラッシュ**(v2.14 はクラッシュしなかった) |

**v2.15の結果から、`buffer[1]`(ペイロード)側の検証が失敗していると切り分けられた。**

### 4.3.1 v2.16 の結論(2026-07-07 追試)——native_buf コピー方式は不可

Microsoft referencesource で確定した実測値:
- kabu STATION は `AcceptWebSocketAsync(receiveBufferSize=16384)` 相当で接続。
  → `m_ReceiveBufferSize=16384`, `m_SendBufferSize=16`(最小値),
    `nativeBufferSize = 16384 + 16 + 144 = 16544` (= 実測 native_buf 長と完全一致)。
  → `GetMaxBufferSize() = max(16384,16) = 16384`。
- **`m_SendBufferSize` は 16 バイトしかない**。これが決定的:
  実 Windows の websocket.dll がペイロードを「コピーせず」ピン留めユーザーバッファを
  そのまま buffer[1] で返すのは、ネイティブ送信用スクラッチ領域が 16 バイトと極小で
  大きなペイロードを置けない設計だからである。
- v2.16 はペイロード(~1889B)を native_buf の先頭にコピーしたが、この領域は
  受信バッファ等 .NET/コンポーネントが別用途で使うメモリと重なり、**メモリ破壊 →
  KabuS.exe クラッシュ** を招いた(v2.14 は書き込まないのでクラッシュしなかった)。
- ValidateNativeBuffers のサイズ検査自体は `1893 < 16384` で通るはずだが、
  破壊の副作用でプロセスが落ちるため実用不可。

**帰結: ペイロードは「ピン留めユーザーバッファをそのまま buffer[1] で返す」
(= v2.14 方式)以外に選択肢がない。よって IsPinnedSendPayloadBuffer を
成立させる = `m_SendBufferState==SendPayloadSpecified` を検証時に満たすことが
唯一の正攻法であり、これが Wine 下で成立しないことが本件の唯一かつ核心の壁。**

### 4.3.2 送信フロー精査で分かったこと(referencesource WebSocketBase.cs)

`WebSocketOperation.Process` を精読した結果:
- `PinSendBuffer`(`m_SendBufferState` を SendPayloadSpecified に設定)→ `WebSocketSend`
  → 最初の `WebSocketGetAction`(ここで ValidateNativeBuffers)までは
  **すべて同一スレッド・`Monitor.Enter(SessionHandle)` ロック保持下で同期実行**され、
  間に `await`・スレッド切替は一切入らない(await は SendToNetwork の
  ネットワーク書込み時に初めて発生)。
- つまりマネージド設計上は検証時点で必ず `m_SendBufferState==SendPayloadSpecified`
  かつ `buffer[1]` のアドレスも `m_PinnedSendBuffer` と一致するはずで、
  AccessViolationException は「起き得ない」。にもかかわらず Wine 下では起きる。
- したがって残る原因は **Wine の .NET Framework 実行環境における
  Interlocked / volatile のメモリ可視性、GCHandle ピン留めアドレスの扱い、
  または非同期基盤の移植差** といった低レベル互換性問題に絞られる。
  シム(websocket.dll)側からは回避不能。
`buffer[0]`(ヘッダー、native_buf内)だけなら例外は起きない。

### 4.4 実メモリ調査(gdb)での確認事項

`gdb -p <KabuS.exe PID>` で実行中プロセスにアタッチし
(`kernel.yama.ptrace_scope=0` に一時変更が必要。**作業後は必ず1に戻すこと**)、
ログに出力した実際のペイロードアドレス値をプロセスメモリ全体から検索:

```
find /8 <rw-pマップの開始>, <終了>, <ペイロードアドレスの値>
```

→ 1箇所ヒット。そのアドレス周辺をダンプすると:

```
offset+0x00: ペイロード開始アドレスと完全一致 (m_PinnedSendBufferStartAddress と推定)
offset+0x08: ペイロード終了アドレスと完全一致 (m_PinnedSendBufferEndAddress と推定)
offset+0x10: int32 = 16384 (バッファサイズと推定)
offset+0x14: int32 = 16
offset+0x18: int32 = 0   ← ここが m_SendBufferState だとすると "None"(期待値は1のはず)
offset+0x1C: int32 = 6
```

**アドレス一致は確認できた**(=`IsPinnedSendPayloadBuffer`のアドレス比較条件は
満たされているはず)。しかし `m_SendBufferState` とみられる位置に "0"(None)が
見えており、これが本当にそのフィールドであれば、.NET側が「送信ペイロード
指定済み」の内部状態を維持できていないことになる。

**注意**: これはシンボル情報なしの手作業でのオフセット推定であり、
**100%の確証はない**。referencesourceのフィールド宣言順序
(`m_PinnedSendBufferStartAddress`, `m_PinnedSendBufferEndAddress` の直後に
`m_SendBufferState`)と一致してはいるが、CLRは必ずしも宣言順通りに
フィールドを配置するとは限らない。

### 4.5 未解決の核心

`m_SendBufferState` は完全にマネージドコード側([WebSocketBase.SendOperation]の
`Initialize()`→`CreateBuffer()`→`PinSendBuffer()`)で設定され、
`WebSocketSend`のネイティブ呼び出し**直前**に同期的にセットされるはずで、
`ak.SendAsync()`→`Initialize()`→`WebSocketSend`→(即座に)`GetAction`という
一直線の同期呼び出しの間に、途中で状態がリセットされる理由が見当たらない
(`ReleasePinnedSendBuffer()`は送信完了後の`Cleanup()`でのみ呼ばれるはずで、
このタイミングでは早すぎる)。

**つまり「なぜWine環境下でこの状態が(もしそうだとして)Noneになってしまうのか」
は特定できていない。** これはWineの.NET Framework実行環境(スレッドプール、
非同期処理の互換性等)の深い部分に起因する可能性があるが、確証には
シンボル・型情報を解釈できる実際の.NETデバッガ(WinDbg + SOS拡張、または
同等のツール)でのライブ検証が必要。

## 5. 現在のファイル状態(2026-07-07時点)

### インストール済み(有効化されている修正)
- `~/.wine/drive_c/windows/system32/drivers/http.sys` ← `http_ws_v4_cancelreq.sys` (3.1節の修正)
- `/opt/wine-stable/lib/wine/{x86_64,i386}-windows/httpapi.dll` ← `httpapi_v1_cancelreq.dll`/`httpapi32_v1_cancelreq.dll` (3.2節の修正、システム全体に影響)
- `~/.wine/drive_c/windows/{system32,syswow64}/websocket.dll` ← v2.14
  (4節の通り根本解決には至っていないが、これまでで最も正しい実装。
  通常運用でクラッシュはしない)
- `~/bin/kabustation`: `WINEDLLOVERRIDES="websocket=native"` のみ指定
  (httpapi.dll は 3.2 節の理由により override 不要)

### ソースコード一式
`~/wine_websocket_fix/` 以下(**ユーザー指示によりクリーンアップ未実施、
大量の実験用ファイルが残存**):
- `http_ws.c` — http.sys 修正版ソース(3.1+3.2両方の修正を含む最新版)
- `websocket_shim_v2xx.c` — websocket.dll シムのソース各バージョン
  (v215が最新の診断版、v214が実運用上ベストな2バッファ正式版)
- `httpapi_v1_cancelreq.dll` / `httpapi32_v1_cancelreq.dll` — httpapi.dll修正版
- `httpapi_ORIGINAL_backup_*.dll` — 元のhttpapi.dllバックアップ
- `wine_http_h_orig.h` / `httpapi_main_orig.c` / `httpapi_orig.spec` — 元のWineソースバックアップ

### Wineソースツリー
`/tmp/wine-wine-11.0` — configure済み(`--enable-archs=i386,x86_64`、多数の
`--without-*`でGUI/メディア関連を無効化)。**`/tmp`配下なので再起動等で
消える可能性がある**。再度必要な場合は同じ手順でWine 11.0のソースを
展開・configureし直すこと(3.1節参照、`gcc-multilib`/`flex`/`bison`が必要)。

## 6. 次回再開する場合の推奨手順

1. まず「4.4節の推定が正しいか」を確実にするため、`dotnet-dump`/`SOS`相当の
   ツールで `m_SendBufferState` フィールドの値を型情報付きで確認する方法を検討する。
   - Wine上の.NET FrameworkプロセスはWindows PEのDACを使うため、Linux版
     `dotnet-dump`(.NET Core/5+用)は直接使えない可能性が高い。
   - 代替案: 本物のWindows環境(または実機)で同じ箇所にブレークポイントを張り、
     `m_SendBufferState`の実際の値・オフセットを確認し、その情報を元にWine側の
     メモリを解釈し直す。
   - あるいは、mdbg(古い.NET Framework SDK付属の管理下デバッガ)がWine上で
     動作するか試す。
2. それでも特定できない場合、実用上の代替案として **REST API
   (`/kabusapi/board/{symbol}`)のポーリング**に切り替えることを検討する
   (push配信ではなく定期ポーリングだが、確実に動作する)。
3. `data_feed.py`(`~/work/stock/src/stock/autotrade/data_feed.py`)には
   permessage-deflate(RSV1)フレームをwebsocket-clientライブラリで
   受信するためのパッチが既に組み込み済み(`_patch_websocket_client_for_permessage_deflate`)。
   これは実際の外部クライアント接続では使われていない可能性が高い
   (4.1節参照、.NET側は独自にwebsocket.dllを呼ぶため)が、実害はないので
   残したままでよい。

## 7. 検証用スクリプト・コマンド集

### kabu STATION起動(トレース付き)
```bash
mv ~/.wine/drive_c/ws_shim.log ~/.wine/drive_c/ws_shim.log.old 2>/dev/null
DISPLAY=:20 XAUTHORITY=~/.Xauthority WINEDEBUG=+http,+websocket \
  nohup ~/bin/kabustation > /tmp/kabu_trace.log 2>&1 &
# → リモートデスクトップから手動ログインが必要
```

### 完全クリーン再起動(キャッシュされたwineserverの影響を避ける)
```bash
KPID=$(pgrep -x "KabuS.exe"); [ -n "$KPID" ] && kill $KPID
sleep 2
wineserver -k
sleep 2
# 上記の起動コマンドを再実行
```
**注意**: `pkill -f "KabuS.exe"` は使わないこと。呼び出し元シェルのコマンドライン
自体に文字列 "KabuS.exe" が含まれる場合、自分自身にもマッチしてしまうことがある。
`pgrep -x "KabuS.exe"`(完全一致、フルコマンドラインでなくプロセス名のみ)を使うこと。

### API確認・push受信テスト
```python
import sys, time
sys.path.insert(0, "src")
sys.path.insert(0, "/home/kitamura/work/data_fetcher/src")
import stock
config_path = stock.constants.PROJECT_ROOT / "configs" / "autotrade.json"
config = stock.autotrade.AutoTradeConfig.model_validate_json(config_path.read_text()).to_absolute()
trader = stock.autotrade.AutoTrader(config)
trader._order_mgr.refresh_token()
trader._data_feed.refresh_headers(trader._order_mgr.headers())
trader._data_feed.start()
for i in range(15):
    price = trader._data_feed.get_current_price("1458")
    print(f"t+{i}s price={price}")
    if price:
        print("SUCCESS")
        break
    time.sleep(1)
```

### 未observed例外を強制的に表面化させる(プロセスは終了する)
```bash
COMPlus_ThrowUnobservedTaskExceptions=1 <上記の起動コマンド>
# 送信テスト後、60秒程度待つとGCが走り例外がイベントログ経由でトレースに出力される
```
`/tmp/kabu_trace.log` 内で `grep "AccessViolationException"` すると確認できる。
