# Install Mailadmin on the existing production mailserver

This procedure takes over the existing Debian 12/13 Courier server and its
`mail` database. It preserves mailbox identities, paths and Courier's custom
authmysql query. Use `mailadmin_mode: existing` throughout. Staging initialization
must remain disabled. All commands below are instructions for the operator;
writing this guide does not deploy anything to production.

Run Ansible and build images on the controller, from the repository root.
Commands marked **mailserver** run in a root Bash shell on the production host.
The inventory alias is `production`; replace example hostnames and paths before
running commands. Read [administration and recovery](../../doc/administration.md)
for activation, filter, password and cleanup semantics.

## 1. Check the existing server and prepare a maintenance window

On the **mailserver**, confirm the recorded identities and paths:

```bash
getent passwd courier
getent group courier
getent group 2107
command -v maildrop mailbot makealiases makehosteddomains makeacceptmailfor
namei -l /var/lib/courier/spool-virtusers
mariadb --database=mail -e \
  "SELECT TABLE_NAME,ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA='mail';
   SELECT uid,gid,COUNT(*) AS accounts FROM passwd GROUP BY uid,gid;
   SELECT @@innodb_flush_log_at_trx_commit;"
```

The reference production server uses Courier UID **107**, GID **113** and
`/var/lib/courier/spool-virtusers/<organization_id>/<passwd_id>/Maildir`.
Verify actual mailbox `home`/`maildir` values and their numeric ownership too.
Use the existing IDs in inventory; do not renumber Courier or recursively change
mailbox ownership. GID 2107 must be free or already belong to `mailadmin-config`.
The host Courier account must not be a member of that administration group.
The current container units mount MariaDB's socket directory at `/run/mysqld`;
the normal socket is `/run/mysqld/mysqld.sock`.

Check Courier's `MYSQL_USERNAME` setting privately and choose a different,
dedicated `mailadmin_database_user` (the example uses `mailadmin_api`). Confirm
that this name is not an existing login used by another service: `host.yml` sets
the supplied password and privileges on the selected login.

Identify and stop the legacy administration application, generators, cron jobs,
imports and password-changing interfaces before migration. They must remain off
after takeover. Establish how to quiesce delivery/IMAP writes for consistent
database and mailbox backups. Do not assume that stopping the old web application
stops every database writer. A hypervisor/storage snapshot is another option if
it captures the database, mail and configuration consistently.

Review these takeover effects before enabling administration:

- The migration derives domain ownership from organization defaults, mailbox
  addresses and aliases. Resolve duplicate organization IDs, conflicting domain
  owners, orphan records and alias/account address collisions first. Compare
  existing hosted-domain files with the database: an extra empty domain present
  only in Courier configuration is not automatically imported.
- The publisher replaces only the three `mailadm_generated` sources under
  `/etc/courier/aliases`, `/etc/courier/hosteddomains` and
  `/etc/courier/esmtpacceptmailfor.dir`, rebuilds their databases and restarts
  Courier's routing service. SMTP listeners may briefly restart through unit
  dependencies. Other source files remain in use.
- Reconciliation starts automatically in administration mode. Existing accounts
  with effective `filterctl=yes` receive managed delivery filters. Review their
  existing `.courier`/`.mailfilter` files, forwards, auto-replies and inherited
  organization policies. With filter control disabled, manually maintained
  filters are preserved.
- Spam cleanup can run immediately on startup. Review `spammgt`, `spamfld`,
  `spamdays` and `spamtrash`, including account overrides. An empty effective
  trash folder means permanent deletion of expired spam. The cleanup interval
  does not provide an initial grace period.

Require durable MariaDB commits (`innodb_flush_log_at_trx_commit=1`) and enough
space for backups and InnoDB conversion. Host provisioning installs missing
maildrop/mailbot, Docker and other runtime packages and enables MariaDB/Docker.
Debian package installation can affect running services; schedule provisioning
as well as schema conversion appropriately. The playbooks do not configure a
firewall, relay policy, public certificates or MX cutover.

## 2. Back up before provisioning, and again before the write-mode cutover

On the **mailserver**, with writers quiesced, take a protected SQL and Courier
configuration backup:

```bash
(
set -euo pipefail
umask 077
mailadmin_backup_dir=$(mktemp -d /var/backups/mailadmin-production.XXXXXXXX)
mariadb-dump --protocol=socket --socket=/run/mysqld/mysqld.sock \
  --lock-tables --routines --events --triggers --databases mail \
  --result-file="$mailadmin_backup_dir/mail.sql"
test -s "$mailadmin_backup_dir/mail.sql"
tar --acls --xattrs --numeric-owner -cpf "$mailadmin_backup_dir/courier.tar" \
  -C / etc/courier
printf 'Protected backup directory: %s\n' "$mailadmin_backup_dir"
)
```

