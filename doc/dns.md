# Mail DNS provisioning

The API daemon uses RESTinCurl and an Asio coroutine worker to consume nsblast.
Administrative requests commit desired DNS intent into `admin_dns`; Courier and
DNS activation have separate status. Existing domains are not automatically
enrolled or overwritten during migration or startup. No MCP tools were added.

## Configuration and credentials

| Protected API configuration field | Meaning |
| --- | --- |
| `nsblast_api_url` | API root ending in `/api/v1`; empty disables provisioning |
| `nsblast_api_key_file` | Absolute path to a protected bearer-key file |
| `default_mail_hostname` | Shared MX hostname for new domains |
| `dns_default_configuration` | Complete default configuration, or policy overrides combined with the shared hostname |
| `nsblast_mta_sts_https_ready` | Operator confirmation of trusted HTTPS policy delivery; false by default |

Use a tenant-owned key with `USE_API`, `CREATE_MAIL`, `UPDATE_MAIL`, `DELETE_MAIL`
and scope covering the managed domains and affected names. Zones must already
exist on a primary or standalone server. Missing zones and authorization errors
never trigger zone creation or fallback to broader credentials.

Deployment writes the key root:mailadmin-config 0640 into the read-only credentials
mount. Ordinary host Courier cannot read it. Reads and logs never expose it. The
worker reads the credential at startup; retain its authority through pending
transitions and restart when rotating it. HTTPS validates certificates/hostnames;
explicit numeric loopback HTTP is supported for staging. Redirects, environment
proxies and transport tracing are disabled. Timeouts are three seconds to connect
and ten seconds total. Responses are limited to 64 KiB; desired configuration is
limited to 15 KiB within the existing 16 KiB administrative API body limit.

## Shared versus dedicated hosting

