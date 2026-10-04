import asyncio, json, re, sys, time
import websockets

port, token, script = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
log = []


def say(line):
    log.append(line)
    print(line, flush=True)


async def main():
    url = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(url, additional_headers={"Authorization": f"Bearer {token}"}) as ws:
        t0, inbox = time.time(), asyncio.Queue()

        async def reader():
            async for m in ws:
                await inbox.put(m)

        task = asyncio.create_task(reader())
        await ws.send(json.dumps({"t": "hello", "name": "rig"}))
        for step in script:
            if isinstance(step, (int, float)):
                await asyncio.sleep(step)
            elif "wait" in step:
                end = time.time() + step.get("secs", 5)
                while True:
                    try:
                        m = await asyncio.wait_for(inbox.get(), max(0.01, end - time.time()))
                    except asyncio.TimeoutError:
                        print(f"relayclient: no frame matched /{step['wait']}/ in {step.get('secs', 5)}s",
                              file=sys.stderr)
                        sys.exit(1)
                    say(f"{time.time() - t0:6.2f} < {m}")
                    if re.search(step["wait"], m):
                        break
            else:
                say(f"{time.time() - t0:6.2f} > {json.dumps(step)}")
                await ws.send(json.dumps(step))
        task.cancel()


asyncio.run(main())
