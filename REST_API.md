# Flux REST API: Final Design

## Design Principles

1. **Preserve Flux security model** - No compromises to defense-in-depth
2. **Leverage site infrastructure** - Use existing web security (Kerberos, OAuth, PKI)
3. **Minimal Flux-specific web code** - Flux speaks Unix sockets, sites handle HTTP/TLS
4. **Scale gracefully** - Works from 1 node to 100K nodes
5. **Sub-instance independence** - System instance has no special privilege in user jobs

## Security Requirements

### Must Preserve

✅ **Defense in depth**
- Multiple independent validation layers
- No single point of compromise

✅ **Sub-instance independence**  
- User's job is user's property
- System instance cannot execute code in user jobs
- Users can run modified Flux

✅ **Bidirectional distrust**
- Sub-instances don't trust system instance
- System instance validates user requests

### Must NOT Do

❌ **Server-side job signing** - Would allow compromised system to forge jobs
❌ **Flux-specific PKI** - Sites already have web security infrastructure  
❌ **TLS/authentication in Flux** - Sites handle via nginx, Flux uses Unix socket transport

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│ System Instance (Head/Login Node)                              │
│                                                                 │
│  nginx (site-provided TLS/auth)                                │
│    ↓                                                            │
│  http-to-socket proxy (fork/setuid per user)                   │
│    ↓                                                            │
│  /run/user/$UID/flux-http.sock (systemd user socket)          │
│    ↓ SO_PEERCRED                                               │
│  connector-http (listens on Unix socket)                       │
│    ↓                                                            │
│  flux-broker                                                    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ Sub-Instance (Compute Node, Job Rank 0)                        │
│                                                                 │
│  systemd --user socket (ephemeral port, socket-activated)      │
│    ↓                                                            │
│  nginx (runs as user, site-provided auth)                      │
│    ↓                                                            │
│  /tmp/flux-$JOBID/http.sock                                    │
│    ↓ SO_PEERCRED                                               │
│  connector-http (listens on Unix socket)                       │
│    ↓                                                            │
│  flux-broker (user's instance)                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Component Design

### 1. connector-http (Flux Module)

**Speaks HTTP protocol over Unix domain socket. No TLS, TCP, or authentication code.**

```python
# src/modules/connector-http/connector-http.py

import socket
import flux

def main():
    h = flux.Flux()
    
    # Create Unix socket (like connector-local)
    sock_path = os.path.join(flux.attr_get(h, "rundir"), "http.sock")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(128)
    
    while True:
        conn, addr = server.accept()
        
        # Get peer credentials (kernel-provided, unforgeable)
        ucred = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                               struct.calcsize('3i'))
        pid, uid, gid = struct.unpack('3i', ucred)
        
        # Determine role
        instance_owner = os.getuid()
        if uid == instance_owner:
            rolemask = FLUX_ROLE_OWNER
        else:
            rolemask = FLUX_ROLE_USER
        
        # Handle HTTP request with Flux RPCs
        handle_http_request(conn, uid, rolemask)
```

**Configuration:**
```toml
[connector-http]
enable = true
# That's it - just enable it. Unix socket path automatic.
```

**Security properties:**
- ✅ Gets UID from SO_PEERCRED (kernel-enforced)
- ✅ No authentication code (delegated to nginx)
- ✅ No TLS/certificate management (handled by nginx)
- ✅ No TCP networking (Unix socket transport only)
- ✅ Same security model as connector-local

### 2. Sub-Instance: Socket-Activated nginx

**Each job gets on-demand nginx, managed by systemd user instance.**

#### Flux Package Provides

**systemd socket unit** (`/usr/lib/systemd/user/flux-http@.socket`):
```ini
[Unit]
Description=Flux HTTP Socket for Job %i

[Socket]
# Bind to ephemeral port (systemd picks)
ListenStream=0.0.0.0:0
Accept=false

[Install]
WantedBy=sockets.target
```

**systemd service unit** (`/usr/lib/systemd/user/flux-http@.service`):
```ini
[Unit]
Description=Flux HTTP Service for Job %i

[Service]
Type=forking
ExecStart=/usr/sbin/nginx -c %t/flux-http-%i.conf
ExecStop=/usr/sbin/nginx -s quit -c %t/flux-http-%i.conf
# Linger 5 minutes after idle
TimeoutStopSec=300

[Install]
WantedBy=default.target
```

**nginx config template** (`/usr/share/flux/http.nginx.template`):
```nginx
daemon off;
pid {{RUNDIR}}/nginx.pid;
error_log {{RUNDIR}}/nginx-error.log;

events {
    worker_connections 256;
}

http {
    access_log {{RUNDIR}}/nginx-access.log;
    
    server {
        listen {{PORT}} ssl;
        
        # Site provides TLS cert and auth config
        include /etc/flux/http-site.conf;
        
        location / {
            # Proxy to Flux Unix socket (no auth needed - SO_PEERCRED)
            proxy_pass http://unix:{{FLUX_RUNDIR}}/http.sock;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
        }
    }
}
```

#### Job Bootstrap (Rank 0)

**Via modprobe task** in `etc/modprobe/rc1.py`:

```python
from flux.modprobe import task

@task(
    "setup-http",
    ranks="0",
    after=["connector-http"],
    needs=["connector-http"],
)
def setup_http_endpoint(context):
    jobid = os.environ["FLUX_JOB_ID"]
    flux_rundir = flux.attr_get(h, "rundir")
    user_rundir = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    
    # 1. Generate nginx config from template
    nginx_conf = f"{user_rundir}/flux-http-{jobid}.conf"
    generate_nginx_config(
        template="/usr/share/flux/http.nginx.template",
        output=nginx_conf,
        # PORT placeholder filled after systemd binds
        flux_rundir=flux_rundir,
        rundir=user_rundir
    )
    
    # 2. Start systemd socket (systemd binds to ephemeral port)
    subprocess.run([
        "systemctl", "--user", "start", f"flux-http@{jobid}.socket"
    ], check=True)
    
    # 3. Query the port systemd bound to
    result = subprocess.run([
        "systemctl", "--user", "show", f"flux-http@{jobid}.socket",
        "--property=Listen"
    ], capture_output=True, text=True, check=True)
    
    # Parse: "Listen=[::]:47293 (Stream)" or "Listen=0.0.0.0:47293 (Stream)"
    port = parse_port_from_systemd(result.stdout)
    
    # 4. Update nginx config with actual port
    update_nginx_config(nginx_conf, port=port)
    
    # 5. Advertise URI to parent via memo
    hostname = socket.gethostname()
    parent_uri = context.attr_get("parent-uri")
    if parent_uri:  # Only if we're a sub-instance
        parent_h = flux.Flux(parent_uri)
        parent_h.rpc("job-manager.memo", {
            "id": jobid,
            "memo": {
                "uri": f"https://{hostname}:{port}/"
            }
        })
```

**Security properties:**
- ✅ nginx runs as user (unprivileged)
- ✅ Socket activation (only runs when accessed)
- ✅ Site controls TLS and authentication
- ✅ Flux just provides Unix socket
- ✅ No port conflicts (systemd picks ephemeral)
- ✅ Scales to 100K nodes (only rank 0 per job)

### 3. System Instance: nginx + Proxy Service

**Multi-user system requires routing to per-user Unix sockets.**

#### nginx Configuration (Site Provides)

```nginx
# /etc/nginx/conf.d/flux.conf

upstream flux_proxy {
    server 127.0.0.1:8082;  # flux-http-proxy service
}

server {
    listen 443 ssl;
    server_name cluster.example.com;
    
    # Site's TLS configuration
    ssl_certificate /etc/pki/tls/certs/cluster.crt;
    ssl_certificate_key /etc/pki/tls/private/cluster.key;
    
    # Site's authentication (example: Kerberos)
    location /api/ {
        auth_gss on;
        auth_gss_keytab /etc/http.keytab;
        
        # Forward authenticated user to proxy
        proxy_pass http://flux_proxy;
        proxy_set_header X-Remote-User $remote_user;
        proxy_http_version 1.1;
    }
}
```

#### flux-http-proxy Service

**Standalone service (not Flux-specific, could be separate package).**

Accepts authenticated HTTP from nginx, connects to user's Unix socket:

```python
#!/usr/bin/env python3
# flux-http-proxy - bridge HTTP to per-user Unix sockets

import os
import pwd
import socket
import subprocess
from flask import Flask, request, Response

app = Flask(__name__)

@app.route('/', defaults={'path': ''}, methods=['GET', 'POST', 'PUT', 'DELETE'])
@app.route('/<path:path>', methods=['GET', 'POST', 'PUT', 'DELETE'])
def proxy(path):
    # Trust nginx-provided username (only from localhost)
    if request.remote_addr != "127.0.0.1":
        return "Forbidden", 403
    
    username = request.headers.get("X-Remote-User")
    if not username:
        return "Unauthorized", 401
    
    # Strip Kerberos realm if present
    username = username.split('@')[0]
    
    # Get UID
    try:
        uid = pwd.getpwnam(username).pw_uid
    except KeyError:
        return "Unknown user", 403
    
    # Connect to user's systemd socket as that user
    sock_path = f"/run/user/{uid}/flux-http.sock"
    
    # Fork/setuid to connect as user
    read_fd, write_fd = os.pipe()
    
    pid = os.fork()
    if pid == 0:
        # Child: become user
        os.close(read_fd)
        os.setuid(uid)
        
        try:
            # Connect to user's Flux Unix socket
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(sock_path)
            
            # Forward HTTP request
            http_request = build_http_request(request, path)
            sock.sendall(http_request)
            
            # Read response
            response = sock.recv(65536)
            
            # Write to parent via pipe
            os.write(write_fd, response)
        except Exception as e:
            os.write(write_fd, f"HTTP/1.1 502 Bad Gateway\r\n\r\n{e}".encode())
        finally:
            os._exit(0)
    
    # Parent: read response from child
    os.close(write_fd)
    os.waitpid(pid, 0)
    
    response_data = os.read(read_fd, 65536)
    os.close(read_fd)
    
    # Parse and return HTTP response
    return parse_http_response(response_data)

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8082)
```

**systemd service** (`/etc/systemd/system/flux-http-proxy.service`):
```ini
[Unit]
Description=Flux HTTP to Unix Socket Proxy
After=network.target

[Service]
Type=simple
ExecStart=/usr/libexec/flux-http-proxy
# Needs CAP_SETUID to fork/setuid
AmbientCapabilities=CAP_SETUID
# Drop other privileges
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

**Alternative: Per-user systemd sockets + socket activation**

Sites could use systemd socket activation per user instead of fork/setuid:

```ini
# /etc/systemd/user/flux-http-handler.socket
[Socket]
ListenStream=%t/flux-http.sock

[Install]
WantedBy=sockets.target
```

Then nginx directly connects to `/run/user/$UID/flux-http.sock` (requires nginx to run as each user, or use systemd's DynamicUser feature).

**Security properties:**
- ✅ Site controls authentication completely
- ✅ fork/setuid ensures correct user context
- ✅ Each user's socket isolated by permissions
- ✅ No Flux-specific auth code

## Site Integration

### What Sites Must Provide

**Single configuration file** (`/etc/flux/http-site.conf`):

```nginx
# TLS configuration
ssl_certificate /etc/pki/tls/certs/site.crt;
ssl_certificate_key /etc/pki/tls/private/site.key;

# Authentication method (site chooses)

# Option 1: Kerberos
auth_gss on;
auth_gss_keytab /etc/http.keytab;

# Option 2: Mutual TLS
# ssl_client_certificate /etc/pki/ca.crt;
# ssl_verify_client on;

# Option 3: OAuth/OIDC (via auth_request)
# auth_request /oauth2/auth;

# Option 4: Simple capability token (for sub-instances)
# if ($http_authorization != "Bearer ${FLUX_HTTP_TOKEN}") {
#     return 403;
# }
```

**That's it.** Sites use their existing:
- TLS certificates
- Authentication infrastructure (Kerberos, LDAP, PKI, OAuth)
- nginx expertise

### What Flux Provides

**Packages:**
- `flux-core` - includes connector-http module
- `flux-http-proxy` (optional) - for system instance multi-user routing
- systemd unit files for socket activation
- nginx config template

**Documentation:**
- Example nginx configurations for common auth methods
- Integration guide for different HPC environments

## Job Signing Revisited

### User Sub-Instances: No Signing Required

**Key insight:** User instances don't use IMP, so no privilege escalation occurs.

```
User's job:
  flux-broker runs as user
  flux-shell runs as user
  Tasks run as user
  → No IMP involved, no signature needed
```

Jobs submitted to user instances can be unsigned!

**Configuration:**
```toml
# Sub-instance (automatic)
[sign]
default-type = "none"
allowed-types = ["none"]
# Owner can submit unsigned jobs
```

### System Instance: Allocation Jobs May Not Need Signing

**Current:** All jobs require MUNGE signatures (validated by IMP before exec)

**Possible relaxation:** If job only allocates resources (doesn't run user code directly):

```
User submits allocation request:
  jobspec = {resources: [...], attributes: {system: {shell: {}}}}
  → Gets allocation
  → flux-shell starts user instance
  → User can now submit real work inside (unsigned)
```

**Rationale:**
- Allocation jobspec is minimal (resources only)
- Real work runs in user instance (doesn't need signature)
- IMP still validates if allocation jobspec spawns tasks

**This could allow unsigned allocation requests,** but:
- Requires careful policy design
- Out of scope for initial REST API
- Can be added later without breaking API

### For REST API: MUNGE Signing Service

For job submission to system instance (if signatures required):

**Separate standalone service** (not Flux-specific):
- Input: Authenticated UID + payload
- Output: MUNGE credential signed as that UID
- Generic: works with Flux, Slurm, or any MUNGE-using system

See separate design doc: `munge-signing-service.md` (to be created)

This enables external clients (CI, web UIs) to submit jobs without local MUNGE daemon.

## API Endpoints

All under `/api/v1/`:

### Authentication
- `POST /auth/login` - Get credentials (if JWT-based)

### Jobs
- `GET /jobs` - List jobs
- `POST /jobs` - Submit job
- `GET /jobs/{id}` - Job details
- `DELETE /jobs/{id}` - Cancel job
- `GET /jobs/{id}/eventlog` - Stream eventlog (Server-Sent Events)
- `GET /jobs/{id}/output` - Stream output (SSE)

### Resources
- `GET /resources` - Resource status

### System
- `GET /` - API info, version

## Deployment Scenarios

### Scenario 1: HPC Site with Kerberos

**System instance:**
```
nginx (Kerberos auth) → flux-http-proxy → per-user sockets
```

**Sub-instances:**
```
Socket-activated nginx (token auth) → Flux Unix socket
```

**Site provides:**
- `/etc/flux/http-site.conf` with Kerberos config
- `/etc/nginx/conf.d/flux.conf` for system instance

### Scenario 2: Cloud/Containerized

**System instance:**
```
nginx (OAuth/OIDC) → flux-http-proxy → per-user sockets
```

**Sub-instances:**
```
Socket-activated nginx (JWT validation) → Flux Unix socket
```

**Site provides:**
- OAuth provider integration in nginx
- JWT validation in `/etc/flux/http-site.conf`

### Scenario 3: Development/Testing

**System instance:**
```
nginx (HTTP Basic Auth) → flux-http-proxy → per-user sockets
```

**Sub-instances:**
```
Socket-activated nginx (no auth, localhost only) → Flux Unix socket
```

**Site provides:**
- Simple htpasswd file for testing

## Implementation Phases

### Phase 1: Connector-http (Unix Socket Only)
- Python module listening on Unix socket
- HTTP/REST request parsing
- Flux RPC translation
- ~1000 LOC

### Phase 2: Sub-Instance Socket Activation
- systemd unit files
- nginx config template
- Bootstrap integration (memo advertisement)
- ~500 LOC

### Phase 3: System Instance Proxy
- flux-http-proxy service (fork/setuid)
- systemd service file
- Documentation for nginx integration
- ~500 LOC (separate package)

### Phase 4: Polish
- Server-Sent Events for streaming
- OpenAPI specification
- Rate limiting, metrics
- ~500 LOC

## Security Analysis

### Attack Scenarios

**Attack 1: Compromised System Instance Broker**
- ❌ Cannot create MUNGE credentials (no signing service access)
- ❌ Cannot execute in user sub-instances (no privilege)
- ❌ Cannot forge SO_PEERCRED (kernel-enforced)
- ✅ Can only read guest-accessible data

**Attack 2: Compromised nginx**
- ❌ Cannot forge user identity to flux-http-proxy (needs setuid)
- ❌ Cannot directly access Flux (no socket permissions)
- ✅ Can intercept plaintext (requires HTTPS)

**Attack 3: Compromised flux-http-proxy**
- ❌ Cannot access user data without valid username from nginx
- ❌ Cannot bypass Unix socket permissions (kernel-enforced)
- ✅ Could setuid to any user (requires CAP_SETUID)
  - Mitigation: Audit logs, minimal privileges, separate package

**Attack 4: User Compromise**
- ✅ User can access their own jobs (expected)
- ❌ Cannot access other users' jobs (Unix socket permissions)
- ❌ Cannot compromise system instance (no privilege escalation)

### Defense in Depth Layers

1. **Site authentication** (nginx/Kerberos/OAuth)
2. **flux-http-proxy validation** (username from trusted source)
3. **Unix socket permissions** (kernel-enforced)
4. **SO_PEERCRED validation** (kernel-provided UID)
5. **Flux authorization** (rolemask checks)
6. **Job signature validation** (if used - IMP layer)

**Six independent layers. Compromise requires breaking multiple.**

## Open Questions

1. **flux-http-proxy implementation language?** Python (prototype) or C (performance)?
2. **Connection pooling?** Keep persistent connections to user sockets?
3. **systemd user instance availability?** All HPC sites have this?
4. **Alternative to fork/setuid?** systemd DynamicUser, or per-user socket files?
5. **MUNGE signing service scope?** Include in initial release or defer?

## Success Criteria

✅ **Security preserved**
- No new privilege escalation paths
- Defense-in-depth maintained
- Sub-instance independence intact

✅ **Site control**
- Sites use existing auth infrastructure
- No Flux-specific PKI/certs required
- Standard nginx configuration

✅ **Scalability**
- Works at 100K node scale
- Minimal overhead on idle nodes
- Socket activation avoids persistent services

✅ **Maintainability**
- No TLS/authentication code in Flux
- HTTP protocol only, transport delegated to nginx
- Clear separation of concerns
- Standard protocols throughout

## References

- **connector-local** - Unix socket connector implementation
- **systemd.socket(5)** - Socket activation documentation  
- **nginx** - Reverse proxy and authentication
- **SO_PEERCRED** - Unix credential passing
- **RFC 15** - Independent Minister of Privilege (IMP)
