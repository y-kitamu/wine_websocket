/*
 * Wine用 websocket.dll 実装 v2.17
 *
 * ★★★ 根本原因を特定し修正 (2026-07-07) ★★★
 *
 * v2.15 までの症状: kabu STATION の WebSocket push (server->client SendAsync) が
 *   .NET Framework の WebSocketBuffer.ValidateNativeBuffers() で
 *   AccessViolationException となり毎回 WebSocketAbortHandle → push が届かない。
 *
 * 根本原因(標準ローカルC#再現環境 + reflection で確定):
 *   ValidateNativeBuffers は SEND_TO_NETWORK で返す各バッファについて
 *     (a) IsPinnedSendPayloadBuffer(=ピン留めペイロード) または
 *     (b) IsNativeBuffer([m_StartAddress, m_EndAddress] 内)
 *   を要求する。payload は (a) で常に通っていた。**問題は header(buffer[0])**:
 *   ヘッダーを書き込む native_buf を、WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE
 *   プロパティの pvValue "そのもの" として扱っていたが、実際には
 *   **pvValue は「ネイティブバッファへのポインタが格納された場所」を指す**
 *   (受信/送信バッファサイズ等の他プロパティと同じく pvValue は値の格納位置)。
 *   そのため native_buf が [m_StartAddress, m_EndAddress] の外(m_PropertyBuffer
 *   付近、+約33KB)を指し、header が IsNativeBuffer に失敗して AVE となっていた。
 *   (reflection 実測: raw pvValue=0x..960a0 / *(pvValue)=0x..8dff8=m_StartAddress)
 *
 * 修正 (extract_allocated_buffer):
 *   native_buf = *(BYTE **)pProperties[i].pvValue;   // ★参照外し★
 *   これだけで header が正しいネイティブ領域に入り、2バッファ
 *   [header(native_buf), payload(ピン留め pBuffer そのまま)] が検証を通過。
 *   ローカル再現環境で server SendAsync → Python websocket-client 受信を確認済み。
 *
 * v2.8 からの変更:
 *   [BUGFIX] WebSocketGetAction の SEND_TO_NETWORK_ACTION で、フレームヘッダーと
 *            ペイロードを別々の2バッファとして返していたのを、連結した単一バッファ
 *            (send_combined) を返すように変更。
 *            WINEDEBUG=+http,+websocket でトレースしたところ、kabu STATION は
 *            SEND_TO_NETWORK_ACTION を受け取った直後、実際の送信 IOCTL
 *            (HttpSendResponseEntityBody) を一度も呼ばずに WebSocketAbortHandle を
 *            呼んでいた。.NET 側が2バッファ形式を想定しておらず例外を投げている
 *            可能性が高いため、実 Windows の websocket.dll と同様に単一バッファで
 *            返すよう修正。
 *
 * v2.6 からの変更:
 *   [DIAG] WebSocketGetAction / WebSocketReceive を常にログ出力するよう変更。
 *          v2.6 では条件分岐内にのみログがあったため、
 *          NO_ACTION や pDataBuffers=NULL のケースが記録されなかった。
 *          v2.7 では関数先頭で無条件ログを出す（最初100回まで）。
 *
 *          また send_phase=HEADER の際に reqCount<1 でも SEND_TO_NETWORK_ACTION
 *          を返していたバグを修正。reqCount<1 の場合は NO_ACTION を返す。
 *
 * ログファイル: C:\ws_shim.log (= ~/.wine/drive_c/ws_shim.log)
 * ログ抑制:    WS_SHIM_QUIET=1 で stderr を抑制 (ファイルには常に記録)
 */

#include <windows.h>
#include <wincrypt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ───────────────────── デバッグログ ───────────────────── */
static int s_quiet = -1;
static FILE *s_logfile = NULL;

static void ws_log_open(void) {
    if (s_logfile) return;
    s_logfile = fopen("C:\\ws_shim.log", "a");
    if (!s_logfile)
        s_logfile = fopen("Z:\\tmp\\ws_shim.log", "a");
}

static void ws_log(const char *fmt, ...)
{
    if (s_quiet < 0) {
        const char *e = getenv("WS_SHIM_QUIET");
        s_quiet = (e && e[0] == '1') ? 1 : 0;
    }

    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

    if (!s_quiet) {
        fprintf(stderr, "[websocket.dll v2.17] ");
        vfprintf(stderr, fmt, ap);
        fflush(stderr);
    }

    ws_log_open();
    if (s_logfile) {
        fprintf(s_logfile, "[websocket.dll v2.17] ");
        vfprintf(s_logfile, fmt, ap2);
        fflush(s_logfile);
    }

    va_end(ap2);
    va_end(ap);
}

/* ───────────────────── 型定義 ───────────────────── */
typedef PVOID WEB_SOCKET_HANDLE;

typedef enum {
    WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE       = 0,
    WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE          = 1,
    WEB_SOCKET_DISABLE_MASKING_PROPERTY_TYPE           = 2,
    WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE          = 3,
    WEB_SOCKET_DISABLE_UTF8_VERIFICATION_PROPERTY_TYPE = 4,
    WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE        = 5,
    WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE        = 6
} WEB_SOCKET_PROPERTY_TYPE;

typedef struct {
    WEB_SOCKET_PROPERTY_TYPE Type;
    PVOID pvValue;
    ULONG ulValueSize;
} WEB_SOCKET_PROPERTY, *PWEB_SOCKET_PROPERTY;
typedef const WEB_SOCKET_PROPERTY* PCWEBSOCKET_PROPERTY;

