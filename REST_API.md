# Flux REST API: Design Sketch

> **Status: early design sketch.** Likely a standalone project
> (`flux-rest-server`) depending only on flux-core's public client API — it needs
> no flux-core changes. See `REST_SERVER_MVP.md` for the first build target.

## Key idea

Run the HTTP↔RPC translation in an **ordinary, unprivileged Flux client that
runs as the requesting user** — not a broker module. nginx authenticates the
user; the per-user client is started **as that user** (by systemd — Component 2).
From there it behaves exactly like the `flux` CLI on a login node:

- reaches the **system instance** over `local://` (guest role from `SO_PEERCRED`);
- reaches the **user's own jobs** over `ssh://` (login as the user, end-to-end
  encrypted, the `flux` broker out of the data path);
- **signs its own submissions** with MUNGE as the user.

Because it *is* the user, the whole thing has the **same trust as a CLI session**:
no privileged on-behalf-of component, no signing service, no broker-resident
network code, no IMP changes. This is the Open OnDemand pattern (a privileged
stager spawns a per-user web process); here even the privileged step is delegated
to systemd + polkit, so we write no setuid binary at all.

It reuses what Flux already has — `local://`, `ssh://` (`flux proxy ssh://…`,
`flux uri --remote`), `SO_PEERCRED` identity — and delegates TLS/auth to the
site's nginx.

## Architecture

```
external client
  │  HTTPS
  ▼
nginx  (site TLS + authn; sets $remote_user)
  │  auth_request → start flux-rest-server@USER.socket   (systemd does setuid;
  │                                                        polkit authorizes)
  ▼  proxy_pass → /run/flux-rest-server-USER.sock
per-user flux-rest-server   ── runs AS the user, unprivileged ──
  │   parses HTTP/SSE, translates ↔ Flux RPC, signs as the user
  ├─ local://                  → system instance (guest role; SO_PEERCRED)
  └─ ssh://node/…/local        → the user's job  (owner; e2e, broker not in path)
```

No HTTP server is embedded in any broker.

## Components

### 1. Per-user flux-rest-server (unprivileged, runs as the user)

A small HTTP server, an ordinary libflux client, run as a socket-activated
per-user systemd service (`flux-rest-server@USER.service`, `User=%i`):

- Parses HTTP/REST (incl. SSE/chunked for streaming). The whole parser is
  **unprivileged** — an exploit is confined to that one user's session.
- Opens `local://` for the system instance (guest role, automatic from
  `SO_PEERCRED`), and `ssh://` for the user's jobs (resolved via
  `flux uri --remote`).
- Signs submissions with MUNGE as the user — identical to `flux submit`.
- Must persist for a connection's lifetime (and idle-time-out across them) to
  serve streaming/keep-alive; a per-request spawn cannot.

### 2. Per-user activation (systemd + polkit — no custom privileged binary)

systemd performs the privileged `setuid` via a templated `User=%i` service;
**polkit** authorizes nginx to trigger it. (polkit is Linux's authorization
broker: systemd asks it whether a subject may perform an action; a narrow rule
grants exactly one.) So we ship only declarative config plus a tiny *unprivileged*
trigger:

- a `flux-rest-server@.socket` (per-user `/run/flux-rest-server-%i.sock`,
  group-restricted to nginx) and `@.service` (`User=%i`, inherits the socket);
- a polkit rule letting the nginx user start *only* `flux-rest-server@*` units;
- an nginx `auth_request` helper that runs `systemctl start
  flux-rest-server@$remote_user.socket` (idempotent).

The privileged actor is **systemd** (already trusted); we add no setuid binary.
`$remote_user` flows into a socket path / unit name, so it **must be sanitized**
(reject `/`, `..`, non-username chars). Full unit/polkit/nginx files are in
`REST_SERVER_MVP.md`. Alternatives if this doesn't fit: `systemd --user` (needs
lingering), or a minimal custom `setuid` launcher (OnDemand `nginx_stage`).

