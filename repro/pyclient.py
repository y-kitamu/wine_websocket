"""Python websocket-client receiving from the C# repro server (mirrors data_feed.py)."""
import sys
sys.path.insert(0, "/home/kitamura/work/stock/src")
import websocket, zlib
from websocket._abnf import ABNF

# RSV1-tolerant patch (same as data_feed.py) — harmless if frames are uncompressed
_orig = ABNF.validate
def _v(self, skip=False):
    if self.rsv1 and self.opcode in (ABNF.OPCODE_TEXT, ABNF.OPCODE_BINARY):
        self.data = zlib.decompressobj(wbits=-15).decompress(bytes(self.data) + b"\x00\x00\xff\xff")
        self.rsv1 = 0
    _orig(self, skip)
ABNF.validate = _v

ws = websocket.create_connection("ws://localhost:28085/ws/", timeout=15)
print("[py] connected")
data = ws.recv()
print(f"[py] >>> RECEIVED {len(data)} bytes; first40={data[:40]!r} <<<")
ws.close()
