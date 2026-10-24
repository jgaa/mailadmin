# Administration and recovery

The daemon supports the legacy `organization`, `passwd`, `alias` and `forward`
tables, plus explicit domain ownership and persistent administrative work. DNS
integration remains deferred. MCP exposure was reviewed: these are administrator
REST operations; MCP remains disabled and no MCP tools are added.

## Upgrade boundary

1. Back up MariaDB and the spool/configuration. Stop legacy writers and the old
   daemon. Convert the four legacy tables to InnoDB with the existing migration
   playbook. Do not allow legacy writers to resume after takeover.
2. Build the image on the controller or use GitHub Actions. Load/pull that image
   on the server. Never build on the mailserver.
3. Run `migrate-administration.yml` with `mailadmin_writers_stopped=true` and the
   image reference. It invokes `mailadmin schema migrate --config PATH` with a
   temporary root-only, socket-authenticated database configuration. Schema DDL
   is resumable but is not transactional; keep the backup until verification.
4. Rotate the daemon database password in Ansible Vault before granting write
   privileges. Earlier inventory deployments allowed the host Courier user to
   read that credential. The new configuration is root:mailadmin-config 0640;
   the host Courier user must not belong to that group.
5. Provision with `mailadmin_administration=true`. The role grants only SELECT,
   INSERT, UPDATE and DELETE to the daemon, never DDL or GRANT. It installs the
   root publisher and timer. This opt-in defaults to false for existing servers.
6. Existing verifier content is copied, without replacement, to
   `/etc/mailadmin/credentials/api-key.sha256`, owned root:courier 0640. All
   ancestors are root-owned and exclude other writers; the directory is mounted
   read-only into the container. The same bearer token remains valid. The old
   Courier-owned state verifier is no longer used.
7. Deploy/start the new image. Check `/api/v1/operations` until activation is
   active, then verify SMTP delivery, alias routing, IMAP authentication, quota
   handling, forwards and auto-replies on synthetic accounts before production.

For a new server, create the key using the image CLI as root, with the protected
credential directory mounted writable for that one invocation. Capture stdout in
a root-only file; never use a container log driver for that invocation. Set the
verifier's group to the configured Courier GID and mode to 0640 before serving.
Rotation uses the same CLI procedure followed by restart. Do not put the token
in command arguments, logs, Ansible output or shell history.

The migration validates InnoDB, unique organization identities, domain ownership,
alias/account address collisions and orphan aliases/forwards before adding:

- `domain`: canonical ASCII domain primary key; organization foreign key.
- `admin_account`: desired enabled state, separate from effective Courier login.
- `admin_state`: desired/applied generations and a sanitized activation error.
- `admin_retired_address`: prevent address reuse while old Courier routing remains.
- `admin_cleanup`: immutable deleted account ID, original organization/home,
  required generation, retry count and sanitized error.

The legacy default domain column is widened to 253 characters. Other legacy
column widths/character sets remain in place; account addresses remain limited
to 128 bytes and organization names to 50 bytes. Plan a separate character-set
migration if Unicode data exceeds the existing database's encoding. International
domains must be supplied as ASCII IDNA form; this service does not perform IDNA
conversion. Mailbox local parts use a deliberately restricted ASCII subset.

## Transaction and activation semantics

All administrative mutations use bound SQL parameters and an InnoDB transaction.
Use `innodb_flush_log_at_trx_commit=1` and durable local storage for the journal.
An advisory lock serializes writers, schema/password CLI work and reconciliation
across daemon processes. A mutation increments the desired generation in its
transaction. Its reply includes `activation`; a pending generation means the DB
change is durable, while Courier/filesystem work is still being retried.

Creation starts a mailbox with `active=no`. The worker creates the Maildir and
filter files, publishes a complete domain/alias request, and waits for the root
publisher's digest-matched acknowledgement. Only then does a transaction enable
requested accounts and mark the generation applied. Effective `hosted=false`
also prevents account authentication. Disabling an account sets `active=no` in
the request transaction. Existing authenticated IMAP sessions are not forcibly
terminated. SMTP delivery already queued by Courier follows Courier's retry and
bounce behavior.

Deletion removes the login, aliases and forwarding records immediately, and
journals physical cleanup in that same transaction. Cleanup starts after the
required Courier generation has activated. A crashed or failed cleanup keeps its
row and retries after restart; missing directories/files count as success.
Cleanup releases the administrative writer lock before removing files, so large
mailboxes do not prevent unrelated database mutations. Deleted/renamed primary and alias addresses are reserved until the retiring
generation activates, preventing old compiled alias routes from delivering to a
new account that reuses an address. IDs
and spool locations must never be reused manually while cleanup is pending.
Organization deletion requires no accounts; its empty owned domains are removed
in the same transaction. Domain deletion/ownership transfer requires no accounts,
aliases or default-domain reference. Account rename stays within its organization
and does not move the immutable spool identity.

