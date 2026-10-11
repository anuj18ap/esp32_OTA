import http.server
import json
import logging
import socket
import threading
from pathlib import Path


def make_server(firmware, host="127.0.0.1", port=8000):
    image = Path(firmware).read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            logging.info("HTTP request path=%s client=%s", self.path, self.client_address[0])
            if self.path in ("/missing.bin", "/status.bin"):
                self.send_error(404 if self.path == "/missing.bin" else 503)
                return
            if self.path not in (
                "/valid.bin",
                "/truncated.bin",
                "/interrupted.bin",
                "/corrupt.bin",
            ):
                self.send_error(404)
                return
            body = image
            if self.path == "/corrupt.bin":
                body = bytes([image[0] ^ 0xFF]) + image[1:]
            if self.path == "/truncated.bin":
                body = image[: len(image) // 2]
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header(
                "Content-Length", str(len(image) if self.path == "/interrupted.bin" else len(body))
            )
            self.end_headers()
            try:
                self.wfile.write(
                    body[: len(body) // 2] if self.path == "/interrupted.bin" else body
                )
                self.wfile.flush()
                if self.path == "/interrupted.bin":
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
            except (BrokenPipeError, ConnectionError):
                logging.info("client closed download")

        def log_message(self, fmt, *args):
            logging.info(fmt, *args)

    return http.server.ThreadingHTTPServer((host, port), Handler)