Also back up the complete spool, including hidden files, numeric owners, ACLs and
extended attributes, while mailbox writes are quiesced. Use your verified backup
system or a consistent snapshot; retain a protected copy off the server. Include
legacy admin configuration, service/cron definitions, certificates and any
external delivery/signing configuration needed to restore operation. If Mailadmin
is already installed, include `/etc/mailadmin`, `/var/lib/mailadmin`,
`/etc/mailadmin-ui`, `/var/lib/mailadmin-ui` and its systemd units.

Record the old image/configuration versions and restore procedure. Test restoration
on an isolated server with outbound mail disabled before the production cutover.
SQL dumps and configuration archives contain secrets and must remain private.
Do not copy production data onto the ordinary staging fixture.

## 3. Prepare images and a private production inventory

On the **controller**, install ansible-core >= 2.16 and the collection:

```bash
ansible-galaxy collection install -r deploy/ansible/requirements.yml
```

Build/test and export the API image locally (Buildah or local Docker):

```bash
./deploy/build-image.sh mailadmin:production \
  "$HOME/.local/state/mailadmin/images/production.tar"
```

Alternatively use a tested GitHub Actions image pinned to a commit tag or digest.
For registry deployment, set `mailadmin_image_archive: ''` and set
`mailadmin_image` to that exact published reference. Configure a pull-only registry
credential on the mailserver if needed; do not install the CI publishing PAT there.
Never build images on the mailserver.

Create `/absolute/private/production.yml` **outside the repository**:

```yaml
mailservers:
  hosts:
    production:
      ansible_host: mail.example.net
      ansible_user: root
      ansible_python_interpreter: /usr/bin/python3
      mailadmin_mode: existing
      mailadmin_initialize_staging: false
      mailadmin_package_state: present
      mailadmin_uid: 107
      mailadmin_gid: 113
      mailadmin_config_gid: 2107
      mailadmin_database_user: mailadmin_api
      mailadmin_database_socket: /run/mysqld/mysqld.sock
      mailadmin_spool: /var/lib/courier/spool-virtusers
      mailadmin_state: /var/lib/mailadmin
      mailadmin_endpoint: 127.0.0.1
      mailadmin_port: '8080'
      mailadmin_image: mailadmin:production
      mailadmin_image_archive: /absolute/controller/path/production.tar
      mailadmin_start_service: false
      mailadmin_administration: false
      mailadmin_spam_cleanup_seconds: 3600
      mailadmin_nsblast_api_url: ''
      mailadmin_default_mail_hostname: mail.example.net
```

Use the archive's actual controller path and the existing shared mail hostname.
For a non-root SSH account, configure sudo/become credentials separately (for
example `--ask-become-pass`). The playbooks use `become: true`.

Generate an independent database credential using the image's linked OpenSSL
library. With Buildah on the **controller**:

```bash
mailadmin_cli=$(buildah from mailadmin:production)
buildah run --network none "$mailadmin_cli" -- /usr/local/bin/mailadmin secret create
buildah rm "$mailadmin_cli"
ansible-vault create /absolute/private/production-vault.yml
```

Store that 64-character hexadecimal value as
`vault_mailadmin_database_password: '<generated-value>'` in the encrypted file.
The Docker equivalent is `docker run --rm --log-driver=none mailadmin:production
secret create`. Keep the value out of shell arguments, source control and logs.
This creates a separate daemon database login; do not change or reuse Courier's
production database credential. No staging auth credential is required.

## 4. Provision and verify read-only inventory

On the **controller**, check the target list, provision, and install the image/unit:

```bash
ansible-playbook -i /absolute/private/production.yml deploy/ansible/host.yml \
  --list-hosts --limit production
ansible-playbook -i /absolute/private/production.yml deploy/ansible/host.yml \
  -e @/absolute/private/production-vault.yml --ask-vault-pass --limit production
ansible-playbook -i /absolute/private/production.yml deploy/ansible/deploy.yml \
  --limit production
```

Existing mode preserves the production authmysql query and does not initialize
the mail database. The daemon receives SELECT only, the spool mount is read-only,
and the publisher timer is disabled. First installation does not start the API.
`mailadmin_start_service: false` prevents automatic starts/restarts; it does
**not** stop an already running installation. Stop it explicitly when required.

On the **mailserver**, create the API verifier and capture its one-time plaintext
token in a root-only file. This example refuses to overwrite the plaintext file:

