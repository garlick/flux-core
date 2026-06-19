# flux-rest-server MVP — Implementation Instructions

Step-by-step instructions to implement the **first, minimal version** of
`flux-rest-server`, a standalone HTTP front-end for Flux. The full design is in
`REST_API.md`; you do **not** need to read it to do this task — everything
required is here.

## What you are building

A small HTTP server, in Python, **using only the standard library** (no Flask or
other web frameworks), that:

- runs as an ordinary (unprivileged) process, as the invoking user;
- connects to the local Flux instance using the default Flux connector
  (`flux.Flux()` with no arguments — it uses `$FLUX_URI` / the local socket);
- exposes a few **read-only** JSON endpoints;
- works in two modes from the same code:
  1. **standalone** — binds a TCP `host:port` (for development/testing);
  2. **socket-activated** — inherits a listening socket from systemd (for the
     per-user deployment). Detected via the systemd `$LISTEN_FDS` protocol.

You will also deliver the **systemd unit templates**, a **polkit rule**, and an
**example nginx config** with deployment instructions, so the per-user front end
can be set up. Those are configuration artifacts (not auto-tested); the server
code is the part with tests.

## Scope — implement EXACTLY this

Three endpoints (all GET, all return `application/json`):

| Method & path          | Success (200) body                                                                                       |
|------------------------|----------------------------------------------------------------------------------------------------------|
| `GET /api/v1/`         | `{"name":"flux-rest-server","api_version":"v1","broker_version":<str>,"rank":<int>,"size":<int>}`        |
| `GET /api/v1/version`  | `{"server":<str>,"broker":<str>}`                                                                        |
| `GET /api/v1/health`   | `{"status":"ok"}`                                                                                        |

Behavior rules:

- `/api/v1/health` is a pure liveness check — it must **not** touch Flux.
- `/api/v1/` and `/api/v1/version` query Flux; if the Flux connection fails,
  respond **503** with `{"error":"flux unavailable","detail":<str>}`.
- Unknown path → **404** with `{"error":"not found","path":<str>}`.

### Explicitly OUT OF SCOPE (do not implement)

The site authentication itself, TLS, the `_ensure` trigger helper logic, job
submission, MUNGE signing, SSH-to-jobs, SSE/streaming, job listing/cancel, and
`/resources`. These are later phases. Provide the nginx/polkit/systemd artifacts
described below, but do not build the auth or trigger-helper code.

## How Flux is accessed (verified API)

```python
import flux
h = flux.Flux()                 # connects to local instance; raises OSError if none
h.attr_get("version")           # -> e.g. "0.86.0-73-g4caf10c8e"  (broker version)
h.attr_get("rank")              # -> "0"   (string; convert with int())
h.attr_get("size")              # -> "1"   (string; convert with int())
```

**Important:** the `flux` Python module is provided by flux-core, **not pip**.
The server must run where `import flux` already works — launched via
`flux python …` / `flux start …`, or with flux-core installed on the system. Do
not add `flux` to your dependencies and do not `pip install flux`.

## Project layout

```
flux-rest-server/
├── pyproject.toml
├── README.md
├── src/
│   └── flux_rest_server/
│       ├── __init__.py
│       ├── server.py
│       └── __main__.py
├── tests/
│   └── test_endpoints.py
├── systemd/
│   ├── flux-rest-server@.socket
│   └── flux-rest-server@.service
├── polkit/
│   └── 50-flux-rest-server.rules
└── nginx/
    └── flux-rest-server.conf.example
```

## File contents

### `pyproject.toml`

```toml
[project]
name = "flux-rest-server"
version = "0.1.0"
description = "Minimal REST/HTTP front-end for Flux (MVP)"
requires-python = ">=3.8"
dependencies = []                      # stdlib only; `flux` comes from flux-core

[project.scripts]
flux-rest-server = "flux_rest_server.__main__:main"

[build-system]
requires = ["setuptools>=61"]
build-backend = "setuptools.build_meta"
```

### `src/flux_rest_server/__init__.py`

