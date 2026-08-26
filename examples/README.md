# Examples

## webrtc_test_client.html

A self-contained, dependency-free HTML page for exercising a running `mezon-sfu` instance directly from the browser — camera publish (VP8), screen share (VP9 L1T3, up to 1080p), and multi-peer viewing, all without any signaling server code of your own.

### Prerequisites

* A running `mezon-sfu` instance (see the [main README](../README.md#running-the-server)).
* The client must be served over HTTP/HTTPS — opening the file directly from disk (`file://`) will fail because modern browsers restrict local network / mDNS discovery for WebRTC in that context.

### Serving the file

Any static file server works. For example, from this directory:

```sh
python3 -m http.server 3000
```

Then open `http://localhost:3000/webrtc_test_client.html`.

### Connecting

On the join screen, fill in:

| Field | Description |
| --- | --- |
| **Signaling WebSocket URL** | The SFU's WebSocket signaling endpoint, e.g. `ws://127.0.0.1:8000/ws`. |
| **Room ID** | The room to join. Any string; the SFU creates the room on first join. |
| **JWT Secret** | Must match the `jwt_secret` configured in the SFU's `config.ini`. The client auto-generates a valid JWT from the Room ID once this is set — for testing only, don't reuse this flow in production. |
| **Your name** | Display name shown to other peers in the room. |

Use the dropdown next to **Join** to choose a join mode:

* **Join as speaker** — publishes your camera/microphone and can screen share.
* **Join as audience** — joins receive-only; no local tracks are sent until promoted to speaker.

### In the room

* Toggle mic/camera and start/stop screen share from the control bar.
* Speaking peers are highlighted with a colored border (driven by the SFU's active-speaker signaling).
* Click a screen-share tile to expand it full-screen.
* The connection-state dot in the top bar reflects signaling/ICE health (green = ok, yellow = warning, red = error).

### Notes

* This client is a diagnostic tool, not a reference implementation — it inlines all HTML/CSS/JS in one file for easy hosting, so it doesn't reflect how a production client should be structured.
* See [Editor & debugger integration](../README.md#editor--debugger-integration-zed--vs-code) in the main README if you want to run the SFU itself under a debugger while testing against this client.
