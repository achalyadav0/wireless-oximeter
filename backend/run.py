import os
import socket
import sys
import threading
import webbrowser

from app import create_app


app = create_app()


def _lan_address() -> str | None:
    """Return this machine's address on the local network, if any."""

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            # No packet is sent; connecting a UDP socket only picks a route.
            probe.connect(("10.255.255.255", 1))
            return probe.getsockname()[0]
    except OSError:
        return None


def _has_display() -> bool:
    # Over SSH there is no desktop, and Python would fall back to a
    # terminal browser that takes over the session.
    return sys.platform == "darwin" or bool(
        os.getenv("DISPLAY") or os.getenv("WAYLAND_DISPLAY")
    )


if __name__ == "__main__":
    host = os.getenv("FLASK_HOST", "127.0.0.1")
    port = int(os.getenv("FLASK_PORT", "5000"))

    local_url = f"http://127.0.0.1:{port}/"
    print(f"Dashboard: {local_url}")

    lan_address = _lan_address()

    if host == "0.0.0.0" and lan_address:
        print(f"On your network: http://{lan_address}:{port}/")

    if os.getenv("OPEN_BROWSER", "1") != "0" and _has_display():
        threading.Timer(1.0, webbrowser.open, args=(local_url,)).start()

    app.run(host=host, port=port)
