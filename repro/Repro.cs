// Minimal standalone reproduction of kabu STATION's server-side WebSocket push
// under Wine. Exercises the SAME path: HttpListener -> AcceptWebSocketAsync ->
// server WebSocket.SendAsync -> native websocket.dll shim.
//
// Goal: reproduce (or not) the AccessViolationException in ValidateNativeBuffers
// WITHOUT needing kabu STATION, login, or the one-shot push.
//
// Build (under Wine):
//   wine .../v4.0.30319/csc.exe /out:Repro.exe /r:System.dll Repro.cs
// Run:
//   WINEDLLOVERRIDES="websocket=native" wine Repro.exe

using System;
using System.Net;
using System.Net.WebSockets;
using System.Runtime.ExceptionServices;
using System.Security;
using System.Threading;
using System.Threading.Tasks;

class Repro
{
    const string HttpUrl = "http://localhost:28080/ws/";
    const string WsUrl = "ws://localhost:28080/ws/";
    const int PayloadSize = 1900; // ~ kabu board snapshot size

    static void Main()
    {
        Console.WriteLine("[main] starting repro");
        var serverTask = Task.Run(() => ServerAsync());
        Thread.Sleep(1500);
        var clientTask = Task.Run(() => ClientAsync());

        Task.WaitAll(new[] { serverTask, clientTask }, 30000);
        Console.WriteLine("[main] done");
    }

    static async Task ServerAsync()
    {
        try
        {
            var listener = new HttpListener();
            listener.Prefixes.Add(HttpUrl);
            listener.Start();
            Console.WriteLine("[server] listening on " + HttpUrl);

            HttpListenerContext ctx = await listener.GetContextAsync();
            Console.WriteLine("[server] request received, IsWebSocketRequest=" + ctx.Request.IsWebSocketRequest);

            // receiveBufferSize=16384 to match kabu STATION (=> native_buf 16544)
            HttpListenerWebSocketContext wsCtx =
                await ctx.AcceptWebSocketAsync(null, 16384, TimeSpan.FromSeconds(30));
            WebSocket ws = wsCtx.WebSocket;
            Console.WriteLine("[server] websocket accepted, state=" + ws.State);

            var payload = new byte[PayloadSize];
            for (int i = 0; i < payload.Length; i++) payload[i] = (byte)('A' + (i % 26));

            Console.WriteLine("[server] calling SendAsync (" + PayloadSize + " bytes, Text)...");
            try
            {
                await ws.SendAsync(new ArraySegment<byte>(payload),
                    WebSocketMessageType.Text, true, CancellationToken.None);
                Console.WriteLine("[server] >>> SEND OK <<<");
                await Task.Delay(3000); // keep connection open so client can receive+print
            }
            catch (Exception e)
            {
                Console.WriteLine("[server] >>> SEND FAILED: " + e.GetType().FullName + ": " + e.Message);
                Console.WriteLine(e.StackTrace);
            }
        }
        catch (Exception e)
        {
            Console.WriteLine("[server] outer error: " + e.GetType().FullName + ": " + e.Message);
            Console.WriteLine(e.StackTrace);
        }
    }

    static async Task ClientAsync()
    {
        try
        {
            var cws = new ClientWebSocket();
            await cws.ConnectAsync(new Uri(WsUrl), CancellationToken.None);
            Console.WriteLine("[client] connected, state=" + cws.State);

            var buf = new byte[8192];
            WebSocketReceiveResult r = await cws.ReceiveAsync(
                new ArraySegment<byte>(buf), CancellationToken.None);
            Console.WriteLine("[client] >>> RECEIVED " + r.Count + " bytes, type=" + r.MessageType + " <<<");
        }
        catch (Exception e)
        {
            Console.WriteLine("[client] error: " + e.GetType().FullName + ": " + e.Message);
        }
    }
}