typedef struct {
    PCSTR pcName;
    ULONG ulNameLength;
    PCSTR pcValue;
    ULONG ulValueLength;
} WEB_SOCKET_HTTP_HEADER, *PWEB_SOCKET_HTTP_HEADER;
typedef const WEB_SOCKET_HTTP_HEADER* PCWEB_SOCKET_HTTP_HEADER;

/* v2.10: 実 Windows websocket.h の値に合わせる (Microsoft Learn 確認済み)。
 * 従来 0〜6 の連番で定義していたため、.NET から渡される実際の値
 * (例: UTF8_MESSAGE=0x80000000) と一致せず、build_frame_header() が
 * 常に default (binary) 扱いになっていた。 */
typedef enum {
    WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE          = 0x80000000,
    WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE         = 0x80000001,
    WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE        = 0x80000002,
    WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE       = 0x80000003,
    WEB_SOCKET_CLOSE_BUFFER_TYPE                 = 0x80000004,
    WEB_SOCKET_PING_PONG_BUFFER_TYPE             = 0x80000005,
    WEB_SOCKET_UNSOLICITED_PONG_BUFFER_TYPE      = 0x80000006
} WEB_SOCKET_BUFFER_TYPE;

typedef struct {
    PBYTE  pbBuffer;
    ULONG  ulBufferLength;
    USHORT usStatus;
} WEB_SOCKET_BUFFER;

typedef enum {
    WEB_SOCKET_NO_ACTION                         = 0,
    WEB_SOCKET_SEND_TO_NETWORK_ACTION            = 1,
    WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION     = 2,
    WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION       = 3,
    WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION  = 4
} WEB_SOCKET_ACTION;

typedef enum {
    WEB_SOCKET_ALL_ACTION_QUEUE     = 0,
    WEB_SOCKET_SEND_ACTION_QUEUE    = 1,
    WEB_SOCKET_RECEIVE_ACTION_QUEUE = 2
} WEB_SOCKET_ACTION_QUEUE;

/* ───────────────────── SHA1 + Base64 ───────────────────── */
/* RFC 6455 マジック文字列 */
static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/*
 * Sec-WebSocket-Accept の計算:
 *   SHA1(key || MAGIC) を Base64 エンコード
 * 戻り値: 成功=1, 失敗=0
 */
static int compute_ws_accept(const char *key, char *out_b64, DWORD out_b64_len)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s%s", key, WS_MAGIC);
    if (n <= 0 || n >= (int)sizeof(buf)) return 0;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hashVal[20];
    DWORD hashLen = 20;
    int ok = 0;

    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash))
        goto done;
    if (!CryptHashData(hHash, (BYTE *)buf, (DWORD)n, 0))
        goto done;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hashVal, &hashLen, 0))
        goto done;
    if (!CryptBinaryToStringA(hashVal, hashLen,
                               CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                               out_b64, &out_b64_len))
        goto done;
    ok = 1;

done:
    if (hHash) CryptDestroyHash(hHash);
    if (hProv) CryptReleaseContext(hProv, 0);
    return ok;
}

/*
 * Sec-WebSocket-Key ヘッダー値をリクエストヘッダー配列から検索
 */
static const char *find_ws_key(PCWEB_SOCKET_HTTP_HEADER pHeaders, ULONG count)
{
    static const char TARGET[] = "sec-websocket-key";
    for (ULONG i = 0; i < count; i++) {
        if (!pHeaders[i].pcName) continue;
        ULONG len = pHeaders[i].ulNameLength;
        if (len != 17) continue; /* len("sec-websocket-key") == 17 */
        char tmp[32] = {0};
        if (len >= sizeof(tmp)) continue;
        for (ULONG j = 0; j < len; j++)
            tmp[j] = (char)tolower((unsigned char)pHeaders[i].pcName[j]);
        if (strcmp(tmp, TARGET) == 0)
            return pHeaders[i].pcValue;
    }
    return NULL;
}

/* ───────────────────── 状態機械 ───────────────────── */
#define SEND_PHASE_IDLE           0
#define SEND_PHASE_HEADER         1
#define SEND_PHASE_PAYLOAD        2
#define SEND_PHASE_COMPLETE       3
#define SEND_PHASE_AWAITING_ACK   4  /* v2.10: SEND_TO_NETWORK_ACTION 返却済み、
                                        WebSocketCompleteAction 待ち */

#define RECV_PHASE_IDLE      0
#define RECV_PHASE_PENDING   1
#define RECV_PHASE_NETWORK   2
#define RECV_PHASE_COMPLETE  3

/* Sec-WebSocket-Accept ヘッダー名 (静的に保持) */
static const char HNAME_ACCEPT[]     = "Sec-WebSocket-Accept";
static const char HNAME_CONNECTION[] = "Connection";
static const char HNAME_UPGRADE[]    = "Upgrade";
static const char HNAME_EXTENSIONS[] = "Sec-WebSocket-Extensions";
static const char HVAL_UPGRADE[]     = "websocket";
static const char HVAL_CONNECTION[]  = "Upgrade";

