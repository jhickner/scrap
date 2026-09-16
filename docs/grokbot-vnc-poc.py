# Read-only proof of concept: RFB handshake + first framebuffer update over the
# Grok Bot VNC websocket proxy. Usage: uv run --with websockets docs/grokbot-vnc-poc.py [agent-name]
import asyncio, base64, hashlib, json, os, ssl, struct, subprocess, sys, urllib.request
from urllib.parse import urlsplit, parse_qs
import websockets

def load_descriptor():
    p = os.path.expanduser("~/Library/Application Support/Grok Bot/gateway-descriptor.json")
    w = json.load(open(p))
    entry = list(w["entries"].values())[0] if w.get("version") == 2 else w
    pw = subprocess.check_output(["security", "find-generic-password", "-w", "-s", "Grok Bot Safe Storage"]).strip()
    raw = base64.b64decode(entry["encrypted"]); assert raw[:3] == b"v10"
    key = hashlib.pbkdf2_hmac("sha1", pw, b"saltysalt", 1003, 16)
    clear = subprocess.check_output(["openssl", "enc", "-d", "-aes-128-cbc", "-K", key.hex(), "-iv", (b" " * 16).hex()], input=raw[3:])
    return json.loads(clear)

def gateway_call(d, method, body):
    req = urllib.request.Request(d["baseUrl"].rstrip("/") + "/api/" + method, data=json.dumps(body).encode(), method="POST",
        headers={"content-type": "application/json", "authorization": "Bearer " + d["token"], **d["headers"]})
    with urllib.request.urlopen(req, timeout=30) as r: return json.loads(r.read())

def ws_url_for(d, agent_name):
    vp = d["vncProxy"]
    if agent_name is None:
        u = urlsplit(vp["primaryUrl"]); q = parse_qs(u.query)
        return f"wss://{u.netloc}/{q['path'][0]}", {"x-anyrun-port-token": q["port_token"][0]}
    agent = next(a for a in gateway_call(d, "listAgents", {}) if a["name"] == agent_name)
    st = gateway_call(d, "getForeverBoxStatus", {"id": agent["id"]})
    if st["state"] != "running" or not st.get("vncUrl"):
        sys.exit(f"{agent_name}: box state={st['state']}, no vncUrl (ensureForeverBox would start it; not called)")
    inner = parse_qs(urlsplit(st["vncUrl"]).query)["path"][0]          # websockify?token=N
    tok = parse_qs(inner.split("?", 1)[1])["token"][0]
    host = urlsplit(vp["forkBaseUrl"]).netloc
    nt = vp["networkToken"]
    return f"wss://{host}/websockify?token={tok}&network_token={nt}&resume_lower_s=900&resume_upper_s=18000", {"x-anyrun-network-token": nt}

async def main():
    d = load_descriptor()
    url, hdr = ws_url_for(d, sys.argv[1] if len(sys.argv) > 1 else None)
    async with websockets.connect(url, additional_headers=hdr, subprotocols=["binary"], open_timeout=20, ssl=ssl.create_default_context()) as ws:
        buf = b""
        async def need(n):
            nonlocal buf
            while len(buf) < n: buf += await ws.recv()
            out, buf = buf[:n], buf[n:]; return out
        print("version:", await need(12)); await ws.send(b"RFB 003.008\n")
        n = (await need(1))[0]; types = await need(n); print("security types:", list(types))
        assert 1 in types; await ws.send(bytes([1])); print("security result:", struct.unpack(">I", await need(4))[0])
        await ws.send(bytes([1]))                                   # ClientInit shared=1
        init = await need(24); w, h = struct.unpack(">HH", init[:4]); nl = struct.unpack(">I", init[20:24])[0]
        print(f"ServerInit {w}x{h} bpp={init[4]} depth={init[5]} name={await need(nl)!r}")
        await ws.send(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))    # FramebufferUpdateRequest, full, non-incremental
        hdr = await need(4); print("msg type:", hdr[0], "rects:", struct.unpack(">H", hdr[2:4])[0])
        x, y, rw, rh, enc = struct.unpack(">HHHHi", await need(12)); print(f"rect {rw}x{rh}@{x},{y} encoding={enc}")
        if enc == 0: print("raw bytes expected:", rw * rh * (init[4] // 8))

asyncio.run(main())
