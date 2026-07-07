// Diagnostic: directly probe whether PinSendBuffer's Interlocked.Exchange on
// m_SendBufferState actually sticks under Wine, and whether the pinned-payload
// address bookkeeping matches. Uses reflection into System.dll internals.

using System;
using System.Net;
using System.Net.WebSockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

class ReflectTest
{
    const string HttpUrl = "http://localhost:28081/ws/";
    const string WsUrl = "ws://localhost:28081/ws/";

    static void Main()
    {
        var s = Task.Run(() => ServerAsync());
        Thread.Sleep(1500);
        Task.Run(() => ClientAsync());
        Task.WaitAll(new[] { s }, 30000);
        Console.WriteLine("[main] done");
    }

    static object GetField(object o, string name)
    {
        Type t = o.GetType();
        while (t != null)
        {
            var f = t.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (f != null) return f.GetValue(o);
            t = t.BaseType;
        }
        throw new Exception("field not found: " + name);
    }

    static MethodInfo GetMethod(object o, string name)
    {
        Type t = o.GetType();
        while (t != null)
        {
            var m = t.GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (m != null) return m;
            t = t.BaseType;
        }
        throw new Exception("method not found: " + name);
    }

    static async Task ServerAsync()
    {
        try
        {
            var listener = new HttpListener();
            listener.Prefixes.Add(HttpUrl);
            listener.Start();
            var ctx = await listener.GetContextAsync();
            var wsCtx = await ctx.AcceptWebSocketAsync(null, 16384, TimeSpan.FromSeconds(30));
            WebSocket ws = wsCtx.WebSocket;
            Console.WriteLine("[server] ws accepted, type=" + ws.GetType().FullName);

            object internalBuffer = GetField(ws, "m_InternalBuffer");
            Console.WriteLine("[probe] internalBuffer type=" + internalBuffer.GetType().FullName);

            long startAddr = (long)GetField(internalBuffer, "m_StartAddress");
            long endAddr = (long)GetField(internalBuffer, "m_EndAddress");
            int recvSize = (int)GetField(internalBuffer, "m_ReceiveBufferSize");
            int sendSize = (int)GetField(internalBuffer, "m_SendBufferSize");
            Console.WriteLine(string.Format("[probe] m_StartAddress=0x{0:x} m_EndAddress=0x{1:x} recvBuf={2} sendBuf={3} range={4}",
                startAddr, endAddr, recvSize, sendSize, endAddr - startAddr));

            int stateBefore = (int)GetField(internalBuffer, "m_SendBufferState");
            Console.WriteLine("[probe] m_SendBufferState BEFORE pin = " + stateBefore + " (0=None,1=SendPayloadSpecified)");

            // Pin a payload via the real internal method
            var payload = new byte[1900];
            var seg = new ArraySegment<byte>(payload);
            var pin = GetMethod(internalBuffer, "PinSendBuffer");
            object[] args = new object[] { seg, false };
            pin.Invoke(internalBuffer, args);
            bool pinned = (bool)args[1];

            int stateAfter = (int)GetField(internalBuffer, "m_SendBufferState");
            long pinStart = (long)GetField(internalBuffer, "m_PinnedSendBufferStartAddress");
            long pinEnd = (long)GetField(internalBuffer, "m_PinnedSendBufferEndAddress");

            Console.WriteLine("[probe] PinSendBuffer returned bufferHasBeenPinned=" + pinned);
            Console.WriteLine("[probe] >>> m_SendBufferState AFTER pin = " + stateAfter + " <<<  (EXPECT 1)");
            Console.WriteLine(string.Format("[probe] m_PinnedSendBufferStartAddress=0x{0:x} EndAddress=0x{1:x}", pinStart, pinEnd));

            // Independently compute the payload's pinned address for comparison
            GCHandle gh = GCHandle.Alloc(payload, GCHandleType.Pinned);
            long myAddr = Marshal.UnsafeAddrOfPinnedArrayElement(payload, 0).ToInt64();
            Console.WriteLine(string.Format("[probe] my UnsafeAddrOfPinnedArrayElement(payload)=0x{0:x} (match pinStart? {1})",
                myAddr, myAddr == pinStart));
            gh.Free();

            // Now re-read state a few times to see if it's volatile/unstable
            for (int i = 0; i < 3; i++)
            {
                Thread.Sleep(50);
                Console.WriteLine("[probe] m_SendBufferState re-read #" + i + " = " + (int)GetField(internalBuffer, "m_SendBufferState"));
            }
        }
        catch (Exception e)
        {
            Console.WriteLine("[server] error: " + e.GetType().FullName + ": " + e.Message);
            Console.WriteLine(e.StackTrace);
        }
    }

    static async Task ClientAsync()
    {
        try
        {
            var cws = new ClientWebSocket();
            await cws.ConnectAsync(new Uri(WsUrl), CancellationToken.None);
            Console.WriteLine("[client] connected");
            await Task.Delay(5000);
        }
        catch (Exception e) { Console.WriteLine("[client] err: " + e.Message); }
    }
}