typedef struct WS_STATE {
    BOOL is_server;
    CRITICAL_SECTION cs;

    /* v2.13: WebSocketCreateServerHandle/ClientHandle の pProperties に含まれる
     * WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE で渡される、.NET 側が
     * 確保・ピン留めした「ネイティブバッファ」領域。実 .NET Framework の
     * WebSocketBuffer.ValidateNativeBuffers() は、SEND_TO_NETWORK_ACTION で
     * 返す各バッファについて、
     *   (a) ペイロード送信バッファそのもの(WebSocketSend に渡された
     *       pBuffer と完全に同一のポインタ・長さ)であるか、
     *   (b) この native_buf の範囲内のポインタであるか
     * のいずれかでなければ AccessViolationException を投げる
     * (referencesource の WebSocketBuffer.cs で確認済み)。
     * よってフレームヘッダーはこの native_buf 内に書き込み、
     * ペイロードは受け取った pBuffer をそのまま返す必要がある。 */
    BYTE  *native_buf;
    ULONG  native_buf_len;

    /* 送信状態 */
    int                    send_phase;
    WEB_SOCKET_BUFFER_TYPE send_type;
    WEB_SOCKET_BUFFER      send_buf;
    PVOID                  send_app_ctx;
    BYTE                   frame_hdr[10];
    ULONG                  frame_hdr_len;
    WEB_SOCKET_BUFFER      send_bufs[2];   /* v2.13: [0]=ヘッダー(native_buf内) [1]=ペイロード(元のまま) */

    /* 受信状態 */
    int               recv_phase;
    WEB_SOCKET_BUFFER recv_buf;
    PVOID             recv_app_ctx;
    ULONG             recv_bytes_transferred;
    LONG              recv_log_count;

    /* 診断カウンタ */
    LONG              getaction_log_count; /* 最初の N 回だけ詳細ログ */

    /* サーバーハンドシェイク応答ヘッダー */
    char                    accept_val[64];     /* Base64 エンコード済み Accept 値 */
    char                    ext_val[256];       /* Sec-WebSocket-Extensions 値 (カンマ区切り) */
    WEB_SOCKET_HTTP_HEADER  srv_headers[4];     /* BeginServerHandshake で返すヘッダー (最大4) */
    ULONG                   srv_header_count;   /* 0 = 未計算 */
} WS_STATE;

static WS_STATE *alloc_state(BOOL is_server) {
    WS_STATE *s = (WS_STATE *)calloc(1, sizeof(WS_STATE));
    if (!s) return NULL;
    s->is_server  = is_server;
    s->send_phase = SEND_PHASE_IDLE;
    s->recv_phase = RECV_PHASE_IDLE;
    InitializeCriticalSection(&s->cs);
    return s;
}

static void free_state(WS_STATE *s) {
    if (!s) return;
    DeleteCriticalSection(&s->cs);
    /* v2.12: send_combined はもう自前確保ではなく .NET 側バッファへの
     * ポインタなので解放しない。 */
    free(s);
}

/* RFC 6455 フレームヘッダー構築 */
static ULONG build_frame_header(BYTE *hdr, WEB_SOCKET_BUFFER_TYPE type, ULONG plen) {
    BYTE opcode;
    switch (type) {
        case WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:    opcode = 0x81; break;
        case WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:   opcode = 0x01; break;
        case WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:  opcode = 0x82; break;
        case WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE: opcode = 0x02; break;
        case WEB_SOCKET_CLOSE_BUFFER_TYPE:           opcode = 0x88; break;
        case WEB_SOCKET_PING_PONG_BUFFER_TYPE:       opcode = 0x8A; break;
        default:                                     opcode = 0x82; break;
    }
    hdr[0] = opcode;
    if (plen <= 125) {
        hdr[1] = (BYTE)plen;
        return 2;
    } else if (plen <= 65535) {
        hdr[1] = 126;
        hdr[2] = (BYTE)(plen >> 8);
        hdr[3] = (BYTE)(plen & 0xFF);
        return 4;
    } else {
        hdr[1] = 127;
        hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0;
        hdr[6] = (BYTE)((plen >> 24) & 0xFF);
        hdr[7] = (BYTE)((plen >> 16) & 0xFF);
        hdr[8] = (BYTE)((plen >>  8) & 0xFF);
        hdr[9] = (BYTE)( plen        & 0xFF);
        return 10;
    }
}

/* ───────────────────── DllMain ───────────────────── */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH)
        ws_log("DLL_PROCESS_ATTACH: websocket.dll v2.17 loaded\n");
    return TRUE;
}

/* ───────────────────── ハンドシェイク ───────────────────── */
static const char s_version_name[]  = "Sec-WebSocket-Version";
static const char s_version_value[] = "13";
static WEB_SOCKET_HTTP_HEADER s_client_headers[] = {
    { s_version_name, 21, s_version_value, 2 }
};

/* v2.13: pProperties から WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE を取り出し、
 * .NET が確保・ピン留めしたネイティブバッファ領域を記録する。 */
static void extract_allocated_buffer(WS_STATE *s, PCWEBSOCKET_PROPERTY pProperties, ULONG ulPropertyCount)
{
    /* v2.14: 診断のため全プロパティを列挙してログ出力する */
    for (ULONG i = 0; i < ulPropertyCount; i++) {
        ws_log("  property[%lu]: Type=%d pvValue=%p ulValueSize=%lu\n",
               (unsigned long)i, (int)pProperties[i].Type,
               pProperties[i].pvValue, (unsigned long)pProperties[i].ulValueSize);
    }
    for (ULONG i = 0; i < ulPropertyCount; i++) {
        if (pProperties[i].Type == WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE) {
            /* ★根本修正 (v2.17)★
             * pvValue は「ネイティブバッファへのポインタが格納された場所」を指す
             * (他プロパティ同様 pvValue=値の格納位置)。参照外しして実体アドレスを得る。
             * これを怠ると native_buf が [m_StartAddress,m_EndAddress] 外を指し、
             * header が IsNativeBuffer に失敗して AccessViolationException となる。 */
            s->native_buf     = *(BYTE **)pProperties[i].pvValue;
            s->native_buf_len = pProperties[i].ulValueSize;
            ws_log("extract_allocated_buffer: native_buf=%p end=%p len=%lu (deref of pvValue=%p)\n",
                   (void*)s->native_buf, (void*)(s->native_buf + s->native_buf_len),
                   (unsigned long)s->native_buf_len, pProperties[i].pvValue);
            return;
        }
    }
    ws_log("extract_allocated_buffer: WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE not found (nProps=%lu)\n",
           (unsigned long)ulPropertyCount);
}

