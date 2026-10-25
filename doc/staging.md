# Staging service and verification

The disposable `ssh staging` VM runs Debian 13; production remains Debian 12 and
has not been contacted or changed. Containers are built on the controller with
rootless Buildah and loaded onto staging. No image is built on the mailserver.
The GitHub Actions container workflow remains available for future main pushes.

The API service runs as Courier UID/GID 107:113 with container-only supplementary
GID 2107. It listens on `127.0.0.1:8080`, with bearer authentication on every
route including metrics. Administration is enabled. The synthetic legacy tables
use InnoDB; explicit domain ownership and administrative journal tables have been
migrated using the image CLI. The original fixture is two disabled accounts,
one alias, and three domains across two organizations. Tests create additional
synthetic `.test` records and clean them up afterward. No production mail data
is copied here.

The separate Wt UI container runs as UID/GID 65532:65532, with a read-only API-key
mount, persistent UI authentication state, a read-only root filesystem, dropped capabilities and a temporary `/tmp`. Its
`mailadmin-ui.service` is enabled on boot. Open **http://192.168.1.199:8081** and
sign in as `admin` using the UI password in
`/var/lib/mailadmin-ui/initial-password` (0600). The UI uses HTTP to consume
the API at `http://127.0.0.1:8080`; the API remains bound to loopback. Both HTTP
transports are intentional for staging. Installing the UI did not restart or
replace the API container or Courier.

`Dockerfile.ui` builds Wt and the application on the controller and runs the
`ui_data`, `ui_credentials` and `ui_authentication` CTests before producing the runtime image. The locally built image is
exported to `~/.local/state/mailadmin/images/ui-staging.tar` and deployed with
`deploy/ansible/deploy-ui.yml`. Repeat deployment was checked with zero changes.
The separate GitHub Actions UI workflow builds/publishes a `-ui` image using the
existing `MY_PAT` secret when relevant changes reach main; no workflow has been
published or run as part of this local deployment.

## Credential and configuration boundary

- Plaintext test API key: `/root/.config/mailadmin-staging/api-key`, root-only.
  Never paste it into a command argument or task log.
- Active verifier: `/etc/mailadmin/credentials/api-key.sha256`, root:courier
  0640, with protected root-owned ancestors and a read-only container mount.
- Runtime database configuration: `/etc/mailadmin/config.json`,
  root:mailadmin-config 0640. Its database credential was rotated when enabling
  writes; ordinary host Courier cannot read it.
- Courier authentication uses a separate SELECT-only database login and secret.
  The administrative login has DML privileges without DDL/GRANT.
- `/etc/mailadmin/generated` is root:mailadmin-config 2770. A restricted root
  publisher validates requests, rebuilds the managed Courier databases and
  restarts the fixed Courier queue service to invalidate routing caches. Debian
  can briefly restart dependent SMTP listeners; IMAP is independent.
- Controller Vault/password files remain outside the repository under the
  controller's protected mailadmin configuration directory. The pre-upgrade
  synthetic database backup is root-only on staging.

## Executed checks

Local API container CTest passes four groups: key-file security, actual Asio
suspension/exception propagation, Maildir/password regressions, and DNS
defaults/public-key boundaries. The Maildir
checks cover repeated provisioning, symlink containment, idempotent removal,
atomic spam movement, explicit spam deletion, quota-cache invalidation and
salted Courier-compatible password hashes. Seven Python tests validate the
privileged publisher's untrusted input boundary and successful/failed rebuild
acknowledgements. Ansible syntax checks and the
container workflow's actionlint check pass.

The UI's container CTest passes its API-contract/ownership/inheritance checks.
`tests/staging_ui.py` passed in Chromium with JavaScript enabled and disabled,
using the real staging API: authenticated tree loading (including empty domains),
mailbox selection and configuration, alias isolation, inherited policies,
mailbox/tree refresh, sign-out and rejected-password behavior. API keys are absent
from rendered authenticated pages and from both services' journals. The API's
ordinary read-only smoke test also passed after the UI deployment. See the
[UI guide](ui.md) for repeatable build, deployment and browser-test commands.

