# Security policy

## Supported version

Security fixes target the latest published package revision. Older revisions
may be useful for diagnosis, but users should upgrade both `nes-emulator` and
`luci-app-nes-emulator` together before reporting a problem.

## Reporting a vulnerability

Use GitHub's **Security → Report a vulnerability** flow when private
vulnerability reporting is enabled for the repository. Include affected
versions, impact, reproduction steps, and a minimal proof of concept.

If that private form is unavailable, open a public issue that asks the
maintainer to establish a private contact channel. Do **not** include the
vulnerability details in that issue.

Never publish or attach:

- `/etc/nes-emulator/auth.token` or a URL containing its value;
- ROMs, BIOS files, SRAM, save states, or router backups;
- private signing keys or credentials;
- unredacted logs containing private addresses or WebSocket query strings.

The browser receives its bootstrap token in the URL fragment, so the initial
`/play` request and its Referer do not contain it. The WebSocket API cannot set
an Authorization header, however, so its upgrade uses `/ws?token=…`. Configure
reverse proxies and access logs to redact query strings. The game client
deliberately ignores a token supplied as `/play?token=…`; only the URL fragment
or its previously established session-storage value can bootstrap the client.

## Deployment boundary

`nesd` provides authentication but does not terminate TLS. Bearer tokens sent
over plain HTTP/WebSocket are not confidential against a network observer.
Keep port `29876` on a trusted LAN. For remote access, use a VPN or a trusted
HTTPS reverse proxy and do not expose the daemon directly to the WAN.

The project is designed for one authenticated media client. It is not a
multi-user Internet streaming service or a security boundary between mutually
untrusted LAN users.