HRESULT WINAPI WebSocketCreateClientHandle(
    PCWEBSOCKET_PROPERTY pProperties, ULONG ulPropertyCount,
    WEB_SOCKET_HANDLE *phWebSocket)
{
    if (!phWebSocket) return E_INVALIDARG;
    WS_STATE *s = alloc_state(FALSE);
    if (!s) return E_OUTOFMEMORY;
    if (pProperties && ulPropertyCount)
        extract_allocated_buffer(s, pProperties, ulPropertyCount);
    *phWebSocket = (WEB_SOCKET_HANDLE)s;
    ws_log("WebSocketCreateClientHandle → handle=%p\n", (void*)*phWebSocket);
    return S_OK;
}

HRESULT WINAPI WebSocketCreateServerHandle(
    PCWEBSOCKET_PROPERTY pProperties, ULONG ulPropertyCount,
    WEB_SOCKET_HANDLE *phWebSocket)
{
    if (!phWebSocket) return E_INVALIDARG;
    WS_STATE *s = alloc_state(TRUE);
    if (!s) return E_OUTOFMEMORY;
    if (pProperties && ulPropertyCount)
        extract_allocated_buffer(s, pProperties, ulPropertyCount);
    *phWebSocket = (WEB_SOCKET_HANDLE)s;
    ws_log("WebSocketCreateServerHandle → handle=%p\n", (void*)*phWebSocket);
    return S_OK;
}

void WINAPI WebSocketDeleteHandle(WEB_SOCKET_HANDLE hWebSocket) {
    ws_log("WebSocketDeleteHandle: handle=%p\n", hWebSocket);
    free_state((WS_STATE *)hWebSocket);
}

void WINAPI WebSocketAbortHandle(WEB_SOCKET_HANDLE hWebSocket) {
    ws_log("WebSocketAbortHandle: handle=%p\n", hWebSocket);
    WS_STATE *s = (WS_STATE *)hWebSocket;
    if (!s) return;
    EnterCriticalSection(&s->cs);
    s->send_phase = SEND_PHASE_IDLE;
    s->recv_phase = RECV_PHASE_IDLE;
    LeaveCriticalSection(&s->cs);
}

HRESULT WINAPI WebSocketBeginClientHandshake(
    WEB_SOCKET_HANDLE hWebSocket,
    PCSTR *pszSubProtocols, ULONG ulSubProtocolCount,
    PCSTR *pszExtensions,   ULONG ulExtensionCount,
    PCWEB_SOCKET_HTTP_HEADER pInitialHeaders, ULONG ulInitialHeaderCount,
    PWEB_SOCKET_HTTP_HEADER *ppAdditionalHeaders, ULONG *pulAdditionalHeaderCount)
{
    (void)hWebSocket; (void)pszSubProtocols; (void)ulSubProtocolCount;
    (void)pszExtensions; (void)ulExtensionCount;
    (void)pInitialHeaders; (void)ulInitialHeaderCount;
    if (ppAdditionalHeaders)      *ppAdditionalHeaders      = s_client_headers;
    if (pulAdditionalHeaderCount) *pulAdditionalHeaderCount = 1;
    ws_log("WebSocketBeginClientHandshake: handle=%p\n", hWebSocket);
    return S_OK;
}

HRESULT WINAPI WebSocketEndClientHandshake(
    WEB_SOCKET_HANDLE hWebSocket,
    PCWEB_SOCKET_HTTP_HEADER pResponseHeaders, ULONG ulReponseHeaderCount,
    ULONG *pulSelectedExtensions, ULONG *pulSelectedExtensionCount,
    ULONG *pulSelectedSubProtocol)
{
    (void)hWebSocket; (void)pResponseHeaders; (void)ulReponseHeaderCount;
    if (pulSelectedExtensions)     *pulSelectedExtensions     = 0;
    if (pulSelectedExtensionCount) *pulSelectedExtensionCount = 0;
    if (pulSelectedSubProtocol)    *pulSelectedSubProtocol    = 0;
    ws_log("WebSocketEndClientHandshake: handle=%p\n", hWebSocket);
    return S_OK;
}

/*
 * WebSocketBeginServerHandshake (v2.7 KEY FIX)
 *
 * 実際の Windows websocket.dll と同じように:
 *   Sec-WebSocket-Accept, Connection, Upgrade
 * を返すことで .NET の HTTP サーバーが WebSocket モードに切り替わるようにする。
 */
