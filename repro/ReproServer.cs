// Server-only: accept a websocket and push a 1900-byte text frame, then stay
// alive so an external (Python) client can receive it. Mirrors kabu STATION.
using System;
using System.Net;
using System.Net.WebSockets;
using System.Threading;
using System.Threading.Tasks;

class ReproServer
{
    const string HttpUrl = "http://localhost:28085/ws/";
    static void Main()
    {
        var t = Task.Run(() => ServerAsync());
        t.Wait(60000);
        Console.WriteLine("[main] done");
    }
    static async Task ServerAsync()
    {
        var listener = new HttpListener();
        listener.Prefixes.Add(HttpUrl);
        listener.Start();
        Console.WriteLine("[server] listening on " + HttpUrl);
        var ctx = await listener.GetContextAsync();
        var wsCtx = await ctx.AcceptWebSocketAsync(null, 16384, TimeSpan.FromSeconds(30));
        WebSocket ws = wsCtx.WebSocket;
        Console.WriteLine("[server] accepted");
        var payload = new byte[1900];
        for (int i = 0; i < payload.Length; i++) payload[i] = (byte)('A' + (i % 26));
        try
        {
            await ws.SendAsync(new ArraySegment<byte>(payload), WebSocketMessageType.Text, true, CancellationToken.None);
            Console.WriteLine("[server] >>> SEND OK <<<");
        }
        catch (Exception e) { Console.WriteLine("[server] SEND FAILED: " + e.GetType().Name); }
        await Task.Delay(12000); // keep alive for external client
    }
}
