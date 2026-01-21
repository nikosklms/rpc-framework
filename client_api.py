#!/usr/bin/env python3
"""
client_api.py - Fixed private implementation
"""

import threading
import socket
import queue
import struct
import os
import random
import time

__all__ = ['init', 'doRequestReply']

class __ClientCore:
    def __init__(self):
        self.__send_queue = queue.Queue()
        self.__pending_requests = {}
        self.__pending_lock = threading.Lock()
        self.__used_request_ids = set()
        self.__udp_socket = None
        self.__server_address = None
        self.__running = True
        self.__servers = []
        self.__server_index = 0
        self.__PERSIST_LOG = "client_requests.log"
        
        print("[Core] Initializing...")
        self.__setup_network()
        
    def __setup_network(self):
        try:
            self.__udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.__udp_socket.settimeout(1.0)
            threading.Thread(target=self.__udp_sender_thread, daemon=True).start()
            threading.Thread(target=self.__udp_listener_thread, daemon=True).start()
            print("[Network] Ready")
        except Exception as e:
            print(f"[ERROR] Network setup failed: {e}")
            self.__running = False

    def __load_servers(self, configfname):
        print(f"[Config] Loading {configfname}...")
        self.__servers = []
        with open(configfname, "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 2:
                    self.__servers.append((parts[0], int(parts[1])))
        print(f"[Config] Found {len(self.__servers)} servers")

    def __get_next_server(self):
        server = self.__servers[self.__server_index % len(self.__servers)]
        self.__server_index += 1
        print(f"[Balancer] Selected server: {server[0]}:{server[1]}")
        return server[0], server[1]

    def __request_reply(self, svcid, reqbuf, reqlen, rspbuf, rsplen):
        print("="*80)
        print(f"[Request] Starting (svcid={svcid}, len={reqlen})")
        
        reqid = random.randint(0, (2**31)-1)
        while reqid in self.__used_request_ids:
            reqid = random.randint(1, (2**31)-1)
        self.__used_request_ids.add(reqid)
        print(f"[Request] Assigned REQID: {reqid}")

        # Set server address BEFORE logging
        self.__server_address = self.__get_next_server()
        
        response_q = queue.Queue()
        with self.__pending_lock:
            self.__pending_requests[reqid] = response_q
        
        self.__log_request(reqid)  
        packet = struct.pack("!iii", reqid, svcid, reqlen) + reqbuf
        last_packet = packet
        self.__send_queue.put(packet)
        print(f"[Network] Sent to {self.__server_address}")

        retry_count = 0
        while retry_count < 4:
            try:
                response = response_q.get(timeout=5)
                resp_reqid, resp_len = struct.unpack("!ii", response[:8])
                
                if resp_reqid == reqid:
                    print(f"[Response] Success (REQID: {reqid})")
                    rspbuf[:] = response[8:8+resp_len]
                    rsplen[0] = resp_len
                    self.__remove_log_entry(reqid)
                    return 0
                elif resp_reqid == -reqid:
                    print(f"[Heartbeat] Received (REQID: {reqid})")
                    heartbeat_ack = struct.pack("!iii", resp_reqid, svcid, 0) + b''
                    print(f"Sent ack for heartbeat for reqid {reqid}")
                    #print(f"(debug) putting {heartbeat_ack} in send_queue...")
                    last_packet = heartbeat_ack
                    self.__send_queue.put(heartbeat_ack)
                    continue
                  
            except queue.Empty:
                retry_count += 1
                if retry_count >= 4:
                    break
                print(f"[Timeout] Retry {retry_count}/3 (REQID: {reqid})")
                self.__send_queue.put(last_packet)
        
        print(f"[Error] Failed after {retry_count-1} retries (REQID: {reqid}). No heartbeat was received")
        self.__remove_log_entry(reqid)
        return -1

    def __udp_sender_thread(self):
        while self.__running:
            try:
                packet = self.__send_queue.get(timeout=0.5)
                reqid = struct.unpack("!i", packet[:4])[0]
                dest = self.__server_address  # Παίρνουμε τη διεύθυνση που χρησιμοποιείται για την αποστολή
                print(f"[Sender] Sending (REQID: {reqid}) to {dest[0]}:{dest[1]}")
                self.__udp_socket.sendto(packet, dest)
            except queue.Empty:
                continue
            except Exception as e:
                print(f"[ERROR] Send failed: {e}")

    def __udp_listener_thread(self):
        while self.__running:
            try:
                packet, addr = self.__udp_socket.recvfrom(4096)
                reqid, rsp_len = struct.unpack("!ii", packet[:8])
                print(f"[Listener] Received from {addr} (REQID: {reqid})")
                
                with self.__pending_lock:
                    if abs(reqid) in self.__pending_requests:
                        self.__pending_requests[abs(reqid)].put(packet)
            except socket.timeout:
                continue
            except Exception as e:
                print(f"[ERROR] Receive failed: {e}")

    def __log_request(self, reqid):
        ip, port = self.__server_address
        with open(self.__PERSIST_LOG, "a") as f:
            f.write(f"{ip} {port} {reqid}\n")

    def __remove_log_entry(self, reqid):
        if not os.path.exists(self.__PERSIST_LOG):
            return
        
        new_lines = []
        with open(self.__PERSIST_LOG, "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) == 3:
                    ip, port, log_reqid = parts[0], int(parts[1]), int(parts[2])
                    if log_reqid == reqid and (ip, port) == self.__server_address:
                        continue  # Skip this line (do not add it to new_lines)
                new_lines.append(line)  # Keep all other lines
        
        with open(self.__PERSIST_LOG, "w") as f:
            f.writelines(new_lines)

    def __process_pending_log(self):
        pending = []
        if os.path.exists(self.__PERSIST_LOG):
            with open(self.__PERSIST_LOG, "r") as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) == 3:
                        pending.append((parts[0], int(parts[1]), int(parts[2])))
        return pending

    def __get_server_for_reqid(self, reqid):
        if not os.path.exists(self.__PERSIST_LOG):
            return None
        with open(self.__PERSIST_LOG, "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3 and int(parts[2]) == reqid:
                    return (parts[0], int(parts[1]))
        return None

    def __send_cancel_request(self, reqid):
        new_address = self.__get_server_for_reqid(reqid)
        if new_address is not None:
            self.__server_address = new_address
            print(f"[cancel request] Updated server address to {self.__server_address} for reqid {reqid}")
        else:
            print(f"[cancel request] No server address found for reqid {reqid} in persist file, using current {self.__server_address}")

        response_q = queue.Queue()
        with self.__pending_lock:
            self.__pending_requests[reqid] = response_q

        packet = struct.pack("!iii", reqid, -1, 0) + b""
        self.__send_queue.put(packet)

        try:
            response_l = response_q.get(timeout=5)
        except queue.Empty:
            response_l = b""
        finally:
            with self.__pending_lock:
                if reqid in self.__pending_requests:
                    del self.__pending_requests[reqid]

        if response_l:
            resp_reqid, resp_len = struct.unpack("!ii", response_l[:8])
            resp_buf = response_l[8:8+resp_len].decode()
            print(f"[cancel request] reqid: {resp_reqid} resplen: {resp_len} response: {resp_buf}")

        print(f"Removed request {reqid} from the log file.")
        self.__remove_log_entry(reqid)



# Singleton instance
__api_core = __ClientCore()

# Public API
def init(configfname):
    """Initialize the client API"""
    print(f"[API] Initializing with {configfname}")
    __api_core._ClientCore__load_servers(configfname)
    
    pending = __api_core._ClientCore__process_pending_log()
    for _, _, reqid in pending:
        __api_core._ClientCore__send_cancel_request(reqid)


def doRequestReply(svcid, reqbuf, reqlen, rspbuf, rsplen):
    """Execute request-reply cycle"""
    print(f"[API] New request (svcid={svcid})")
    return __api_core._ClientCore__request_reply(svcid, reqbuf, reqlen, rspbuf, rsplen)