HRESULT WINAPI WebSocketBeginServerHandshake(
    WEB_SOCKET_HANDLE hWebSocket,
    PCSTR pszSubProtocolSelected,
    PCSTR *pszExtensionSelected, ULONG ulExtensionSelectedCount,
    PCWEB_SOCKET_HTTP_HEADER pRequestHeaders, ULONG ulRequestHeaderCount,
    PWEB_SOCKET_HTTP_HEADER *ppAdditionalHeaders, ULONG *pulAdditionalHeaderCount)
{
    (void)pszSubProtocolSelected;

    WS_STATE *s = (WS_STATE *)hWebSocket;

    /* デフォルト: 空レスポンス */
    if (ppAdditionalHeaders)      *ppAdditionalHeaders      = NULL;
    if (pulAdditionalHeaderCount) *pulAdditionalHeaderCount = 0;

    if (!s) return E_INVALIDARG;

    /* Sec-WebSocket-Key を検索 */
    const char *ws_key = find_ws_key(pRequestHeaders, ulRequestHeaderCount);
    if (!ws_key) {
        ws_log("WebSocketBeginServerHandshake: Sec-WebSocket-Key not found "
               "(nHeaders=%lu)\n", (unsigned long)ulRequestHeaderCount);
        return S_OK; /* キーがなくても続行 */
    }

    /* Sec-WebSocket-Accept を計算 */
    memset(s->accept_val, 0, sizeof(s->accept_val));
    if (!compute_ws_accept(ws_key, s->accept_val, sizeof(s->accept_val))) {
        ws_log("WebSocketBeginServerHandshake: compute_ws_accept failed\n");
        return S_OK;
    }

    /* 応答ヘッダーを組み立て (固定3ヘッダー) */
    s->srv_headers[0].pcName        = HNAME_ACCEPT;
    s->srv_headers[0].ulNameLength  = (ULONG)strlen(HNAME_ACCEPT);
    s->srv_headers[0].pcValue       = s->accept_val;
    s->srv_headers[0].ulValueLength = (ULONG)strlen(s->accept_val);

    s->srv_headers[1].pcName        = HNAME_CONNECTION;
    s->srv_headers[1].ulNameLength  = (ULONG)strlen(HNAME_CONNECTION);
    s->srv_headers[1].pcValue       = HVAL_CONNECTION;
    s->srv_headers[1].ulValueLength = (ULONG)strlen(HVAL_CONNECTION);

    s->srv_headers[2].pcName        = HNAME_UPGRADE;
    s->srv_headers[2].ulNameLength  = (ULONG)strlen(HNAME_UPGRADE);
    s->srv_headers[2].pcValue       = HVAL_UPGRADE;
    s->srv_headers[2].ulValueLength = (ULONG)strlen(HVAL_UPGRADE);

    s->srv_header_count = 3;

    /* Extensions 処理:
     * 方針1: pszExtensionSelected に値があれば使う (kabu STATION 主導)
     * 方針2: なければ pRequestHeaders の Sec-WebSocket-Extensions を見て返す
     * 方針3: kabu STATION は .NET Framework の ManagedWebSocket を使い、
     *        pszExtensionSelected=0 でも内部的に permessage-deflate (RSV1=1) を
     *        送信するため、.NET が Extension ヘッダーを pRequestHeaders に渡さない
     *        場合は無条件で permessage-deflate を返す。
     *        (Python の websockets は compression="deflate" でリクエストに含め、
     *        サーバーが Sec-WebSocket-Extensions を返せば Extension を登録する) */
    s->ext_val[0] = '\0';

    if (pszExtensionSelected && ulExtensionSelectedCount > 0) {
        /* 方針1: pszExtensionSelected を使う */
        for (ULONG i = 0; i < ulExtensionSelectedCount; i++) {
            if (!pszExtensionSelected[i]) continue;
            if (s->ext_val[0] != '\0')
                strncat(s->ext_val, ", ", sizeof(s->ext_val) - strlen(s->ext_val) - 1);
            strncat(s->ext_val, pszExtensionSelected[i],
                    sizeof(s->ext_val) - strlen(s->ext_val) - 1);
        }
    }

    if (s->ext_val[0] == '\0') {
        /* 方針2: pRequestHeaders を確認 */
        static const char EXT_HDR[] = "sec-websocket-extensions";
        for (ULONG i = 0; i < ulRequestHeaderCount; i++) {
            if (!pRequestHeaders[i].pcName) continue;
            ULONG len = pRequestHeaders[i].ulNameLength;
            if (len != 24) continue;
            char tmp[32] = {0};
            if (len >= sizeof(tmp)) continue;
            for (ULONG j = 0; j < len; j++)
                tmp[j] = (char)tolower((unsigned char)pRequestHeaders[i].pcName[j]);
            if (strcmp(tmp, EXT_HDR) != 0) continue;
            const char *extval = pRequestHeaders[i].pcValue;
            if (extval && strstr(extval, "permessage-deflate")) {
                strncpy(s->ext_val, "permessage-deflate", sizeof(s->ext_val) - 1);
                ws_log("WebSocketBeginServerHandshake: extension from pRequestHeaders: %s\n",
                       s->ext_val);
            }
            break;
        }
    }

    if (s->ext_val[0] == '\0') {
        /* 方針3: kabu STATION は常に permessage-deflate を使用するため無条件で追加。
         * .NET Framework は Extension を pRequestHeaders に含めない実装のため。 */
        strncpy(s->ext_val, "permessage-deflate", sizeof(s->ext_val) - 1);
        ws_log("WebSocketBeginServerHandshake: forcing permessage-deflate (fallback)\n");
    }

    if (s->ext_val[0] != '\0') {
        s->srv_headers[3].pcName        = HNAME_EXTENSIONS;
        s->srv_headers[3].ulNameLength  = (ULONG)strlen(HNAME_EXTENSIONS);
        s->srv_headers[3].pcValue       = s->ext_val;
        s->srv_headers[3].ulValueLength = (ULONG)strlen(s->ext_val);
        s->srv_header_count = 4;
        ws_log("WebSocketBeginServerHandshake: extensions=%s\n", s->ext_val);
    }

    if (ppAdditionalHeaders)      *ppAdditionalHeaders      = s->srv_headers;
    if (pulAdditionalHeaderCount) *pulAdditionalHeaderCount = s->srv_header_count;

    ws_log("WebSocketBeginServerHandshake: handle=%p key=%.20s... accept=%s nExt=%lu nReqHdr=%lu\n",
           hWebSocket, ws_key, s->accept_val, (unsigned long)ulExtensionSelectedCount,
           (unsigned long)ulRequestHeaderCount);
    return S_OK;
}

