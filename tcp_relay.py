"""
TCP Relay (Windows native) untuk meneruskan koneksi MQTT dari jaringan Wi-Fi
ke NanoMQ yang berjalan di WSL2.

Alasan: koneksi masuk dari LAN ke WSL2 diblokir oleh Hyper-V firewall
(DefaultInboundAction = Block), sedangkan akses 127.0.0.1 ke WSL tetap berfungsi.
Relay ini listen di Windows (0.0.0.0:1884) lalu meneruskan ke 127.0.0.1:1885.

ESP32  --TCP-->  Windows 0.0.0.0:1884  --relay-->  127.0.0.1:1885 (NanoMQ di WSL)
"""
import asyncio
import sys

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 1884
TARGET_HOST = "127.0.0.1"
TARGET_PORT = 1885


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(4096)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle_client(client_reader, client_writer):
    peer = client_writer.get_extra_info("peername")
    try:
        target_reader, target_writer = await asyncio.open_connection(TARGET_HOST, TARGET_PORT)
    except OSError as e:
        print(f"[RELAY] Gagal konek ke NanoMQ {TARGET_HOST}:{TARGET_PORT} ({e}). Apakah NanoMQ aktif?", flush=True)
        client_writer.close()
        return

    print(f"[RELAY] Klien terhubung: {peer[0]}:{peer[1]} -> NanoMQ", flush=True)
    await asyncio.gather(
        pipe(client_reader, target_writer),
        pipe(target_reader, client_writer),
    )
    print(f"[RELAY] Klien terputus: {peer[0]}:{peer[1]}", flush=True)


async def main():
    server = await asyncio.start_server(handle_client, LISTEN_HOST, LISTEN_PORT)
    print(f"[RELAY] Listen {LISTEN_HOST}:{LISTEN_PORT} -> {TARGET_HOST}:{TARGET_PORT}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)