```python
"""flux-rest-server: a minimal, stdlib-only HTTP front-end for Flux."""
```

### `src/flux_rest_server/server.py`

```python
"""HTTP <-> Flux translation using only the Python standard library.

Read-only endpoints under /api/v1/. Runs as the invoking user and connects to
the local Flux instance via the default connector. See REST_SERVER_MVP.md.
"""

import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from importlib.metadata import PackageNotFoundError, version as _pkg_version

import flux

API_VERSION = "v1"
SERVER_NAME = "flux-rest-server"
_PREFIX = f"/api/{API_VERSION}"

_handle = None


def _flux():
    """Return a cached Flux handle, creating it on first use.

    Raises OSError if no broker is reachable.
    """
    global _handle
    if _handle is None:
        _handle = flux.Flux()
    return _handle


def _server_version():
    try:
        return _pkg_version("flux-rest-server")
    except PackageNotFoundError:
        return "0+dev"


def _root():
    h = _flux()
    return 200, {
        "name": SERVER_NAME,
        "api_version": API_VERSION,
        "broker_version": h.attr_get("version"),
        "rank": int(h.attr_get("rank")),
        "size": int(h.attr_get("size")),
    }


def _version():
    h = _flux()
    return 200, {"server": _server_version(), "broker": h.attr_get("version")}


def _health():
    return 200, {"status": "ok"}


ROUTES = {
    f"{_PREFIX}/": _root,
    f"{_PREFIX}/version": _version,
    f"{_PREFIX}/health": _health,
}


class Handler(BaseHTTPRequestHandler):
    server_version = f"{SERVER_NAME}/{_server_version()}"

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        route = ROUTES.get(path)
        if route is None:
            self._send(404, {"error": "not found", "path": path})
            return
        try:
            status, body = route()
        except OSError as err:                    # Flux not reachable
            status, body = 503, {"error": "flux unavailable", "detail": str(err)}
        self._send(status, body)

    def _send(self, status, body):
        data = (json.dumps(body) + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def address_string(self):
        # client_address is '' for AF_UNIX peers (socket activation); the
        # default implementation assumes a (host, port) tuple and would fail.
        ca = self.client_address
        return ca[0] if isinstance(ca, tuple) else "unix"


def server_on_address(host, port):
    """Standalone/dev mode: bind and listen on host:port."""
    return HTTPServer((host, port), Handler)


def server_on_socket(listen_sock):
    """Socket-activation mode: serve on an already-listening socket
    (e.g. one passed by systemd). Works for AF_INET or AF_UNIX."""
    srv = HTTPServer(("", 0), Handler, bind_and_activate=False)
    try:
        srv.socket.close()        # discard the throwaway socket HTTPServer made
    except OSError:
        pass
    srv.address_family = listen_sock.family
    srv.socket = listen_sock
    srv.server_address = listen_sock.getsockname()
    return srv
```

### `src/flux_rest_server/__main__.py`

```python
"""Entry point: `flux-rest-server` / `python -m flux_rest_server`.

Serves on a systemd-provided socket if socket-activated, otherwise binds the
given --host/--port. The Flux handle is not thread-safe, so the server is
single-threaded (one request at a time) for this MVP.
"""

import argparse
import os
import socket

from .server import server_on_address, server_on_socket

_LISTEN_FDS_START = 3  # systemd passes the first listen fd here


def _socket_activated():
    return (
        os.environ.get("LISTEN_PID") == str(os.getpid())
        and int(os.environ.get("LISTEN_FDS", "0")) >= 1
    )


def main():
    parser = argparse.ArgumentParser(prog="flux-rest-server")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address in standalone mode (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080,
                        help="bind port in standalone mode (default 8080)")
    args = parser.parse_args()

    if _socket_activated():
        # family/type are auto-detected from the inherited fd on Linux.
        listen_sock = socket.socket(fileno=_LISTEN_FDS_START)
        srv = server_on_socket(listen_sock)
    else:
        srv = server_on_address(args.host, args.port)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
```

### `tests/test_endpoints.py`