HRESULT WINAPI WebSocketEndServerHandshake(WEB_SOCKET_HANDLE hWebSocket) {
    ws_log("WebSocketEndServerHandshake: handle=%p\n", hWebSocket);
    (void)hWebSocket;
    return S_OK;
}

/* ───────────────────── Send ───────────────────── */

HRESULT WINAPI WebSocketSend(
    WEB_SOCKET_HANDLE      hWebSocket,
    WEB_SOCKET_BUFFER_TYPE bufferType,
    WEB_SOCKET_BUFFER     *pBuffer,
    PVOID                  pvContext)
{
    WS_STATE *s = (WS_STATE *)hWebSocket;
    if (!s || !pBuffer) return E_INVALIDARG;

    EnterCriticalSection(&s->cs);
    if (s->send_phase != SEND_PHASE_IDLE) {
        ws_log("WebSocketSend: BUSY (phase=%d)\n", s->send_phase);
        LeaveCriticalSection(&s->cs);
        return E_FAIL;
    }
    s->send_type     = bufferType;
    s->send_buf      = *pBuffer;
    s->send_app_ctx  = pvContext;
    s->frame_hdr_len = build_frame_header(s->frame_hdr, bufferType,
                                          pBuffer->ulBufferLength);

    /* v2.13: [根本修正] .NET Framework の referencesource (WebSocketBuffer.cs
     * ValidateNativeBuffers) を実際に確認して特定した正しい契約:
     *   - ペイロードバッファは WebSocketSend に渡された pBuffer と
     *     "完全に同一" のポインタ・長さでなければならない
     *     (IsPinnedSendPayloadBuffer でのみ許可される)。
     *   - それ以外のバッファ(ヘッダー等)は、WebSocketCreateServerHandle の
     *     pProperties で渡された WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE の
     *     範囲内でなければならない(IsNativeBuffer)。
     * v2.9〜v2.12 はどちらも満たさない自前メモリ/オフセットを返しており、
     * AccessViolationException の原因だった。
     * → ヘッダーは native_buf に書き込み、ペイロードは pBuffer をそのまま
     *   別バッファとして返す(2バッファ構成に戻す)。 */
    if (s->native_buf && s->native_buf_len >= s->frame_hdr_len) {
        memcpy(s->native_buf, s->frame_hdr, s->frame_hdr_len);
    } else {
        ws_log("WebSocketSend: WARNING native_buf unavailable (buf=%p len=%lu need=%lu)\n",
               (void*)s->native_buf, (unsigned long)s->native_buf_len,
               (unsigned long)s->frame_hdr_len);
    }
    s->send_bufs[0].pbBuffer       = s->native_buf;
    s->send_bufs[0].ulBufferLength = s->frame_hdr_len;
    s->send_bufs[0].usStatus       = 0;
    s->send_bufs[1] = *pBuffer;   /* .NET のピン留めバッファをそのまま渡す(改変禁止) */
    s->send_bufs[1].usStatus       = 0;

    s->send_phase    = SEND_PHASE_HEADER;
    LeaveCriticalSection(&s->cs);

    ws_log("WebSocketSend: handle=%p type=%d bufLen=%lu hdrLen=%lu ctx=%p\n",
           hWebSocket, (int)bufferType, (unsigned long)pBuffer->ulBufferLength,
           (unsigned long)s->frame_hdr_len, pvContext);
    ws_log("  payload pBuffer->pbBuffer=%p end=%p len=%lu | native_buf=%p end=%p len=%lu | gap=%lld\n",
           (void*)pBuffer->pbBuffer, (void*)(pBuffer->pbBuffer + pBuffer->ulBufferLength),
           (unsigned long)pBuffer->ulBufferLength,
           (void*)s->native_buf, (void*)(s->native_buf + s->native_buf_len),
           (unsigned long)s->native_buf_len,
           (long long)(pBuffer->pbBuffer - (s->native_buf + s->native_buf_len)));
    return S_OK;
}

/* ───────────────────── Receive ───────────────────── */

HRESULT WINAPI WebSocketReceive(
    WEB_SOCKET_HANDLE  hWebSocket,
    WEB_SOCKET_BUFFER *pBuffer,
    PVOID              pvContext)
{
    WS_STATE *s = (WS_STATE *)hWebSocket;
    if (!s) return E_INVALIDARG;

    LONG cnt = InterlockedIncrement(&s->recv_log_count);
    if (cnt <= 5) {
        ws_log("WebSocketReceive: handle=%p bufLen=%lu ctx=%p (call#%ld)\n",
               hWebSocket,
               (unsigned long)(pBuffer ? pBuffer->ulBufferLength : 0),
               pvContext, (long)cnt);
    }

    EnterCriticalSection(&s->cs);
    if (s->recv_phase != RECV_PHASE_IDLE) {
        LeaveCriticalSection(&s->cs);
        return E_FAIL; /* 既にキュー済み */
    }
    s->recv_buf     = pBuffer ? *pBuffer
                              : (WEB_SOCKET_BUFFER){NULL, 0, 0};
    s->recv_app_ctx = pvContext;
    s->recv_phase   = RECV_PHASE_PENDING;
    LeaveCriticalSection(&s->cs);
    return S_OK;
}