The worker reissues reconciliation at startup, polls pending work, and supports
an explicit `POST /api/v1/reconcile`. Failed activations remain visible in
`GET /api/v1/operations`; filesystem cleanup failures include an account ID and
retry count. Logs use fixed error codes and identifiers, never SQL, password
values or raw connector errors. Operations are eventually applied; this does not
provide an atomic transaction across all of Courier's compiled databases. A
partial rebuild is retried as a full generation, and is never acknowledged as
successful until all fixed commands exit successfully.

## Restricted publication

The daemon writes only `request.json` under root:mailadmin-config 2770
`/etc/mailadmin/generated`. The host Courier user cannot traverse/write it. The
root-owned `/usr/local/libexec/mailadmin-publish` timer reads bounded regular
files without following symlinks, independently validates every domain/address,
and accepts no paths or commands from its input. It atomically replaces only:

- `/etc/courier/aliases/mailadm_generated`
- `/etc/courier/hosteddomains/mailadm_generated`
- `/etc/courier/esmtpacceptmailfor.dir/mailadm_generated`

It runs `makealiases`, `makehosteddomains` and `makeacceptmailfor`, then
`systemctl try-restart courier.service` to invalidate the queue daemon's routing
cache. All paths, unit names, arguments and environments are fixed, with
per-command timeouts and no shell. IMAP stays independent; Debian's unit
dependencies may briefly restart SMTP listeners during activation. It never modifies
smtpaccess, authentication configuration, certificates or unrelated source files.
`status.json` is root-owned, carries the exact request digest, and is only
successful after all rebuilds. Ordinary Courier processes receive no new config
write permission. Changes to the three old generated sources are intentional
ownership takeover: stop the legacy generator before enabling this helper.

## Delivery and quotas

Each managed mailbox uses `home=<spool>/<organization_id>/<passwd_id>` and
`maildir=<home>/Maildir`. The daemon checks the stored paths, UID/GID and all
traversed directory components; filesystem work uses directory descriptors and
refuses symlink directories. It creates directories 0700 and filter files 0600,
with fsync and atomic replacement. Recursive removal unlinks symlinks without
following them, and caps each cleanup batch. Unexpected legacy spool layouts are
reported for operator repair rather than guessed or deleted.

`filter_enabled`, `spam_enabled`, `spam_folder`, `spam_retention_days` and
`spam_trash` inherit independently from the organization when account fields are
null. When filter control is enabled, `.courier` pipes through spamc and `maildrop ./.mailfilter`; passing the explicit
filter avoids standalone Debian maildrop using the system Courier account home.
`.mailfilter` delivers flagged spam to its folder, invokes mailbot for an
optional auto-reply, then forwards through maildrop's native `!address` syntax.
`keep_copy=true` uses `cc` before local delivery; false uses final `to` forwarding.
Mailbot handles mailing-list/DSN suppression and its sender-history database
limits repeated replies to one per day. Reply text is stored separately from
executable filter instructions. The filter explicitly uses `/bin/sh` for its
fixed external commands; it must not inherit the Courier system account's
`nologin` shell. When filter control is disabled, existing
manually maintained filters are preserved; new mailboxes get direct delivery.
Forwarding and auto-reply changes need filter control enabled to take effect.

Quotas accept an empty string or comma-separated byte (`S`) and message (`C`)
limits. The database remains the authentication quota source. On a changed
quota, the Maildir++ cache is invalidated with the new quota header and a file
larger than 5120 bytes, forcing the delivery/IMAP library to recalculate actual
counts instead of installing false zero totals. These are Courier's voluntary
quotas, with its documented concurrency and delayed-accounting limitations.

Spam cleanup runs on a configured interval (default one hour), examines only
regular Courier-owned files in the spam folder's `new` and `cur`, and uses mtime
for the retention age. `spam_trash=""` explicitly deletes expired spam; a named
trash folder moves mail there. Linux `renameat2(RENAME_NOREPLACE)` gives atomic,
non-overwriting moves. It never purges the trash destination or reads message
contents. Spam and trash folder names must differ. Restart retries are safe.

See the upstream [maildrop filtering manual](https://www.courier-mta.org/maildrop/maildropfilter.html),
[mailbot manual](https://www.courier-mta.org/mailbot.html), and
[Maildir++ quota protocol](https://www.courier-mta.org/imap/README.maildirquota.html).

## Passwords

New/reset passwords are supplied by the administrator or generated using linked
OpenSSL `RAND_bytes` (256 random bits, hex encoded). Generated values appear only
in that mutation's response and cannot be read back. New passwords require at
least 12 bytes, with a 1024-byte maximum. The service stores SHA-512 crypt hashes
with 100000 rounds, OpenSSL-generated salts and an empty legacy `clear` field;
Debian libxcrypt provides Courier-compatible hashing. Do not enable CRAM-style
authentication mechanisms that require retained plaintext.

`mailadmin passwords migrate --config PATH --confirm-hash-compatible` removes
existing plaintext only after an operator confirms hash-only IMAP and SMTP AUTH
work on the target server. It locks and converts all applicable rows in one
InnoDB transaction. Existing hashes are preserved if they match the plaintext;
a mismatch aborts the entire conversion. Do not run this against MyISAM tables.
Rotation remains explicit through `PATCH /api/v1/accounts/{id}`.