```python
"""Endpoint tests. Run the whole suite inside a Flux instance:

    flux start pytest -v

`test_health` and `test_not_found` pass without Flux; the others need a
reachable broker, which `flux start` provides.
"""

import json
import threading
import urllib.error
import urllib.request

from flux_rest_server.server import server_on_address


def _start():
    srv = server_on_address("127.0.0.1", 0)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    host, port = srv.server_address[:2]
    return srv, f"http://{host}:{port}"


def _get(base, path):
    try:
        with urllib.request.urlopen(base + path) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as err:
        return err.code, json.loads(err.read())


def test_health():
    srv, base = _start()
    try:
        assert _get(base, "/api/v1/health") == (200, {"status": "ok"})
    finally:
        srv.shutdown()


def test_not_found():
    srv, base = _start()
    try:
        status, body = _get(base, "/api/v1/nope")
        assert status == 404 and body["error"] == "not found"
    finally:
        srv.shutdown()


def test_root():
    srv, base = _start()
    try:
        status, body = _get(base, "/api/v1/")
        assert status == 200
        assert body["name"] == "flux-rest-server"
        assert body["api_version"] == "v1"
        assert isinstance(body["broker_version"], str) and body["broker_version"]
        assert isinstance(body["rank"], int)
        assert isinstance(body["size"], int)
    finally:
        srv.shutdown()


def test_version():
    srv, base = _start()
    try:
        status, body = _get(base, "/api/v1/version")
        assert status == 200
        assert isinstance(body["broker"], str) and body["broker"]
        assert isinstance(body["server"], str) and body["server"]
    finally:
        srv.shutdown()
```

### `systemd/flux-rest-server@.socket`

```ini
[Unit]
Description=flux-rest-server socket for user %i

[Socket]
# Per-user listening socket. systemd creates it and activates the matching
# flux-rest-server@%i.service on first connection (Accept=no, the default).
ListenStream=/run/flux-rest-server-%i.sock
SocketUser=root
SocketGroup=nginx          # adjust to your nginx group (e.g. www-data)
SocketMode=0660            # so only nginx (and root) may connect

[Install]
WantedBy=sockets.target
```

### `systemd/flux-rest-server@.service`

```ini
[Unit]
Description=flux-rest-server for user %i

[Service]
User=%i                    # systemd performs the privileged setuid to the user
ExecStart=/usr/bin/flux-rest-server
# The server inherits the listening socket via $LISTEN_FDS (socket activation);
# no --host/--port needed. flux.Flux() connects to the system instance as the
# user. If it cannot find the instance automatically, uncomment and adjust:
# Environment=FLUX_URI=local:///run/flux/local
NoNewPrivileges=true
```

### `polkit/50-flux-rest-server.rules`

```javascript
// Allow the nginx user to start (only) per-user flux-rest-server socket units,
// so the front end can bring a user's service up on demand. Adjust "nginx" to
// your web-server user.
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.systemd1.manage-units" &&
        subject.user == "nginx" &&
        action.lookup("unit").indexOf("flux-rest-server@") == 0) {
        return polkit.Result.YES;
    }
});
```

### `nginx/flux-rest-server.conf.example`

```nginx
# Example front end. Terminates TLS, authenticates the user, ensures the user's
# flux-rest-server is running, and proxies to its per-user socket. Adjust the
# auth method, nginx user/group, hostname, and cert paths for your site.
server {
    listen 443 ssl;
    server_name HOSTNAME;

    ssl_certificate     /etc/pki/tls/certs/site.crt;
    ssl_certificate_key /etc/pki/tls/private/site.key;

    # --- site authentication: pick ONE; it must set $remote_user ---
    # auth_gss on;                                            # Kerberos
    # auth_basic "flux"; auth_basic_user_file /etc/flux/htpasswd;   # testing

    location /api/ {
        # Reject anything that isn't a plain username before using it in a path.
        if ($remote_user !~ "^[a-z_][a-z0-9_-]*$") { return 403; }

        auth_request /_ensure;                       # bring the user's socket up
        set $sock /run/flux-rest-server-$remote_user.sock;
        proxy_pass http://unix:$sock:/;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
    }

    # Internal helper (NOT implemented in this MVP). It should run, as the nginx
    # user and idempotently:  systemctl start flux-rest-server@$remote_user.socket
    # (authorized by the polkit rule), returning 2xx on success.
    location = /_ensure {
        internal;
        fastcgi_pass unix:/run/flux-rest-server-ensure.sock;
        include fastcgi_params;
    }
}
```

