// Diagnostic 2: directly drive ValidateNativeBuffers via reflection with a
// hand-built "pinned payload" buffer, to isolate whether the pinned-payload
// path validates, and what address ConvertPinnedSendPayloadToNative yields.

using System;
using System.Net;
using System.Net.WebSockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

class ReflectTest2
{
    const string HttpUrl = "http://localhost:28082/ws/";
    const string WsUrl = "ws://localhost:28082/ws/";

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

    static void Main()
    {
        var s = Task.Run(() => ServerAsync());
        Thread.Sleep(1500);
        Task.Run(() => ClientAsync());
        Task.WaitAll(new[] { s }, 30000);
        Console.WriteLine("[main] done");
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
            object ib = GetField(ws, "m_InternalBuffer");

            var payload = new byte[1900];
            var seg = new ArraySegment<byte>(payload);

            // PinSendBuffer
            var pin = GetMethod(ib, "PinSendBuffer");
            object[] pinArgs = new object[] { seg, false };
            pin.Invoke(ib, pinArgs);
            int state = (int)GetField(ib, "m_SendBufferState");
            long pinStart = (long)GetField(ib, "m_PinnedSendBufferStartAddress");
            long pinEnd = (long)GetField(ib, "m_PinnedSendBufferEndAddress");
            Console.WriteLine(string.Format("[t2] state={0} pinStart=0x{1:x} pinEnd=0x{2:x}", state, pinStart, pinEnd));

            // ConvertPinnedSendPayloadToNative(seg) -> the address .NET hands to WebSocketSend
            MethodInfo conv = null;
            for (Type tt = ib.GetType(); tt != null && conv == null; tt = tt.BaseType)
                foreach (var m in tt.GetMethods(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public))
                {
                    if (m.Name != "ConvertPinnedSendPayloadToNative") continue;
                    var ps = m.GetParameters();
                    if (ps.Length == 1 && ps[0].ParameterType == typeof(ArraySegment<byte>)) { conv = m; break; }
                }
            Console.WriteLine("[t2] ConvertPinnedSendPayloadToNative params: " +
                string.Join(",", Array.ConvertAll(conv.GetParameters(), p => p.ParameterType.Name)));
            object nativePtrObj = conv.Invoke(ib, new object[] { seg });
            IntPtr nativePtr = (IntPtr)nativePtrObj;
            Console.WriteLine(string.Format("[t2] ConvertPinnedSendPayloadToNative(seg)=0x{0:x}  (== pinStart? {1})",
                nativePtr.ToInt64(), nativePtr.ToInt64() == pinStart));

            // Build WebSocketProtocolComponent.Buffer[] with the pinned payload as buffer[0]
            Assembly sys = typeof(WebSocket).Assembly;
            Type tComp = sys.GetType("System.Net.WebSockets.WebSocketProtocolComponent");
            Type tBuffer = tComp.GetNestedType("Buffer", BindingFlags.NonPublic);
            Type tAction = tComp.GetNestedType("Action", BindingFlags.NonPublic);
            Type tBufType = tComp.GetNestedType("BufferType", BindingFlags.NonPublic);
            Console.WriteLine("[t2] Buffer=" + tBuffer + " Action=" + tAction + " BufferType=" + tBufType);

            // Inspect Buffer layout
            foreach (var f in tBuffer.GetFields(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public))
                Console.WriteLine("[t2] Buffer field: " + f.Name + " : " + f.FieldType.FullName);

            // Build the payload buffer via marshalling raw bytes to be safe:
            // struct is 16 bytes: IntPtr(8) + uint(4) + pad. Use a byte layout.
            int bufSize = Marshal.SizeOf(tBuffer);
            Console.WriteLine("[t2] sizeof(Buffer)=" + bufSize);

            object boxed = Activator.CreateInstance(tBuffer);
            // set Data.BufferData and Data.BufferLength
            var fData = tBuffer.GetField("Data", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            object dataVal = fData.GetValue(boxed);
            Type tData = dataVal.GetType();
            foreach (var f in tData.GetFields(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public))
                Console.WriteLine("[t2] Data field: " + f.Name + " : " + f.FieldType.FullName);
            var fBufData = tData.GetField("BufferData", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            var fBufLen = tData.GetField("BufferLength", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            fBufData.SetValue(dataVal, nativePtr);
            fBufLen.SetValue(dataVal, (uint)1900);
            fData.SetValue(boxed, dataVal);

            Array arr = Array.CreateInstance(tBuffer, 1);
            arr.SetValue(boxed, 0);

            object actionSend = Enum.ToObject(tAction, 1);        // SendToNetwork
            object bt = Enum.ToObject(tBufType, 0x80000000u);     // UTF8 message

            var validate = GetMethod(ib, "ValidateNativeBuffers");
            Console.WriteLine("[t2] calling ValidateNativeBuffers with pinned-payload buffer[0] (count=1)...");
            try
            {
                validate.Invoke(ib, new object[] { actionSend, bt, arr, (uint)1 });
                Console.WriteLine("[t2] >>> count=1 payload PASSED <<<");
            }
            catch (TargetInvocationException tie)
            {
                Console.WriteLine("[t2] >>> count=1 payload THREW: " + tie.InnerException.GetType().FullName + " <<<");
            }

            // Now the REAL scenario: 2 buffers [header@m_StartAddress len=4, payload@pinStart len=1900]
            long mStart = (long)GetField(ib, "m_StartAddress");
            Func<IntPtr, uint, object> mkBuf = (ptr, len) =>
            {
                object b = Activator.CreateInstance(tBuffer);
                object d = fData.GetValue(b);
                fBufData.SetValue(d, ptr);
                fBufLen.SetValue(d, len);
                fData.SetValue(b, d);
                return b;
            };
            Array arr2 = Array.CreateInstance(tBuffer, 2);
            arr2.SetValue(mkBuf(new IntPtr(mStart), 4u), 0);      // header in native buffer
            arr2.SetValue(mkBuf(nativePtr, 1900u), 1);            // pinned payload
            Console.WriteLine(string.Format("[t2] 2-buf: hdr=0x{0:x}(len4) pay=0x{1:x}(len1900)", mStart, nativePtr.ToInt64()));
            try
            {
                validate.Invoke(ib, new object[] { actionSend, bt, arr2, (uint)2 });
                Console.WriteLine("[t2] >>> count=2 [header,payload] PASSED <<<");
            }
            catch (TargetInvocationException tie)
            {
                Console.WriteLine("[t2] >>> count=2 [header,payload] THREW: " + tie.InnerException.GetType().FullName + " <<<");
            }

            // Also test header-alone (v2.15 scenario) for completeness
            Array arrH = Array.CreateInstance(tBuffer, 1);
            arrH.SetValue(mkBuf(new IntPtr(mStart), 4u), 0);
            try
            {
                validate.Invoke(ib, new object[] { actionSend, bt, arrH, (uint)1 });
                Console.WriteLine("[t2] >>> count=1 header PASSED <<<");
            }
            catch (TargetInvocationException tie)
            {
                Console.WriteLine("[t2] >>> count=1 header THREW: " + tie.InnerException.GetType().FullName + " <<<");
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
            await Task.Delay(6000);
        }
        catch { }
    }
}