/* ───────────────────── GetAction ───────────────────── */

HRESULT WINAPI WebSocketGetAction(
    WEB_SOCKET_HANDLE       hWebSocket,
    WEB_SOCKET_ACTION_QUEUE eActionQueue,
    WEB_SOCKET_BUFFER      *pDataBuffers,
    ULONG                  *pulDataBufferCount,
    WEB_SOCKET_ACTION      *pAction,
    WEB_SOCKET_BUFFER_TYPE *pBufferType,
    PVOID                  *pvApplicationContext,
    PVOID                  *pvActionContext)
{
    WS_STATE *s = (WS_STATE *)hWebSocket;

    /* 呼び出し元が渡したバッファ数を先に保存する。
     * 後で *pulDataBufferCount = 0 で初期化するため、先に読まないと失われる。 */
    ULONG inBufCount = pulDataBufferCount ? *pulDataBufferCount : 0;

    if (pulDataBufferCount)   *pulDataBufferCount   = 0;
    if (pAction)              *pAction              = WEB_SOCKET_NO_ACTION;
    if (pBufferType)          *pBufferType          = WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    if (pvApplicationContext) *pvApplicationContext = NULL;
    if (pvActionContext)      *pvActionContext      = NULL;

    if (!s) return E_INVALIDARG;

    /* ─── 無条件ログ (最初100回) ─── */
    LONG cnt = InterlockedIncrement(&s->getaction_log_count);
    if (cnt <= 100) {
        ws_log("WebSocketGetAction[%ld]: handle=%p queue=%d inBufCount=%lu "
               "send_phase=%d recv_phase=%d\n",
               (long)cnt, hWebSocket, (int)eActionQueue, (unsigned long)inBufCount,
               s->send_phase, s->recv_phase);
    }

    BOOL want_send = (eActionQueue == WEB_SOCKET_ALL_ACTION_QUEUE ||
                      eActionQueue == WEB_SOCKET_SEND_ACTION_QUEUE);
    BOOL want_recv = (eActionQueue == WEB_SOCKET_ALL_ACTION_QUEUE ||
                      eActionQueue == WEB_SOCKET_RECEIVE_ACTION_QUEUE);

    EnterCriticalSection(&s->cs);

    /* 送信優先 */
    if (want_send && s->send_phase != SEND_PHASE_IDLE) {
        ULONG reqCount = inBufCount; /* 呼び出し元が渡したバッファ数 (関数先頭で保存済み) */

        switch (s->send_phase) {
        case SEND_PHASE_HEADER:
            /* v2.17: 2バッファ [header(native_buf), payload(ピン留め pBuffer)] を返す。
             * native_buf は extract_allocated_buffer の参照外し修正により
             * [m_StartAddress, m_EndAddress] 内を指すため header は IsNativeBuffer を、
             * payload は IsPinnedSendPayloadBuffer を満たし、検証を通過する。 */
            if (pDataBuffers && reqCount >= 2) {
                pDataBuffers[0] = s->send_bufs[0];
                pDataBuffers[1] = s->send_bufs[1];
                if (pulDataBufferCount) *pulDataBufferCount = 2;
                s->send_phase = SEND_PHASE_AWAITING_ACK;
                ws_log("WebSocketGetAction: SEND_TO_NETWORK 2bufs hdrLen=%lu payLen=%lu\n",
                       (unsigned long)s->send_bufs[0].ulBufferLength,
                       (unsigned long)s->send_bufs[1].ulBufferLength);
                if (pAction)              *pAction              = WEB_SOCKET_SEND_TO_NETWORK_ACTION;
                if (pBufferType)          *pBufferType          = s->send_type;
                if (pvApplicationContext) *pvApplicationContext = s->send_app_ctx;
                if (pvActionContext)      *pvActionContext      = (PVOID)&s->send_phase;
            } else {
                /* バッファ数不足 → NO_ACTION を返してポンプに再試行させる */
                ws_log("WebSocketGetAction: SEND_HEADER but insufficient buffers (count=%lu) → NO_ACTION\n",
                       (unsigned long)reqCount);
            }
            break;

        case SEND_PHASE_PAYLOAD:
            /* v2.9: 単一バッファ化により本フェーズは使用しなくなったが、
             * 念のため残しておく(到達しない想定)。 */
            ws_log("WebSocketGetAction: unexpected SEND_PHASE_PAYLOAD reached\n");
            s->send_phase = SEND_PHASE_COMPLETE;
            break;

        case SEND_PHASE_COMPLETE:
            if (pAction)              *pAction              = WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION;
            if (pBufferType)          *pBufferType          = s->send_type;
            if (pvApplicationContext) *pvApplicationContext = s->send_app_ctx;
            s->send_phase = SEND_PHASE_IDLE;
            ws_log("WebSocketGetAction: INDICATE_SEND_COMPLETE appCtx=%p\n",
                   s->send_app_ctx);
            break;

        default:
            break;
        }

        LeaveCriticalSection(&s->cs);
        return S_OK;
    }

    /* 受信 */
    if (want_recv) {
        switch (s->recv_phase) {
        case RECV_PHASE_PENDING:
            if (pDataBuffers && pulDataBufferCount && *pulDataBufferCount >= 1) {
                pDataBuffers[0]          = s->recv_buf;
                pDataBuffers[0].usStatus = 0;
                *pulDataBufferCount      = 1;
            }
            if (pAction)              *pAction              = WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION;
            if (pBufferType)          *pBufferType          = WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            if (pvApplicationContext) *pvApplicationContext = s->recv_app_ctx;
            if (pvActionContext)      *pvActionContext      = (PVOID)&s->recv_phase;
            s->recv_phase = RECV_PHASE_NETWORK;
            ws_log("WebSocketGetAction: RECEIVE_FROM_NETWORK_ACTION\n");
            break;

        case RECV_PHASE_COMPLETE:
            if (pAction)              *pAction              = WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION;
            if (pBufferType)          *pBufferType          = WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            if (pvApplicationContext) *pvApplicationContext = s->recv_app_ctx;
            s->recv_phase = RECV_PHASE_IDLE;
            ws_log("WebSocketGetAction: INDICATE_RECEIVE_COMPLETE bytes=%lu\n",
                   (unsigned long)s->recv_bytes_transferred);
            break;

        default:
            break;
        }
    }

    LeaveCriticalSection(&s->cs);
    return S_OK;
}

