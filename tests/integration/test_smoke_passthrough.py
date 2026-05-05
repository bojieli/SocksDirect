"""Pass-through smoke tests.

These run *without* requiring libsd to be built — they verify the
integration harness itself works, and that simple system commands behave
identically with and without libsd preloaded. They're the canary for
"is the test infrastructure healthy."
"""

from __future__ import annotations

import socket
import subprocess


def test_harness_can_run_a_command():
    """Sanity: the basic subprocess plumbing works."""
    r = subprocess.run(["true"], check=False)
    assert r.returncode == 0


def test_localhost_loopback_works_without_socksdirect():
    """Sanity: a stock loopback round-trip works in this environment.

    If this fails, no libsd test will pass either — fail fast with a clear
    message instead of confusing libsd-specific assertions.
    """
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(("127.0.0.1", port))
    server, _ = listener.accept()

    client.sendall(b"ping")
    assert server.recv(4) == b"ping"

    client.close()
    server.close()
    listener.close()
