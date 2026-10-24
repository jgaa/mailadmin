# Staging service and verification

The disposable `ssh staging` VM runs Debian 13; production remains Debian 12 and
has not been contacted or changed. Containers are built on the controller with
rootless Buildah and loaded onto staging. No image is built on the mailserver.
The GitHub Actions container workflow remains available for future main pushes.

The service runs as Courier UID/GID 107:113 with container-only supplementary
GID 2107. It listens on `127.0.0.1:8080`, with bearer authentication on every
route including metrics. Administration is enabled. The synthetic legacy tables
use InnoDB; explicit domain ownership and administrative journal tables have been
migrated using the image CLI. The original fixture is two disabled accounts,
one alias, and three domains across two organizations. Tests create additional
synthetic `.test` records and clean them up afterward. No production mail data
is copied here.

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

Local container CTest passes all three groups: key-file security, actual Asio
suspension/exception propagation, and Maildir/password regressions. The Maildir
checks cover repeated provisioning, symlink containment, idempotent removal,
atomic spam movement, explicit spam deletion, quota-cache invalidation and
salted Courier-compatible password hashes. Seven Python tests validate the
privileged publisher's untrusted input boundary and successful/failed rebuild
acknowledgements. Ansible syntax checks and the
container workflow's actionlint check pass.

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