```bash
(
  set -euo pipefail
  install -d -m 0700 /root/.config/mailadmin
  umask 077
  set -o noclobber
  docker run --rm --log-driver=none --read-only --cap-drop=ALL \
    --security-opt=no-new-privileges --user 0:0 \
    --mount type=bind,src=/etc/mailadmin/credentials,dst=/etc/mailadmin/credentials \
    mailadmin:production key create \
      --key-file /etc/mailadmin/credentials/api-key.sha256 \
      > /root/.config/mailadmin/api-key
  test -s /root/.config/mailadmin/api-key
  chown root:113 /etc/mailadmin/credentials/api-key.sha256
  chmod 0640 /etc/mailadmin/credentials/api-key.sha256
)
```

Use the configured GID if different from 113. Preserve the plaintext token in a
password manager/protected backup; the `.sha256` verifier cannot recover it.
For an existing installation, retain the working verifier/token rather than
running `key create`. Investigate any failed creation before proceeding.

Change only `mailadmin_start_service` to `true` in the private inventory and rerun
`deploy.yml`. On the **controller**, open a tunnel:

```bash
ssh -N -L 28080:127.0.0.1:8080 root@mail.example.net
```

In another controller terminal, obtain a private local copy of the plaintext
token and run the read-only test **without** its staging-fixture option:

```bash
(umask 077; scp root@mail.example.net:/root/.config/mailadmin/api-key /absolute/private/production-api-key)
python3 tests/staging_smoke.py --url http://127.0.0.1:28080 \
  --token-file /absolute/private/production-api-key
```

Despite its historical filename/output, this test performs no mutations. Compare
organization, domain, account and alias counts with the legacy database. Check
`systemctl status mailadmin` and `journalctl -u mailadmin` on the mailserver.
Keep the API on loopback; use the tunnel or a separately configured trusted TLS
proxy for remote access. Direct non-loopback API binding requires TLS configuration
that the supplied host template does not provide.

## 5. Migrate and enable administration

Enter the cutover window, quiesce all writers, and repeat the consistent backups
from step 2. Leave legacy administration/generators disabled permanently after
takeover. On the **mailserver**, stop any running Mailadmin processes:

```bash
systemctl stop mailadmin
systemctl stop mailadmin-publish.timer mailadmin-publish.service
```

Set `mailadmin_start_service: false` again in the private inventory. On the
**controller**, convert the tables and apply the administration schema:

```bash
ansible-playbook -i /absolute/private/production.yml deploy/ansible/migrate-innodb.yml \
  -e mailadmin_writers_stopped=true --limit production
ansible-playbook -i /absolute/private/production.yml deploy/ansible/migrate-administration.yml \
  -e mailadmin_writers_stopped=true --limit production
```

The assertion is your confirmation of an actual offline boundary, not a request
for Ansible to stop unknown writers. The first playbook makes an additional locked
SQL backup when MyISAM conversion is needed; it does not back up an already-InnoDB
database. The second adds explicit domains, desired account state, activation,
cleanup and DNS work tables using temporary socket-authenticated database-root
access. It requires the image to have been loaded in step 4. DDL is resumable,
not transactional. On failure, inspect/resolve the reported conflict and rerun
while writers remain stopped; do not drop tables or resume the legacy writer.
Neither migration converts mailbox passwords.

If upgrading a former inventory installation whose database credential was
readable by host Courier, generate a fresh secret and update the Vault value
before granting write access. Then set `mailadmin_administration: true`, leaving
`mailadmin_start_service: false`, and run:

```bash
ansible-playbook -i /absolute/private/production.yml deploy/ansible/host.yml \
  -e @/absolute/private/production-vault.yml --ask-vault-pass --limit production
```

This grants SELECT/INSERT/UPDATE/DELETE without DDL/GRANT, protects the daemon
configuration with the container-only group, and enables the restricted root
publisher timer. Host Courier receives no configuration write permission.

Finally set `mailadmin_start_service: true` and rerun `deploy.yml`. This installs
the write-enabled spool mount and starts reconciliation. Keep those two flags true
in the inventory for subsequent normal deployments.

Before ending maintenance:

- Repeat the read-only smoke check. Inspect authenticated `GET /api/v1/operations`
  until `activation.status` is `active` and cleanup/activation errors are resolved.
  Check `journalctl -u mailadmin -u mailadmin-publish.service` and compare the three
  published sources with the intended domains/aliases.
- Verify existing account login over TLS IMAP and SMTP AUTH, local/inbound delivery,
  aliases, forwarding, keep-copy, auto-replies and quotas using controlled mailboxes.
  Readiness alone does not prove mail delivery works.
- Confirm no legacy job will overwrite routing sources or bypass desired-state
  tables. Resume delivery/IMAP activity when the checks and backup boundary permit.

For example, inspect activation on the **mailserver** without putting the token
in command arguments:

```bash
python3 - <<'PY'
import json
from pathlib import Path
from urllib.request import Request, urlopen
token = Path('/root/.config/mailadmin/api-key').read_text().strip()
request = Request('http://127.0.0.1:8080/api/v1/operations',
                  headers={'Authorization': 'Bearer ' + token})
with urlopen(request, timeout=20) as response:
    print(json.dumps(json.load(response), indent=2))
PY
```