/* ───────────────────── CompleteAction ───────────────────── */

void WINAPI WebSocketCompleteAction(
    WEB_SOCKET_HANDLE hWebSocket,
    PVOID             pvActionContext,
    ULONG             ulBytesTransferred)
{
    WS_STATE *s = (WS_STATE *)hWebSocket;
    if (!s) return;

    ws_log("WebSocketCompleteAction: handle=%p actionCtx=%p bytes=%lu "
           "send_phase=%d recv_phase=%d\n",
           hWebSocket, pvActionContext, (unsigned long)ulBytesTransferred,
           s->send_phase, s->recv_phase);

    EnterCriticalSection(&s->cs);
    if (pvActionContext == (PVOID)&s->send_phase && s->send_phase == SEND_PHASE_AWAITING_ACK) {
        /* v2.10: 送信完了の確認が取れたので COMPLETE へ進める。
         * 次回 GetAction 呼び出しで INDICATE_SEND_COMPLETE_ACTION を返す。 */
        s->send_phase = SEND_PHASE_COMPLETE;
        ws_log("WebSocketCompleteAction: send done bytes=%lu → COMPLETE\n",
               (unsigned long)ulBytesTransferred);
    }
    if (s->recv_phase == RECV_PHASE_NETWORK) {
        s->recv_bytes_transferred = ulBytesTransferred;
        s->recv_phase = RECV_PHASE_COMPLETE;
        ws_log("WebSocketCompleteAction: recv done bytes=%lu → COMPLETE\n",
               (unsigned long)ulBytesTransferred);
    }
    LeaveCriticalSection(&s->cs);
}

/* v2.11: 常に E_NOTIMPL を返していたのを修正。
 * .NET Framework の WebSocketProtocolComponent は静的初期化時にこの関数で
 * バッファサイズやサポートバージョンを問い合わせ、失敗すると内部的に
 * "WebSocket未サポート" 扱いとなり、以後の実送信を毎回 Abort する疑いがある。
 */
HRESULT WINAPI WebSocketGetGlobalProperty(
    WEB_SOCKET_PROPERTY_TYPE eType, PVOID pvValue, ULONG *ulSize)
{
    ws_log("WebSocketGetGlobalProperty: eType=%d pvValue=%p ulSize=%p (*ulSize=%lu)\n",
           (int)eType, pvValue, (void*)ulSize, ulSize ? (unsigned long)*ulSize : 0);

    if (!ulSize) return E_INVALIDARG;

    switch (eType) {
    case WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE:
    case WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE: {
        ULONG val = 4096;
        if (*ulSize < sizeof(ULONG)) { *ulSize = sizeof(ULONG); return E_INVALIDARG; }
        if (pvValue) memcpy(pvValue, &val, sizeof(ULONG));
        *ulSize = sizeof(ULONG);
        return S_OK;
    }
    case WEB_SOCKET_DISABLE_MASKING_PROPERTY_TYPE:
    case WEB_SOCKET_DISABLE_UTF8_VERIFICATION_PROPERTY_TYPE: {
        BOOL val = FALSE;
        if (*ulSize < sizeof(BOOL)) { *ulSize = sizeof(BOOL); return E_INVALIDARG; }
        if (pvValue) memcpy(pvValue, &val, sizeof(BOOL));
        *ulSize = sizeof(BOOL);
        return S_OK;
    }
    case WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE: {
        ULONG val = 30000;
        if (*ulSize < sizeof(ULONG)) { *ulSize = sizeof(ULONG); return E_INVALIDARG; }
        if (pvValue) memcpy(pvValue, &val, sizeof(ULONG));
        *ulSize = sizeof(ULONG);
        return S_OK;
    }
    case WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE: {
        /* RFC 6455 のバージョン番号 13 を1バイトで返す */
        static const BYTE versions[] = { 13 };
        if (*ulSize < sizeof(versions)) { *ulSize = sizeof(versions); return E_INVALIDARG; }
        if (pvValue) memcpy(pvValue, versions, sizeof(versions));
        *ulSize = sizeof(versions);
        return S_OK;
    }
    default:
        ws_log("WebSocketGetGlobalProperty: unknown eType=%d\n", (int)eType);
        return E_NOTIMPL;
    }
}