With a configured shared hostname, new domains and organization default domains
queue create-only DNS provisioning. Address fields are omitted to preserve the
shared host's A/AAAA records. The hostname must already resolve to usable
addresses rather than a CNAME, per [RFC 5321 §5](https://www.rfc-editor.org/rfc/rfc5321.html#section-5).
`provision_dns:false` explicitly skips DNS. Without a shared hostname/default
configuration, domain creation leaves DNS unmanaged.

For a dedicated host, supply `mail_server` on creation with `hostname`, `priority`
and explicit `ipv4` and/or `ipv6` arrays. The service combines it with policy
defaults. A complete `dns` object can alternatively override the creation
configuration. These options are mutually exclusive. Addresses, reporting
mailboxes and DKIM selectors are never inferred or invented.

## Defaults and RFC rationale

| Property | Default | Rationale |
| --- | --- | --- |
| MX preference | 10 | Conventional RFC example; smaller values are preferred |
| TTL | 3600 seconds | Operational one-hour choice, not a universal RFC requirement |
| SPF | `v=spf1 mx -all` | [RFC 7208 Appendix A.1](https://www.rfc-editor.org/rfc/rfc7208.html#appendix-A.1) example for domains whose MX hosts send their mail |
| DMARC | Enabled, `p=none` | Initial non-enforcing example in [RFC 7489 B.2.1](https://www.rfc-editor.org/rfc/rfc7489.html#appendix-B.2.1) |
| DKIM | Empty selector list | Obtain public keys from the actual signer |
| Reporting addresses | Unset | Require real recipients/endpoints |
| MTA-STS | Disabled | Requires trusted HTTPS policy delivery and SMTP TLS readiness; [RFC 8461](https://www.rfc-editor.org/rfc/rfc8461.html) |
| TLS reporting | Disabled | Requires an explicit recipient under [RFC 8460](https://www.rfc-editor.org/rfc/rfc8460.html) |

Override SPF if other services send outbound mail. RFC 7208 discusses its DNS
lookup costs and alternatives in [§10.1](https://www.rfc-editor.org/rfc/rfc7208.html#section-10.1).
DMARC without `rua` does not collect reports. Move to quarantine/reject after
validating sender alignment. DKIM input is checked locally with linked OpenSSL:
canonical Base64 DER RSAPublicKey, 2048–8192 bits. Private DER, PEM, SPKI wrappers
and trailing data are rejected before storage. Signing/private keys, certificates,
PTR and DNSSEC are outside this integration.
The key encoding follows nsblast's current contract. RFC 6376 has a known
[format/example inconsistency](https://www.rfc-editor.org/errata/rfc6376) involving
RSAPublicKey versus SubjectPublicKeyInfo; verify compatibility with the chosen
signer/verifiers before enabling DKIM. No selectors are published by default.

## Administrative API

`GET /api/v1/domains/{domain}/dns` returns availability, desired configuration,
revision, state, report, error, transition, attempts, retry time and earliest
completion. It also serves deleted-domain tombstones. Operations lists DNS jobs
alongside Courier and filesystem cleanup.

POST on the same path is create-only; existing records conflict. PUT replaces
the complete mail configuration: retain all required DKIM selectors and explicit
policies. Omitting sections can remove selectors or reset defaults. The UI starts
from stored desired state and explicitly distinguishes creation from replacement.
There is no provider export endpoint for unmanaged configurations.

DELETE removes recognized mail DNS with `preserve_ip=true`, retaining shared and
dedicated host addresses, zones and unrelated records. Deleting a managed local
domain/organization queues this removal in the same transaction. Local removal
may precede external retirement; inspect Operations. Domain reuse is blocked
until pending deletion completes. When migrating outside this DNS server, publish
replacement MX records only after DELETE completes.

Ownership is established only after nsblast acknowledges provisioning or a
pending transition. A create conflict, authorization failure or rejected input
does not claim pre-existing DNS: local deletion cancels that intent and leaves
the remote records alone. An unacknowledged/ambiguous operation blocks automatic
destructive cleanup until the administrator explicitly reconciles or removes its
mail DNS. Operations retains tombstone details and permits explicit DNS repair
for tracked domains even after their local organization/domain is gone.

## Recovery

The additive `admin_dns` table is installed by explicit `schema migrate`; startup
never runs DDL. It retains intent, revisions, reports and deadlines after local
domain deletion. Per-domain advisory locks serialize each bounded exchange with
intent updates. Concurrent API changes return `dns_busy` promptly; network work
does not hold the global administrative lock. Result writes match the revision.

HTTP 200 can mean pending/running/failed. The worker inspects the envelope/state,
polls GET status and preserves warnings and deadlines. A different transition
becomes `superseded`. It does not resubmit mutations as polling or shorten an
MTA-STS retirement deadline. Provider failure details are retained for the UI
without logging raw responses or credentials.

Identical current PUT/DELETE intent retries transport/server failures up to six
attempts with bounded exponential backoff. Ambiguous POST, including restart
after its durable dispatch marker, becomes `unknown` without automatic replay
or conversion to PUT. Identical failed intent does not reset retry budgets.
Successful polls reset consecutive transport failures; twelve failed polling
exchanges stop as `poll_timeout`. Polling stops a day after the initial safe
deadline, or a day after acceptance without a future deadline. The provider can
still be pending; polling timeout is not a claim that its operation failed.

## Staging and verification

After a protected backup and stopping administrative writers, migrate the
schema and configure DNS through host provisioning or
`deploy/ansible/configure-dns.yml`. The latter preserves existing database/HTTP
settings. Supply `vault_mailadmin_nsblast_api_key` or an explicit root-only host
key source. Production defaults have no provider URL or shared hostname.

Staging uses `http://127.0.0.1:18080/api/v1`, the key scoped to
`mailtest-cycle.test` and the existing synthetic `mail.example.test` shared host.
This is a test configuration, not public mail delivery. Live checks use unique
children of the authorized `.test` zone. nsblast, production DNS and the primary
mail data are not reconfigured by the integration.

`tests/staging_dns.py` creates an isolated DB/API container and mock provider to
check replacement validation, pending polling, restarts/retries, ambiguous POST,
terminal conflicts/failures, supersession, deadlines and deletion ordering.
`tests/dns.cpp` checks defaults and public-key boundaries.
`tests/staging_ui_crud.py` exercises both browser modes and the real staging
provider, including shared/dedicated provisioning and complete replacement.
