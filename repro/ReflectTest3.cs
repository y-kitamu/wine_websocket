// Diagnostic 3: drive the REAL native WebSocketSend + WebSocketGetAction_Raw via
// reflection, then inspect what marshaled back into the managed dataBuffers array
// and whether it matches m_PinnedSendBufferStartAddress. Manually run
// ValidateNativeBuffers to see if it throws on the shim-written buffers.

using System;
using System.Net;
using System.Net.WebSockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;

class ReflectTest3
{
    const string HttpUrl = "http://localhost:28083/ws/";
    const string WsUrl = "ws://localhost:28083/ws/";

    static object GetField(object o, string name)
    {
        for (Type t = o.GetType(); t != null; t = t.BaseType)
        {
            var f = t.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (f != null) return f.GetValue(o);
        }
        throw new Exception("field not found: " + name);
    }
    static object GetProp(object o, string name)
    {
        for (Type t = o.GetType(); t != null; t = t.BaseType)
        {
            var p = t.GetProperty(name, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            if (p != null) return p.GetValue(o, null);
        }
        throw new Exception("prop not found: " + name);
    }
    static MethodInfo FindMethod(Type type, string name, int nParams)
    {
        for (Type t = type; t != null; t = t.BaseType)
            foreach (var m in t.GetMethods(BindingFlags.Instance | BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public))
                if (m.Name == name && m.GetParameters().Length == nParams) return m;
        throw new Exception("method not found: " + name + "/" + nParams);
    }
    static MethodInfo FindMethod1Param(object o, string name, Type pType)
    {
        for (Type t = o.GetType(); t != null; t = t.BaseType)
            foreach (var m in t.GetMethods(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public))
            {
                if (m.Name != name) continue;
                var ps = m.GetParameters();
                if (ps.Length == 1 && ps[0].ParameterType == pType) return m;
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
            object sessionHandle = GetProp(ws, "SessionHandle");

            var payload = new byte[1900];
            var seg = new ArraySegment<byte>(payload);

            // PinSendBuffer
            FindMethod(ib.GetType(), "PinSendBuffer", 2).Invoke(ib, new object[] { seg, false });
            long pinStart = (long)GetField(ib, "m_PinnedSendBufferStartAddress");
            long pinEnd = (long)GetField(ib, "m_PinnedSendBufferEndAddress");
            int state = (int)GetField(ib, "m_SendBufferState");
            IntPtr nativePtr = (IntPtr)FindMethod1Param(ib, "ConvertPinnedSendPayloadToNative", typeof(ArraySegment<byte>)).Invoke(ib, new object[] { seg });
            Console.WriteLine(string.Format("[t3] state={0} pinStart=0x{1:x} pinEnd=0x{2:x} convNative=0x{3:x}",
                state, pinStart, pinEnd, nativePtr.ToInt64()));

            Assembly sys = typeof(WebSocket).Assembly;
            Type tComp = sys.GetType("System.Net.WebSockets.WebSocketProtocolComponent");
            Type tBuffer = tComp.GetNestedType("Buffer", BindingFlags.NonPublic);
            Type tAction = tComp.GetNestedType("Action", BindingFlags.NonPublic);
            Type tBufType = tComp.GetNestedType("BufferType", BindingFlags.NonPublic);
            Type tActionQueue = tComp.GetNestedType("ActionQueue", BindingFlags.NonPublic);
            var fData = tBuffer.GetField("Data", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            Type tData = fData.FieldType;
            var fBufData = tData.GetField("BufferData", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            var fBufLen = tData.GetField("BufferLength", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);

            Func<object, IntPtr> getBD = (b) => { object d = fData.GetValue(b); return (IntPtr)fBufData.GetValue(d); };
            Func<object, uint> getBL = (b) => { object d = fData.GetValue(b); return (uint)fBufLen.GetValue(d); };

            // Build payload Buffer and call the real native WebSocketSend wrapper
            object payloadBuf = Activator.CreateInstance(tBuffer);
            {
                object d = fData.GetValue(payloadBuf);
                fBufData.SetValue(d, nativePtr);
                fBufLen.SetValue(d, (uint)1900);
                fData.SetValue(payloadBuf, d);
            }
            object utf8 = Enum.ToObject(tBufType, 0x80000000u);
            // WebSocketSend(WebSocketBase, BufferType, Buffer)
            MethodInfo wsSend = FindMethod(tComp, "WebSocketSend", 3);
            wsSend.Invoke(null, new object[] { ws, utf8, payloadBuf });
            Console.WriteLine("[t3] native WebSocketSend called");

            // Call WebSocketGetAction_Raw directly (bypasses ValidateNativeBuffers)
            MethodInfo getActionRaw = null;
            foreach (var m in tComp.GetMethods(BindingFlags.Static | BindingFlags.NonPublic))
                if (m.Name == "WebSocketGetAction_Raw") { getActionRaw = m; break; }
            Console.WriteLine("[t3] WebSocketGetAction_Raw params: " +
                string.Join(",", Array.ConvertAll(getActionRaw.GetParameters(), p => p.ParameterType.Name)));

            Array dataBuffers = Array.CreateInstance(tBuffer, 2);
            dataBuffers.SetValue(Activator.CreateInstance(tBuffer), 0);
            dataBuffers.SetValue(Activator.CreateInstance(tBuffer), 1);
            object sendQueue = Enum.ToObject(tActionQueue, 1); // SendActionQueue

            object[] gaArgs = new object[] {
                sessionHandle, sendQueue, dataBuffers, (uint)2,
                null, null, IntPtr.Zero, IntPtr.Zero
            };
            object ret = getActionRaw.Invoke(null, gaArgs);
            uint outCount = (uint)gaArgs[3];
            object outAction = gaArgs[4];
            object outBufType = gaArgs[5];
            Console.WriteLine("[t3] GetAction_Raw ret=" + ret + " outCount=" + outCount + " action=" + outAction + " bufType=" + outBufType);

            for (int i = 0; i < 2; i++)
            {
                object b = dataBuffers.GetValue(i);
                long bd = getBD(b).ToInt64();
                uint bl = getBL(b);
                Console.WriteLine(string.Format("[t3] dataBuffers[{0}] BufferData=0x{1:x} BufferLength={2}", i, bd, bl));
            }
            Console.WriteLine(string.Format("[t3] compare: dataBuffers[1].BufferData vs pinStart=0x{0:x} pinEnd=0x{1:x}", pinStart, pinEnd));
            int stateNow = (int)GetField(ib, "m_SendBufferState");
            Console.WriteLine("[t3] m_SendBufferState now = " + stateNow);

            long mStart = (long)GetField(ib, "m_StartAddress");
            long mEnd = (long)GetField(ib, "m_EndAddress");
            int recvSz = (int)GetField(ib, "m_ReceiveBufferSize");
            int sendSz = (int)GetField(ib, "m_SendBufferSize");
            Console.WriteLine(string.Format("[t3] m_StartAddress=0x{0:x} m_EndAddress=0x{1:x} recv={2} send={3} maxBuf={4}",
                mStart, mEnd, recvSz, sendSz, Math.Max(recvSz, sendSz)));

            MethodInfo validate = FindMethod(ib.GetType(), "ValidateNativeBuffers", 4);

            // full 2-buffer
            try {
                validate.Invoke(ib, new object[] { outAction, outBufType, dataBuffers, (uint)2 });
                Console.WriteLine("[t3] >>> 2-buf shim PASSED <<<");
            } catch (TargetInvocationException tie) {
                Console.WriteLine("[t3] >>> 2-buf shim THREW: " + tie.InnerException.GetType().Name + " <<<");
            }

            // header alone (shim buffer[0])
            Array a0 = Array.CreateInstance(tBuffer, 1);
            a0.SetValue(dataBuffers.GetValue(0), 0);
            try {
                validate.Invoke(ib, new object[] { outAction, outBufType, a0, (uint)1 });
                Console.WriteLine("[t3] >>> header-alone (shim) PASSED <<<");
            } catch (TargetInvocationException tie) {
                Console.WriteLine("[t3] >>> header-alone (shim) THREW: " + tie.InnerException.GetType().Name + " <<<");
            }

            // payload alone (shim buffer[1])
            Array a1 = Array.CreateInstance(tBuffer, 1);
            a1.SetValue(dataBuffers.GetValue(1), 0);
            try {
                validate.Invoke(ib, new object[] { outAction, outBufType, a1, (uint)1 });
                Console.WriteLine("[t3] >>> payload-alone (shim) PASSED <<<");
            } catch (TargetInvocationException tie) {
                Console.WriteLine("[t3] >>> payload-alone (shim) THREW: " + tie.InnerException.GetType().Name + " <<<");
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
            await Task.Delay(8000);
        }
        catch { }
    }
}