The UI now provides explicit Save/Cancel CRUD for organizations, domains,
mailboxes, forwarding and aliases, plus password actions and policy overrides.
`tests/staging_ui_crud.py` passed with JavaScript enabled and disabled, including
cancel without a mutation, ownership changes, generated passwords, mailbox
properties, forwarding list changes, alias rename/retargeting and deletion.
Dedicated host DNS provisioning and a complete DMARC policy replacement passed
against the authorized nsblast mail-test instance.

DNS uses `http://127.0.0.1:18080/api/v1`, a key scoped to `mailtest-cycle.test`,
and configurable `default_mail_hostname` (`mail.example.test` for the existing
synthetic fixture). A protected database backup preceded the additive `admin_dns`
schema migration. API and UI containers were built locally and deployed; nsblast
was not reconfigured. The credential is root:mailadmin-config 0640 and was checked
to be unreadable by host Courier. Existing API/DB/DNS secrets were absent from
both administration services' journals.

`tests/staging_dns.py` passed using an isolated database/API container and a mock
provider, covering invalid-input atomicity, pending polling, unknown POST outcomes,
restart/retry, terminal errors, superseded transitions, bounded polling and
deletion ordering. Temporary test DB/users/containers are removed. Live browser
tests remove their mailboxes/domains; dedicated host address records are retained
by the intentional preserve-IP policy. No production DNS or mail data was used.
Ownership regression checks also passed: a failed create conflict does not claim
or automatically delete pre-existing DNS, unknown creates block destructive
implicit cleanup, conflicting create-only changes preserve pending tracking,
invalid deletion acknowledgements retain ownership, and deleted-domain DNS can
be explicitly repaired through Operations. Backfill runs on every migration
invocation so it can recover after an interrupted column addition.

`tests/staging_business.py --mail-protocols` has passed against the running VM:

- Organization/domain/account/alias CRUD, address collisions and ownership rules.
- Generated and supplied passwords, hash-only TLS IMAP login, immediate disabling,
  and all-or-nothing plaintext conversion (including a deliberate hash conflict).
- Real SMTP delivery through an alias into its Maildir.
- Real forwarding with/without a local copy and an auto-reply delivered to a
  synthetic local sender. Local sendmail injection avoids requiring public DNS
  for `.test` senders; SMTP delivery checks use a null envelope sender.
- Policy inheritance and scheduled movement of expired spam into Trash.
- Pending activation surviving daemon restart.
- Immediate database deletion and protection against premature address reuse.
- Deliberately unsafe directory permissions causing journaled cleanup failure,
  followed by daemon restart, permission repair and successful physical cleanup.
- API/database/generated/supplied credentials absent from the relevant journal.

The ordinary host Courier user was also checked directly: it cannot read the
administrative DB configuration or rewrite the verifier, generated directory or
managed Courier sources. Provisioning and deployment are checked for repeatability.
SMTP AUTH is not enabled by staging's package defaults; production plaintext
conversion still requires checking its configured SMTP AUTH mechanisms before
using the compatibility confirmation flag. The read-only smoke test remains available for authentication, errors, inventory
and exact domain-label/count assertions.

Two Debian integration details are implemented deliberately: generated filters
invoke `maildrop ./.mailfilter` so standalone maildrop uses the virtual mailbox
working directory, and set `SHELL='/bin/sh'` so fixed external commands do not
inherit Courier's `nologin` shell. Staging authmysql includes the package-required
socket/port/options defaults and end marker.

## Operations

```bash
./deploy/build-image.sh
ansible-playbook -i deploy/ansible/inventory.staging.yml deploy/ansible/deploy.yml
ansible-playbook -i deploy/ansible/inventory.staging.yml deploy/ansible/verify.yml
```

Use Vault arguments when provisioning `host.yml`. New servers need the offline
schema migration and root-created verifier before enabling the daemon; follow
[administration and recovery](administration.md) and the
[Ansible runbook](../deploy/ansible/README.md). The checked-in staging inventory
selects administration and a five-second spam-cleanup interval for debugging;
the production default is one hour. Public DNS, MX records and production
credentials remain unchanged. No staging reset is required.
