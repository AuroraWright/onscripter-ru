#!/usr/bin/env python3

"""Connect to the ONScripter-RU text WebSocket and print received text."""

import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
from urllib.parse import urlsplit


WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WebSocket:
    def __init__(self, url, timeout=None):
        parsed = urlsplit(url)
        if parsed.scheme != "ws" or not parsed.hostname:
            raise ValueError("URL must use the form ws://host:port/path")

        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.path = parsed.path or "/"
        if parsed.query:
            self.path += "?" + parsed.query
        self.socket = socket.create_connection((self.host, self.port), timeout)
        self.socket.settimeout(timeout)
        self.buffer = bytearray()
        self._handshake()

    def _handshake(self):
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        host = self.host if self.port == 80 else f"{self.host}:{self.port}"
        request = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.socket.sendall(request.encode("ascii"))

        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ConnectionError("server disconnected during the handshake")
            response.extend(chunk)
            if len(response) > 16384:
                raise ConnectionError("WebSocket handshake response is too large")

        header_data, remaining = response.split(b"\r\n\r\n", 1)
        lines = header_data.decode("iso-8859-1").split("\r\n")
        if len(lines[0].split()) < 2 or lines[0].split()[1] != "101":
            raise ConnectionError(f"WebSocket upgrade failed: {lines[0]}")

        headers = {}
        for line in lines[1:]:
            name, separator, value = line.partition(":")
            if separator:
                headers[name.lower()] = value.strip()

        expected = base64.b64encode(
            hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            raise ConnectionError("server returned an invalid Sec-WebSocket-Accept value")
        self.buffer.extend(remaining)

    def _read_exactly(self, size):
        while len(self.buffer) < size:
            chunk = self.socket.recv(max(4096, size - len(self.buffer)))
            if not chunk:
                raise EOFError("server disconnected")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def _read_frame(self):
        first, second = self._read_exactly(2)
        final = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._read_exactly(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._read_exactly(8))[0]
        mask = self._read_exactly(4) if masked else None
        payload = bytearray(self._read_exactly(length))
        if mask:
            for index in range(length):
                payload[index] ^= mask[index % 4]
        return final, opcode, bytes(payload)

    def _send_frame(self, opcode, payload=b""):
        mask = os.urandom(4)
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        header.extend(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.socket.sendall(header)

    def receive_text(self):
        message = bytearray()
        receiving_text = False
        while True:
            final, opcode, payload = self._read_frame()
            if opcode == 0x8:
                self._send_frame(0x8, payload[:125])
                return None
            if opcode == 0x9:
                self._send_frame(0xA, payload[:125])
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x1:
                message = bytearray(payload)
                receiving_text = True
            elif opcode == 0x0 and receiving_text:
                message.extend(payload)
            else:
                continue
            if final:
                return message.decode("utf-8")

    def close(self):
        if self.socket is None:
            return
        try:
            self._send_frame(0x8)
        except OSError:
            pass
        self.socket.close()
        self.socket = None


def main():
    parser = argparse.ArgumentParser(
        description="Print text streamed by ONScripter-RU's WebSocket server."
    )
    parser.add_argument(
        "url",
        nargs="?",
        default="ws://127.0.0.1:8765/",
        help="server URL (default: %(default)s)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="exit after this many messages; zero waits indefinitely",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        help="connection and receive timeout in seconds",
    )
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count cannot be negative")
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be greater than zero")

    client = None
    try:
        client = WebSocket(args.url, args.timeout)
        print(f"Connected to {args.url}", file=sys.stderr)
        received = 0
        while not args.count or received < args.count:
            text = client.receive_text()
            if text is None:
                print("Server closed the connection", file=sys.stderr)
                break
            print(text, flush=True)
            received += 1
    except KeyboardInterrupt:
        return 130
    except (ConnectionError, EOFError, OSError, UnicodeError, ValueError) as error:
        print(f"WebSocket error: {error}", file=sys.stderr)
        return 1
    finally:
        if client:
            client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
