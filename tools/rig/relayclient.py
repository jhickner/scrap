import asyncio, json, re, sys, time, uuid
import websockets

port, token, script = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
url = f"ws://127.0.0.1:{port}/"
headers = {"Authorization": f"Bearer {token}"}
me = str(uuid.uuid4())
t0 = time.time()


def say(line):
    print(f"{time.time() - t0:6.2f} {line}", flush=True)


def fail(msg):
    print(f"relayclient: {msg}", file=sys.stderr)
    sys.exit(1)


class Replica:
    def __init__(self):
        self.view, self.seq = None, None

    def apply(self, m):
        if m["t"] == "view":
            self.view, self.seq = m, m["seq"]
        elif m["t"] == "delta":
            if self.seq is None or m["seq"] != self.seq + 1:
                fail(f"delta seq {m['seq']} after {self.seq}")
            self.seq = m["seq"]
            for op in m["ops"]:
                kind = op[0]
                if kind == "add":
                    self.view["entries"].append(op[1])
                elif kind == "set":
                    for e in self.view["entries"]:
                        if e["id"] == op[1]:
                            e.update(op[2])
                elif kind in ("live", "ask", "session"):
                    self.view[kind] = op[1]
                else:
                    fail(f"unknown op {kind}")


async def connect(rep, inbox):
    ws = await websockets.connect(url, additional_headers=headers, max_size=None)
    await ws.send(json.dumps({"t": "hello", "v": 2, "client": me}))

    async def reader():
        async for raw in ws:
            m = json.loads(raw)
            rep.apply(m)
            await inbox.put(raw)

    return ws, asyncio.create_task(reader())


async def fresh_view():
    async with websockets.connect(url, additional_headers=headers, max_size=None) as ws:
        await ws.send(json.dumps({"t": "hello", "v": 2, "client": str(uuid.uuid4())}))
        async for raw in ws:
            m = json.loads(raw)
            if m["t"] == "view":
                return m


async def main():
    rep, inbox, n = Replica(), asyncio.Queue(), 0
    ws, task = await connect(rep, inbox)
    for step in script:
        if isinstance(step, (int, float)):
            await asyncio.sleep(step)
        elif "wait" in step:
            end = time.time() + step.get("secs", 5)
            while True:
                try:
                    m = await asyncio.wait_for(inbox.get(), max(0.01, end - time.time()))
                except asyncio.TimeoutError:
                    fail(f"no frame matched /{step['wait']}/ in {step.get('secs', 5)}s")
                say(f"< {m[:400]}")
                if re.search(step["wait"], m):
                    break
        elif "check" in step:
            await asyncio.sleep(1.3)
            v = await fresh_view()
            mine = rep.view["entries"][-len(v["entries"]):] if v["entries"] else []
            for k in ("live", "ask", "session"):
                if rep.view[k] != v[k]:
                    fail(f"replica {k} {rep.view[k]} != view {v[k]}")
            if mine != v["entries"]:
                fail(f"replica entries differ from a fresh view:\n{mine}\n{v['entries']}")
            say("= replica matches a fresh view")
        elif "count" in step:
            got = sum(1 for e in rep.view["entries"] if re.search(step["count"], json.dumps(e)))
            if got != step["n"]:
                fail(f"{got} entries match /{step['count']}/, want {step['n']}")
            say(f"= {got} entries match /{step['count']}/")
        elif "reconnect" in step:
            task.cancel()
            await ws.close()
            rep, inbox = Replica(), asyncio.Queue()
            ws, task = await connect(rep, inbox)
            say("= reconnected")
        else:
            if step.get("t") == "req" and "id" not in step:
                n += 1
                step = {**step, "id": f"r{n}"}
            say(f"> {json.dumps(step)[:400]}")
            await ws.send(json.dumps(step))
    task.cancel()
    await ws.close()


asyncio.run(main())
