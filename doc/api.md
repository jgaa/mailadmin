# Administrator API

All routes, including health and metrics, require `Authorization: Bearer <key>`.
No end-user or organization-scoped credentials are provided: this key authorizes
the mailserver administrator. Use TLS on non-loopback listeners. Browser origins
are denied by default. Responses use `Cache-Control: no-store` and `X-Request-ID`;
errors contain `error.code`, `message` and `request_id` without secret values.
JSON mutations require `Content-Type: application/json`. Body/header limits are
16/8 KiB. Unknown fields, invalid IDs, unsafe folders and malformed addresses are
rejected. Lists are bounded to this small server's 10000-record scope.

Administration requires the explicit configuration flag and schema migration in
[administration.md](administration.md). Inventory-only mode remains available.

| Resource | Methods | Reply |
| --- | --- | --- |
| `/api/v1/health` | GET | Ready state, version and mode |
| `/api/v1/organizations` | GET, POST | Organization inventory / created organization |
| `/api/v1/organizations/{id}` | GET, PATCH, DELETE | Full policy/details / deletion |
| `/api/v1/domains` | GET, POST | Domain inventory/counts / created domain |
| `/api/v1/domains/{domain}` | GET, PATCH, DELETE | Domain owner / deletion |
| `/api/v1/domains/{domain}/dns` | GET, POST, PUT, DELETE | Durable DNS intent, state and reports |
| `/api/v1/accounts` | GET, POST | Accounts / created account |
| `/api/v1/accounts/{id}` | GET, PATCH, DELETE | Account / deletion |
| `/api/v1/aliases` | GET, POST | Aliases / created alias |
| `/api/v1/aliases/{id}` | GET, PATCH, DELETE | Alias / deletion |
| `/api/v1/operations` | GET | Activation generation and pending cleanup jobs |
| `/api/v1/reconcile` | POST | Queue a fresh complete reconciliation |
| `/metrics` | GET | OpenMetrics domain account and alias gauges |

Creation returns 201. Updates/deletions return 200 after the database transaction
commits; `activation.status="pending"` explicitly means Courier work is not yet
complete. Poll operations for `active` or a sanitized activation error. Validation/conflict errors leave no database changes. On transport or database
failures, inspect current state before retrying: a lost commit acknowledgement
cannot prove that a transaction did not commit. A lost response to a successful
password-generation request cannot be replayed: explicitly rotate the password
again. No automatic retry/idempotency-key contract is provided for HTTP creates.
Resource conflicts return 409, missing resources 404, invalid inputs 400,
unsupported methods 405, invalid/missing authentication 401, and overload or
database/worker problems 503. The HTTP framework handles transport, Origin and
pre-authentication request-filter errors using its own dispatch policy.

Create an organization with `name` and `default_domain`; that domain is created
and owned atomically. Optional fields are `hosted`, `inbound_mailserver`,
`outbound_mailserver`, and the five policy fields below. Creation also supports
`provision_dns`, a complete `dns` configuration, or dedicated `mail_server` input
as described in [DNS provisioning](dns.md). Patch changes only
provided fields. A new default domain must already belong to that organization.
Organization deletion requires all accounts removed; its empty domains are then
removed together. Domain creation uses `domain` and `organization_id`. Patch can
change `organization_id` only for an empty, non-default domain. Domain names are
canonical lower-case ASCII; a trailing dot is normalized away.

Account creation requires `address`; its domain determines the organization.
Optional fields are `name`, `quota`, `active` (desired enabled state), `password`,
`generate_password`, `keep_copy`, `auto_reply`, `forwards` and policy fields.
Patch supports the same fields; address changes must stay within the owning
organization. `forwards` replaces the complete list of up to 32 destination
addresses. `auto_reply=""` disables replies. Quota examples: `10000000S`,
`10000000S,1000C`, or `""` for no limit. Alias create/patch uses `address` and
`account_id`; the alias domain must belong to the target account's organization.
Domain creation supports the same DNS options. Primary and alias addresses share
one namespace enforced by the serialized API.
Alias chains, catch-alls and distribution lists are not part of this API.

A new account without a password generates one by default. Supply `password` or
`generate_password:true`, never both. For existing accounts, omitted password
fields preserve the password. Generated passwords are returned in the top-level
`generated_password` field exactly once; supplied passwords are never returned.
Account reads expose effective `active` and `desired_active` separately and
never expose `crypt` or `clear`. API secrets/passwords must never be logged by a
reverse proxy or API client either.

| Policy field | Type | Meaning |
| --- | --- | --- |
| `filter_enabled` | bool | Manage delivery filters |
| `spam_enabled` | bool | Flagged-spam filing and scheduled cleanup |
| `spam_folder` | string | Spam folder, letters/digits/underscore/hyphen |
| `spam_retention_days` | integer 1..36500 | Retention age based on message mtime |
| `spam_trash` | string | Move expired spam here; empty string means delete |

Account policy values may be null to inherit; organization values may not.
Organization defaults remain filters on, spam off, folder `spam`, 30 days, trash
`Trash`. Account policy reads show null inheritance rather than an effective
value. Forwarding and auto-replies use generated filters; when filter management
is disabled, manual files are preserved. Account deletion immediately removes
login, aliases and forwards, while its durable cleanup is completed in the
background. Disabling does not terminate existing IMAP sessions.

Metrics remain exactly `mailadmin_email_accounts{domain="..."}` and
`mailadmin_aliases{domain="..."}`, counting configured records including disabled
accounts. Every hosted explicit domain is exported, including zero-count domains.
No mailbox sizes, traffic, message counts or usage statistics are exported.

The config file is root:mailadmin-config 0640. The API verifier is root:courier
0640 in a root-owned directory mounted read-only into the container. See the
Ansible template for configuration fields, including `administration`,
`spool_root`, `generated_directory`, `reconcile_seconds` and
`spam_cleanup_seconds`. Non-loopback listeners require TLS, serving as root is
refused, and mutations are disabled unless explicitly enabled.

```text
mailadmin serve --config /etc/mailadmin/config.json
mailadmin key create --key-file /etc/mailadmin/credentials/api-key.sha256
mailadmin key rotate --key-file /etc/mailadmin/credentials/api-key.sha256
mailadmin secret create
mailadmin schema migrate --config /root/migration.json
mailadmin passwords migrate --config /root/migration.json --confirm-hash-compatible
mailadmin --version
```

Use `--help`/`-h` for grouped general and command-specific help, and
`--version`/`-v` for the CMake-generated version. Options may precede or follow the
command. Serve accepts `--config=PATH`/`-c PATH` and
`--log-level info|debug|trace`/`-l LEVEL`. Invalid CLI arguments exit 2 without
echoing values. CLI-generated secrets go only to stdout. Create/rotate verifiers
as root in the protected directory, set group/mode as described in the upgrade
runbook, and restart the service after rotating its API key.

Yahat HTTP handlers bridge to owned C++20 awaitables using the existing
stackful-coroutine executor; blocking MariaDB and filesystem work runs away from
HTTP executors. `as_tuple(yield)` avoids Boost 1.83's plain-yield exception-pointer
completion bug. Runtime credentials, SQL parameters and raw connector exceptions
are never included in application logs.
