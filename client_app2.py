#!/usr/bin/env python3
"""
client_app.py – Client Application using the Client API
"""

import client_api
import time

if __name__ == '__main__':
    config_filename = "config.txt"
    client_api.init(config_filename)
    while True:
        number = input("[APP] Enter a number (or type 'exit' to quit): ")
        if number.lower() in ['exit', 'quit']:
            print("[APP] Exiting the application.")
            break
        number_bytes = number.encode()
        #number_bytes = b''  # NULL payload (empty byte string)
        response = bytearray(1024)  # Use a list to capture the response
        rsplenn = [0]
        start_time = time.time()
        result = client_api.doRequestReply(1, number_bytes, len(number), response, rsplenn)
        end_time = time.time()
        delay = end_time - start_time
        print(f"[APP] Request-Reply Delay: {delay} seconds")
        if result == 0:
            print(f"[APP] Server Response: {response.decode()}, Response Length: {rsplenn}")
        else:
            print("[APP] Request failed:", response[0])