### `README.md`

One short paragraph (point to `REST_API.md` for the full design), plus the
install / run / test command snippets from below.

## Build, run, and test (server — this is the tested part)

Install editable, into an environment that also has `flux`:

```sh
pip install -e .
```

Run standalone inside a Flux instance (`flux start` sets `$FLUX_URI` for its
child; the server blocks, keeping the instance up):

```sh
flux start flux-rest-server --port 8080
```

Exercise it from another terminal:

```sh
curl -s http://127.0.0.1:8080/api/v1/        | python3 -m json.tool
curl -s http://127.0.0.1:8080/api/v1/version | python3 -m json.tool
curl -s http://127.0.0.1:8080/api/v1/health  | python3 -m json.tool
```

Run the test suite (the whole pytest run is the initial program of a one-node
instance):

```sh
flux start pytest -v
```

> Developing inside a flux-core source tree instead of against an installed
> Flux? Prefix with the tree wrappers, e.g.
> `./src/cmd/flux start ./src/cmd/flux python -m pytest -v`.

### Verify the socket-activation code path (no root needed)

Use `systemd-socket-activate` to hand the server a listening Unix socket exactly
as systemd would, then talk to it with curl's `--unix-socket`:

```sh
flux start systemd-socket-activate -l /tmp/flux-rest.sock flux-rest-server
# in another terminal:
curl -s --unix-socket /tmp/flux-rest.sock http://localhost/api/v1/health
curl -s --unix-socket /tmp/flux-rest.sock http://localhost/api/v1/
```

(`systemd-socket-activate` may need a full path, e.g.
`/usr/lib/systemd/systemd-socket-activate` or `/lib/systemd/...`.)

## Front-end deployment (manual — provided as artifacts, not auto-tested)

1. Install the server system-wide so `/usr/bin/flux-rest-server` exists and
   `import flux` works for arbitrary users.
2. Copy the unit files to `/etc/systemd/system/`, adjust `SocketGroup` to your
   nginx group, then `systemctl daemon-reload`.
3. Smoke-test per-user activation by hand (needs a system Flux instance):
   ```sh
   sudo systemctl start flux-rest-server@$USER.socket
   curl -s --unix-socket /run/flux-rest-server-$USER.sock http://localhost/api/v1/health
   curl -s --unix-socket /run/flux-rest-server-$USER.sock http://localhost/api/v1/
   ```
4. Install the polkit rule to `/etc/polkit-1/rules.d/`, adjusting the nginx user.
5. Install the nginx example, choose an auth method, and provide the `_ensure`
   helper (a later task — until then you can start the per-user socket unit
   manually as in step 3).

## Definition of done

1. `pip install -e .` succeeds; no third-party runtime dependencies.
2. `flux start flux-rest-server` serves; all three endpoints return the JSON
   shapes in the Scope table with HTTP 200.
3. `flux start pytest -v` passes (4 tests).
4. The socket-activation check above returns 200 for `/api/v1/health` and
   `/api/v1/`.
5. Running standalone **outside** any Flux instance, `/api/v1/` returns **503**
   `{"error":"flux unavailable",...}` while `/api/v1/health` returns 200.
6. The systemd units, polkit rule, and nginx example exist as listed.
7. No out-of-scope features were added.

## Conventions

- Python 3, standard library only (plus `flux` from flux-core); `black`-formatted.
- Keep HTTP/JSON/Flux logic in `server.py`; keep `__main__.py` to argument
  parsing, socket-activation detection, and launching.
```