Do not run `tests/staging_business.py`, `tests/staging_dns.py` or the staging UI
CRUD tests on production. Plaintext-password conversion is a separate operator
action after hash-only IMAP **and SMTP AUTH** compatibility has been verified;
see [password migration](../../doc/administration.md#passwords). Preserve existing
plaintext until then, or rotate accounts deliberately through the API.

## 6. Install the optional administrator UI

Build/export the UI on the **controller**:

```bash
./deploy/build-image.sh mailadmin-ui:production \
  "$HOME/.local/state/mailadmin/images/ui-production.tar" Dockerfile.ui
```

Add these settings to the same private production host entry:

```yaml
      mailadmin_ui_image: mailadmin-ui:production
      mailadmin_ui_image_archive: /absolute/controller/path/ui-production.tar
      mailadmin_ui_api_url: http://127.0.0.1:8080
      mailadmin_ui_api_key_source: /root/.config/mailadmin/api-key
      mailadmin_ui_address: 127.0.0.1
      mailadmin_ui_port: 8081
```

The key source is a file on the **mailserver**, not the controller. The playbook
copies it to a protected, read-only container mount. Alternatively supply
`vault_mailadmin_ui_api_key` and pass the encrypted vars file to `deploy-ui.yml`.
For a registry UI image use an immutable reference and an empty archive setting.

```bash
ansible-playbook -i /absolute/private/production.yml deploy/ansible/deploy-ui.yml \
  --limit production
ssh -N -L 28081:127.0.0.1:8081 root@mail.example.net
```

Open `http://127.0.0.1:28081`. Sign in as `admin`; read its generated password
privately on the mailserver from `/var/lib/mailadmin-ui/initial-password` (0600).
The INFO log reports this path only. The UI has its own salted password hash,
separate from the backend API key. Its state persists in `/var/lib/mailadmin-ui`.

To choose/rotate the UI password, prepare a mode-0600 file owned by UID 65532 at
`/var/lib/mailadmin-ui/new-password`, then on the **mailserver** run:

```bash
docker exec mailadmin-ui /usr/local/bin/mailadmin_ui password set \
  --password-file /var/lib/mailadmin-ui/new-password
rm /var/lib/mailadmin-ui/new-password
```

`--password VALUE` is also supported, but file input keeps the secret out of
process arguments/history. Rotation invalidates UI sessions on their next API
action. Remove the initial plaintext file after recording/rotating it; the hash
is sufficient for subsequent startups. See [UI authentication](../../doc/ui.md).

## 7. Enable production DNS separately

Initial installation above leaves DNS provisioning disabled. To enable it later,
configure the real nsblast URL, a production mail-provisioning credential scoped
to the intended zones, and the actual shared mail hostname. Do not copy staging
URLs, credentials, test-zone names or synthetic addresses.

Set `mailadmin_nsblast_api_url`, `mailadmin_default_mail_hostname`, and either
`mailadmin_nsblast_api_key_source` (a protected file on the mailserver) or
`vault_mailadmin_nsblast_api_key`. Review optional
`mailadmin_dns_default_configuration`. Apply `configure-dns.yml` with the same
production inventory/limit, adding the encrypted vars file when using Vault.
It restarts the API. Keep these values in the inventory/Vault so a later
`host.yml` run preserves them. Enabling the provider does not automatically queue
provisioning for every imported domain; request existing-domain provisioning
explicitly after reviewing ownership and records. See [DNS provisioning](../../doc/dns.md).

## Upgrades and recovery

For an application-only upgrade, back up persistent state, choose a tested image
tag/digest or matching archive, and rerun `deploy.yml` (and `deploy-ui.yml` if
needed). Only changed images/units restart the enabled service. Apply any required
schema migration explicitly in a new offline window. Host package upgrades require
an intentional `mailadmin_package_state=latest` run of `host.yml`; the default is
`present`. Package changes and schema changes have their own rollback boundary.

For a failed cutover, stop the UI/API and both publisher timer/service before
restoration, then quiesce mail/database writers. Restore a consistent set of the
pre-cutover database, spool and Courier sources/compiled configuration, along
with the old application/configuration and its service/cron settings. Verify
ownership and rebuild/restart the required Courier components before resuming
traffic and the chosen administrator. Merely reverting the container image or
setting `mailadmin_administration: false` does not undo schema, filters, routing,
spam cleanup or account deletion.

Once newer mail/account changes have been accepted, do not overwrite them with
an old backup without an explicit recovery/replay plan. Keep durable cleanup/DNS
state and credential files through normal restarts/upgrades; deleting state is
not a recovery procedure.
