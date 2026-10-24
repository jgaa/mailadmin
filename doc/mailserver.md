# Mail server requirements and deployment

The production reference host is Debian 12 with apt-managed Courier and MariaDB
10.11; the supplied staging VM uses Debian 13. Host provisioning supports both.
Mailadmin runs in a container on that host. The same playbooks can prepare a
disposable staging server and later a replacement production server. The daemon supports administration after the explicit offline schema migration
and `mailadmin_administration=true` opt-in. See
[administration and recovery](administration.md) for the upgrade boundary.

## Host requirements

| Area | Requirement |
| --- | --- |
| Identity | Host `courier:courier`, UID 107/GID 113 on the existing server; reserve the same numeric IDs on a replacement before copying mail |
| Database | Existing `mail` schema and working Courier authmysql query; InnoDB conversion approved, with backup and a maintenance window |
| Mail transport | `courier-mta`, `courier-imap`, `courier-authdaemon`, `courier-authlib-mysql` |
| Filtering | `maildrop` (provides maildrop and mailbot), `spamassassin`, `spamc`; verify the installed default delivery pipeline and existing filters |
| Runtime | Docker, systemd, MariaDB Unix socket, writable persistent daemon state |
| Provisioning | Debian 12/13, SSH with become/root access, Python 3, python3-pymysql, Ansible controller with ansible-core >= 2.16 and `ansible.mariadb` |
| API boundary | Default loopback listener on 8080; SSH tunnel or trusted TLS proxy. Direct non-loopback listeners require configured TLS key/certificate |
| Metrics | Authenticated `/metrics`, exporting account and alias counts with a `domain` label; no usage collectors |

Package names are available on Debian 12/13. The host role installs only this project's
runtime packages; normal OS upgrades, public DNS, certificates, firewall policy,
reverse DNS, relay rules and unrelated Courier administration remain host-owner
responsibilities. Fresh staging is for isolated testing, not automatic public
mail service activation. Do not repoint public MX records at staging.

## Filesystem and privilege boundary

- Preserve mailbox homes from the database. On the existing server these follow
  `/var/lib/courier/spool-virtusers/<organization-id>/<mailbox-id>/Maildir`.
  Keep identical absolute paths inside the container for future spool operations.
- Run the daemon as UID/GID 107:113, never root. Its image does not need host
  passwd entries. Keep mailbox directories at 0700 and filters at 0600.
- Store daemon state at `/var/lib/mailadmin`, owned by Courier and mode 0700.
  Do not store the active API key verifier in this Courier-writable directory.
- Keep `/etc/mailadmin` root-owned, group Courier, mode 0750; the JSON config is
  root:mailadmin-config 0640 because it contains a writable database credential.
  Store the verifier separately in root-owned `/etc/mailadmin/credentials`,
  with root:Courier 0640 permissions and a read-only container mount.
- Create `mailadmin-config` (proposed default GID 2107) for the **container's
  supplementary groups only**. Never add the host Courier account to it.
  Fail rather than renumber existing unrelated accounts/groups.
- `/etc/mailadmin/generated` is root:mailadmin-config 2770, under a root-owned
  parent. The container submits only bounded domain/alias JSON here. A root
  host timer independently validates it, publishes the three fixed managed
  Courier sources and runs the three fixed rebuild commands. No general
  configuration/service-control interface or Docker socket is exposed.
- Container root filesystem is read-only, capabilities dropped, privilege
  escalation disabled. It uses host networking for a loopback API and the mounted
  Unix database socket. The spool mount is read-only in inventory mode.

The observed package binaries and configuration boundary are documented in
[project.md](project.md). Confirm package installation with `command -v maildrop
mailbot`; installation alone does not prove that an old `.mailfilter` works.

## Staging and repeatable deployment

See [the Ansible runbook](../deploy/ansible/README.md) for exact commands.
Playbooks deliberately separate:

Build and test images locally using `deploy/build-image.sh`. Future builds can
move to GitHub Actions. Staging only receives and runs finished images; no source
compilation or image build belongs on that server.

1. `host.yml`: packages, numeric identities, restricted directories, a least-privilege
   database login, publisher and daemon config. In explicit staging mode only, initialize
   synthetic InnoDB tables and select authmysql in Courier.
2. `migrate-innodb.yml`: back up and convert the existing four MyISAM tables,
   retaining columns, values and collation. This requires an explicit indication
   that legacy writers have been stopped. Conversion does not change passwords,
   the authentication query, or organization indexes.
3. `migrate-administration.yml`: explicit domain ownership, desired-state and
   cleanup tables, using the image CLI after backup/offline InnoDB conversion.
4. `deploy.yml`: transfer an archive or pull an image
   and install a systemd container unit.
   First installation leaves the daemon stopped until the CLI-created verifier
   exists and starting the service is explicitly selected.

Staging data is synthetic: two disabled accounts in two domains of one
organization, one alias, plus a second organization with an empty domain for
zero-count checks. It has no working mailbox passwords or real messages. Imports
run only when the `mail` database has no tables. Rerunning provisioning preserves
existing data. A failed partial schema import requires inspection rather than
automatic dropping/recreation of tables. No production data is copied to staging
by these playbooks.

Use inventory/Vault for machine-specific settings and secrets. Commit neither
real inventories nor decrypted Vault values. New database secrets can be generated
with `mailadmin secret create`, which uses the linked OpenSSL library. The role
requires that generated 64-character hexadecimal form for its database secret.

## Replacement server and upgrade procedure

Provision a replacement with the same IDs/paths, restore a verified backup of
the `mail` database, and copy Maildirs including hidden files and metadata while
delivery/writers are quiesced. For example, `rsync -aHAX --numeric-ids` is suitable
for the spool when both hosts/filesystems support those attributes. Keep Courier
configuration, certificates, signing keys and service settings in protected
backups/Vault, and restore them deliberately; do not substitute the staging
authmysql template for the production custom query.

Use the inventory API and metrics to compare organizations, domains, account and
alias counts. Then test actual SMTP delivery, IMAP authentication, forwarding and
auto-replies on controlled accounts before switching traffic. Existing sessions,
queued mail, aliases and filters need their own cutover checks. Restore previous
container image/config and database/spool backups if a cutover fails; package and
database rollback are operator-controlled operations, not an automatic playbook
rescue that might overwrite newer mail.

For an application upgrade, retain state and configuration and deploy a new
image reference or image archive. For package upgrades, explicitly select
`mailadmin_package_state=latest` when running the host playbook in a maintenance
window; default `present` avoids an unsolicited broad package upgrade.

## Verification status

The repository includes security, coroutine, Maildir/password and privileged
publisher validation tests, plus read-only and destructive synthetic staging
HTTP/protocol tests. Run the smoke test only after the container is built, the key is
provisioned, and the service is running. The supplied `staging` VM has been
provisioned with runtime packages and synthetic database fixtures. No production
package/schema/configuration changes have been applied during this work.