### 3. Reaching the user's jobs over `ssh://`

Resolve a job to its `ssh://host/…socket` URI (as `flux proxy JOBID` does) and
open it. The `flux` broker is out of the data path; confidentiality/integrity
come from SSH host/user keys it cannot forge. Sub-instances are single-user
(owner only, no guests) so submissions there are unsigned (`[sign] type=none`).

Caveats: assumes users can SSH to their allocated nodes (common at HPC sites, not
universal) and that `flux` is present there (it is, inside the job); watch
per-connection SSH cost at high request rates.

## Security (short, because there isn't much)

Everything we run is unprivileged and acts only as the authenticated user, so the
attack surface is essentially a CLI session's:

- **Per-user service / HTTP parser** — runs as the user, guest role; an exploit
  reaches only that user's own jobs. No cross-user reach, no escalation.
- **Activation glue (nginx, trigger helper)** — unprivileged; polkit lets it
  start *only* `flux-rest-server@*` units. The real `setuid` is systemd's.
- **`flux` broker** — unprivileged; cannot `setuid`, read user keys, make the IMP
  launch without a user signature, or snoop/tamper `ssh://` job traffic.
- **IMP** — unchanged; submissions are signed by the user, validated at exec as
  always.

## Site integration

```nginx
ssl_certificate     /etc/pki/tls/certs/site.crt;
ssl_certificate_key /etc/pki/tls/private/site.key;

# Site auth — pick one; must set $remote_user:
auth_gss on;                       # Kerberos
# ssl_verify_client on;            # mutual TLS
# auth_request /oauth2/auth;       # OAuth/OIDC

location /api/ {
    if ($remote_user !~ "^[a-z_][a-z0-9_-]*$") { return 403; }   # sanitize
    auth_request /_ensure;                       # start the user's socket
    set $sock /run/flux-rest-server-$remote_user.sock;
    proxy_pass http://unix:$sock:/;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
}
location = /_ensure { internal; fastcgi_pass unix:/run/flux-rest-server-ensure.sock; }
```

Sites reuse their existing certs/auth/nginx; the deployment looks like Open
OnDemand.

## API endpoints

Under `/api/v1/`: `GET /` (info/version), `GET|POST /jobs`, `GET|DELETE
/jobs/{id}`, `GET /jobs/{id}/eventlog` and `/output` (SSE), `GET /resources`.
Authorization is owner-vs-guest from the rolemask; scoped/read-only tokens are
future work.

## Implementation phases

1. **Per-user service** — standalone HTTP→RPC over `local://`, no privileged
   parts. (This is the MVP — see `REST_SERVER_MVP.md`.)
2. **Per-user activation** — systemd units + polkit rule + `_ensure` helper +
   nginx.
3. **Jobs over `ssh://`** — resolve job → URI, proxy to sub-instances.
4. **Submission + polish** — sign as the user; SSE hardening; OpenAPI; limits.

## Open questions

1. **Service lifecycle** — idle timeout, per-user limits, auth-token expiry
   mid-stream for long SSE connections.
2. **SSH-to-node assumption** — which sites disallow it, and the fallback there.
3. **Activation** — systemd+polkit (preferred) vs `systemd --user` vs OnDemand's
   `nginx_stage` vs a custom launcher; and whether per-request `systemctl start`
   via `auth_request` needs caching.
4. **Discovery** — standardize job → `ssh://` URI (reuse `flux uri --remote`).
5. **SSH cost** — pool/reuse `ssh://` handles to the same job.

## References

- **`ssh://` connector / `flux proxy` / `flux uri --remote`** — e2e,
  user-authenticated access to a remote instance's socket.
- **`librouter`** (`usock.c`, `auth.c`) — `SO_PEERCRED` identity and rolemask.
- **Open OnDemand** (`nginx_stage` / per-user PUN) — the per-user-web-process
  pattern.
- **systemd.socket(5)** templated `User=%i` units; **polkit** — the activation
  mechanism.
</content>
