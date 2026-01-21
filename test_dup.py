#!/usr/bin/env python3
import client_api
import struct
import socket
import time

client_api.init("config.txt")

# Get server address from config
with open("config.txt") as f:
    parts = f.readline().strip().split()
    server = (parts[0], int(parts[1]))

# Create UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(2)

# Same request ID for both packets (simulating duplicate)
reqid = 999999
svcid = 1
payload = b"17"

# Create packet manually
packet = struct.pack("!iii", reqid, svcid, len(payload)) + payload

print(f"[TEST] Sending request 1 with reqid={reqid}")
sock.sendto(packet, server)
time.sleep(0.1)

print(f"[TEST] Sending DUPLICATE request with reqid={reqid}")
sock.sendto(packet, server)

# Wait for responses
try:
    while True:
        data, addr = sock.recvfrom(4096)
        resp_id = struct.unpack("!i", data[:4])[0]
        print(f"[TEST] Received response for reqid={resp_id}")
except socket.timeout:
    pass

print("[TEST] Done")