# Mailadmin backend specification

Status: backend administration implementation and staging validation, 2026-10-24.
See sections 7–8 and the API/recovery runbooks for implementation boundaries.

## 1. Purpose and requirement status

Rewrite the old Courier Mail Server User Administration project as a backend
service that manages a running Courier installation through an HTTP API.
The target is an existing server originally administered by the legacy project.
The rewrite takes over its current mail service and data; it does not recreate
the old project's tools for migrating other mail servers to Courier.

This document distinguishes:

- **Confirmed**: direction supplied by the project owner or this repository's
  `Agents.md`.
- **Observed**: facts extracted from the old source. These describe the old
  system, including its limitations; they do not automatically mandate parity.
- **Proposed**: requirements recommended for the rewrite, pending discussion.
- **Open**: choices requiring agreement before implementation.

### Confirmed direction

- Work on the backend only. A new web interface is outside the current scope.
- Serve the mail server administrator. End-user/mailbox-owner access and
  self-service APIs are outside scope. The administrator may still manage
  mailbox delivery settings such as forwarding and autoresponses.
- Take over the existing legacy-managed Courier installation while preserving
  its mail, accounts and working delivery configuration. Obsolete features for
  migration to Courier, such as alternate old POP login management and Outlook setup
  exports, do not need feature parity. Existing compatibility fields may be left
  in place where necessary for a safe takeover; this does not require new APIs.
- Keep the design simple and proportionate to a small project. Prefer one service
  and straightforward operations over additional infrastructure or frameworks.
- Keep organizations. An organization can own multiple domains; each domain has
  exactly one owning organization. Include administrator-managed auto-replies and
  spam cleanup.
- The existing server runs Debian 12 with Courier and MariaDB installed via apt.
- Conversion of the four legacy tables from MyISAM to InnoDB is approved for
  takeover. Preserve existing data, IDs, paths and collation; prepare the concrete
  migration and backup/verification steps before applying it.
- Authenticate API requests with a bearer token/API key. Provide a simple CLI
  mode in the same service/container to create a key. Log failures and return
  useful, sanitized error information in failing API responses.
- Domain metrics have a `domain` label and count email accounts and aliases only.
  No storage, traffic or other usage statistics are required.
- Use yahat-cpp as the HTTP server; `../yahat-cpp` is available for research.
- Provide metrics for each hosted mail domain.
- Use asynchronous C++ with Asio C++20 coroutines.
- Generate every new password and secret using OpenSSL linked as a library or
  libsodium. Select one; do not add both without a concrete need. Do not use
  general-purpose PRNGs or invoke command-line crypto tools for generation.
- Local library copies may be inspected for research. At build time, download
  libraries compiled for this project from GitHub; do not depend on sibling
  checkouts or other local source overrides.
- Automatic DNS configuration is a future feature. Defer implementation until
  the owner's DNS server has the necessary REST API features; domain management
  must work without that integration in the meantime.
- Use [RESTinCurl](https://github.com/jgaa/RESTinCurl) for outgoing REST requests
  when that integration is implemented.
- Run in a container with filesystem access to Courier configuration and mail
  spool directories, under the same user as the mail server (`Agents.md`).
- Configuration write access must use a dedicated administration group granted
  to the admin container process only. Do not add the host `courier` account or
  Courier daemons to that group, or grant them configuration write access.
  The owner can install the missing mail delivery/auto-reply runtime packages.
- For new accounts and password resets, accept an administrator-supplied password
  or generate a secure one. Return generated passwords in cleartext in that
  operation's reply, never in logs. The owner will rotate existing passwords
  manually through either API option. Existing plaintext passwords may be
  converted to hashes only where verified safe without changing the password.
- Account deletion takes effect immediately. Filesystem cleanup may run on a
  worker thread, tracked durably in a database table or files in a persistent
  spool directory. Cleanup must be idempotent and resume after crashes/restarts.
- Complete initial research and discuss this draft before implementation.
- Initial research/discussion is complete. Document mail-server requirements,
  automate provisioning/upgrades with Ansible, use a disposable staging server
  for testing/debugging, and begin implementing the admin daemon.
- Build and test container images locally, later in GitHub Actions. Deploy only
  finished images to staging; do not build images or compile sources there.

## 2. Research sources and confidence

Legacy source root: `/var/tmp/ma/mailadm-0.02/`. Paths in the following table are
relative to that directory. The directory name is not a reliable version for
every component: the manual identifies itself as v0.01.01 (2002), while the
utility header declares version 1.01 and copyright through 2008. The manual
predates features present in the code. Where they differ, the observations below
identify the actual source behavior.

| Source | Evidence supplied |
| --- | --- |
| `doc/mailadm-manual.xml` | Purpose, organizations, migration, mailbox fields, deployment and reconciliation intent |
| `mysql/create_maildb.sql` | Four tables, fields, indexes, defaults and Courier authentication layout |
| `www/index.php`, `www/lib/list_orgs.php`, `www/lib/edit_org.php`, `www/lib/submit_org.php` | Administrative entry points and organization management |
| `www/lib/edit_mbox.php`, `www/lib/submit_mbox.php`, `www/lib/list_mbox.php` | Mailbox creation, update, quotas, credentials and aliases |
| `www/lib/delete_org.php`, `www/lib/delete_mbox.php` | Cascading deletion and spool removal |
| `www/lib/resynch.php` | Generated Courier aliases/domains and external activation commands |
| `www/user/index.php` | Mailbox login, password change, multiple forwards, keep-copy and out-of-office settings |
| `src/courierfilter.cpp` | Inheritance, managed maildrop filters, forwarding and autoresponder generation |
| `src/spam.cpp` | Age-based spam movement/deletion and header-based cleanup |
| `src/mailadmutil.cpp`, `src/mailadmutil.h`, `src/mailadmutil.conf`, `src/mysql.cpp`, `src/Makefile` | Filesystem layout, privileges, configuration, database access and helper commands |
| `www/setup/config.php`, `www/lib/lib.php` | PHP configuration, database and process invocation helpers |
| `www/lib/org_report.php`, `www/lib/download_ins_file.php` | Organization reports, MX diagnostics and legacy client setup exports |

The manual embeds a sample `authmysqlrc`; the supplied `mysql/` directory contains
only the schema script. No running Courier instance, production database, logs,
or deployment configuration was inspected. Findings are from static inspection;
the legacy software was neither built nor executed.

The local yahat checkout was inspected at HEAD
`d78714d1434205fef234b36d0799d88900100035`, specifically `docs/http.md`,
`docs/metrics.md`, `docs/building.md`, `include/yahat/HttpServer.h`,
`include/yahat/Metrics.h`, `src/HttpServer.cpp` and the API server example.
RESTinCurl's upstream README was inspected for its asynchronous integration
surface; no dependency was installed.

Follow-up research used yahat commit
`67cb6c31c13f695fc548c71f7789a0619b1333d4` and the new
[`yahat-rest-api` skill](../../yahat-cpp/.agents/skills/yahat-rest-api/SKILL.md),
including its [handler patterns](../../yahat-cpp/.agents/skills/yahat-rest-api/references/handlers.md).
The skill documents the supported bridge from REST handlers to C++20 awaitables;
this resolves the earlier open question about needing a native awaitable REST API.

## 3. Observed legacy model

### Organizations and domains

An organization represents a company or administrative grouping. Each mailbox
belongs to one organization. An organization has a name, one `default_domain`,
informational inbound POP/IMAP and outbound SMTP server names, and a
`hosted_here` switch. Nonlocal organizations can be retained as account records
without activating their domain in Courier.

Only organization names are unique in the schema. Domains are strings on
organizations, not independent records, and are not constrained to be unique.
Several organizations can therefore name the same domain. Primary mailbox
addresses and aliases can also name other domains. The legacy organization is
consequently not a reliable one-to-one representation of a hosted mail domain.

Organization filter defaults are `filterctl=yes`, `spammgt=no`, spam folder
`spam`, retention `30` days and trash folder `Trash` in SQL. The PHP new-organization
form can instead select spam management on and submit empty values. The rewrite
needs one explicit set of defaults rather than inheriting this discrepancy.

### Mailboxes and credentials

The `passwd` table extends Courier's suggested MySQL authentication layout:

| Fields | Meaning |
| --- | --- |
| `passwd_id`, `organization_id` | Numeric mailbox identity and owning organization |
| `id`, `name` | Unique primary email/login address and display name |
| `crypt`, `clear` | Password hash or plaintext password |
| `uid`, `gid` | Mailbox filesystem ownership; schema defaults are 65534 |
| `home`, `maildir` | Absolute filesystem paths |
| `quota` | Courier quota expression, stored as a string |
| `active` | Enabled/disabled flag, default enabled |
| `old_popname` | Optional alternate login for migration from old POP accounts |
| `filterctl`, `spammgt`, `spamfld`, `spamdays`, `spamtrash` | Nullable overrides of organization policy |
| `oomsg`, `keep_copy` | Out-of-office text and whether forwarding retains local delivery |

The admin forms append the organization's default domain to primary addresses
and aliases supplied without `@`. A fully qualified address is accepted as given.
Quota examples shown in the form are `5000000S`, `200C`, and `40000S,300C`, for
byte and message-count limits. The administration code stores the expression;
it does not implement enforcement itself.

Credentials can be stored as plaintext or via PHP `crypt()`. The optional legacy
password generator creates eight uppercase letters using a noncryptographic
PRNG. Passwords are shown in mailbox lists and reports when stored in plaintext.
The self-service password change explicitly switches back to plaintext storage.
These are historical facts, not proposed security requirements. End-user
self-service is excluded from the rewrite.

The manual's Courier authentication query accepts either the full primary
address or `old_popname`, and requires an active mailbox in a locally hosted
organization. Alternate login names have no uniqueness constraint. Management of
these old migration aliases is excluded from the rewrite; any existing use must
be identified during takeover before changing the authentication query.

### Aliases and forwarding

- `alias`: unique email address, mailbox reference and duplicated organization
  reference. Each alias targets one mailbox; one mailbox can have many aliases.
  This does not model distribution lists or arbitrary alias chains.
- `forward`: mailbox reference and destination address; multiple destinations
  are supported by both the self-service form and generated filter.
- `keep_copy=no` is the schema default. With forwarding enabled, the final
  destination ends local delivery unless keep-copy is enabled. With no forwards,
  delivery ends in the local Maildir.
- Mailbox updates replace the complete alias set. Self-service updates replace
  the complete forwarding set. These multi-statement changes are not transactional.
- Primary-address uniqueness and alias uniqueness are separate indexes. The
  schema does not prevent an alias from colliding with a primary address, nor
  does it declare foreign keys or enforce consistent organization references.
- No catch-all or mailing-list management was found. Header-based auto-archiving
  appears only as a TODO in `courierfilter.cpp`.

## 4. Observed workflows and Courier integration

### Administration and self-service

The PHP administration interface lists, creates, edits and deletes organizations
and mailboxes, manages aliases, generates organization reports, and invokes a
global resynchronization. Organization deletion removes its mailboxes and aliases
as well as requesting removal of its entire spool directory. Mailbox deletion
removes its aliases and database row and requests removal of its spool. Neither
deletion path cleans up `forward` records.

The separate self-service page authenticates with the primary mailbox address
and password and allows password, forwarding, keep-copy and out-of-office updates.
An empty out-of-office message disables the generated autoresponder. The source
does not apply Courier's active/local-hosting checks to this login, and the submit
path does not explicitly reject an unauthenticated session. No application-level
administrator authentication or authorization is present in the supplied admin
entry point; any external web-server access controls are unknown.

Organization reports include account details and an external `testmxlookup`
invocation. The project also writes downloadable Microsoft Outlook `.ins` files
containing account configuration and plaintext credentials. These are legacy
presentation/export features, not required outputs for the new backend.

### Maildir lifecycle

The intended generated layout is:

```text
<virtuser-root>/<organization-id>/<mailbox-id>/
    Maildir/
        tmp/
        new/
        cur/
        .<spam-folder>/{tmp,new,cur}/
        .<trash-folder>/{tmp,new,cur}/
```

The helper creates missing directories and assigns ownership. Organization,
mailbox and Maildir directories are created with mode 0700; the virtual-user root
uses 0755. Managed filter files use 0600. New mailbox creation persists the derived
home and Maildir paths. Numeric identities keep paths independent of address
renames. The sample virtual-user root and the compiled fallback differ, so paths
must be explicitly configured and checked during takeover.

Spool deletion is separately gated by `allow-delete` in the helper configuration;
otherwise the database can be deleted while files remain for manual removal.
The legacy helper runs setuid/setgid root and uses recursive `rm` for deletion.
The container rewrite's same-user model supersedes that privilege design.

### Global reconciliation

`ResynchAll()` performs the following sequence:

1. For records with **both** `home` and `maildir` empty, create Maildirs and populate
   paths. This supports third-party database imports; it is not a general repair
   of every missing directory.
2. Write `mailaddr: primary-address` entries for aliases targeting active
   mailboxes to `/etc/courier/aliases/mailadm_generated` by default. This query
   does not check the organization's `hosted_here` flag.
3. Write sorted, distinct organization default domains with `hosted_here=yes`
   to `/etc/courier/hosteddomains/mailadm_generated` by default. Domains appearing
   only in aliases are not added; the manual requires configuring those manually.
4. Invoke `makealiases`, `makehosteddomains`, `makesmtpaccess`,
   `makeacceptmailfor`, then `courier restart` through the helper.

The PHP invocation misspells `run-makesmtpaccess` as `run-makesmtpacess`, so that
step cannot dispatch successfully as written. Ordinary organization and mailbox
updates regenerate filters but do not perform full alias/domain activation.
Mailbox deletion asks the operator to resynchronize later; organization deletion
only invokes `makealiases` without regenerating the text files. This can leave
Courier configuration stale. Global reconciliation does not itself regenerate
all mailbox filters.

### Filters, autoresponses and spam maintenance

`courierfilter` selects active mailboxes in locally hosted organizations, either
globally or for one organization/mailbox. Each nullable mailbox policy field
inherits independently through SQL `IFNULL`. Effective `filterctl=no` skips filter
generation, allowing external/manual management.

The generator intends to connect `.courier` to `/usr/bin/maildrop`, write a
`.mailfilter`, preserve an initial `.bak`, and generate these rules in order:

1. Route messages tagged `X-Spam-Flag: YES` to the configured spam folder when
   spam management is enabled.
2. Deliver delivery-status reports locally before autoresponding or forwarding.
3. If out-of-office text is set, invoke `mailbot` with a UTF-8 message file and
   reply-tracking database (`-D 1` in the generated command).
4. Forward to all destinations, retaining local delivery according to keep-copy.
5. Deliver remaining mail locally.

Spam classification is external: this program consumes tags; it is not a spam
scanner. The generator has concrete path bugs: it attempts to open the home
directory instead of `<home>/.courier`, then mutates `home` before building the
autoresponse path. Its file handling cannot be treated as a working reference
implementation. Disabling spam management also does not reliably relinquish
ownership of an existing manual filter: the code continues to write a filter.

`move-spam` intends to move messages from spam `new/` and `cur/` after their
filesystem modification time exceeds the inherited retention period; zero days
means immediately. An empty destination means deletion. In the actual code,
`spamtrash` is never assigned to the destination variable, so eligible files are
deleted rather than moved. Its SQL also lacks parentheses around the inherited
spam-enable condition, allowing that branch to bypass active/local checks.

`clean-spam` is a separate utility: scan headers for a configurable,
case-insensitive extended regex and remove matching files older than a threshold
(default seven days), optionally recursively and in simulation mode. The sample
regex also matches delivery-status reports. Simulation is not consistently
side-effect free across utility commands. No scheduler or mail-traffic metrics
collector is included in the supplied source.

## 5. Deployment facts supplied by the owner

The running server reports MariaDB
`10.11.19-MariaDB-0+deb12u1` on Debian 12. Its `mail` database contains exactly
`alias`, `forward`, `organization` and `passwd`. Owner-supplied `DESCRIBE` output
confirms the expected legacy fields, including nullable mailbox filter/spam
overrides, forwarding and auto-reply settings, and 128-character `crypt`/`clear`
columns. Any selected hash encoding must fit the deployed column or require an
explicit schema change before use.

Follow-up queries confirm:

- All four tables use **MyISAM** and `latin1_swedish_ci`. The existing tables do
  not provide transactional updates. Preserve text encoding/collation during
  initial takeover; any later character-set conversion is a separate change
  requiring inspection of actual stored bytes.
- `SHOW INDEX FROM organization` lists unique `name_key(name)` and a nonunique
  `organization_id` index, with no explicit `PRIMARY` index. This is more precise
  than the earlier `DESCRIBE` display marking `name` as `PRI`. The duplicate-ID
  query returned no rows: IDs currently have no duplicates, but uniqueness is not
  enforced. Validate again before adding a numeric primary/unique key.
- There are **16 accounts**, all configured with **UID 107, GID 113**. These are
  confirmed by `getent` as **courier:courier**, not the schema defaults of 65534.
  The system account's home is `/var/lib/courier` and shell is
  `/usr/sbin/nologin`. Virtual mailbox homes must still come from database records;
  filesystem ownership is verified for the sampled mailbox below, not all accounts.
- **8 accounts have plaintext passwords; 8 have hashes; none have both**. Hash
  formats and runtime verification of safe plaintext conversion remain open.

The remaining tables report the expected numeric primary keys, with unique
mailbox `id` and alias `mailaddr` fields. Complete constraints and hash-format
counts remain to be collected.

### Installed packages and running services

Owner-supplied package and runtime output confirms:

| Component | Installed version |
| --- | --- |
| Courier base/MTA/webadmin | `1.0.16-3+b6` |
| Courier IMAP | `5.0.13+1.0.16-3+b6` |
| Courier authdaemon/authlib/MySQL/userdb modules | `0.71.4-1+b2` |
| MariaDB server/client | `10.11.19-0+deb12u1` |
| OpenSSL | `3.0.22-1~deb12u1` |
| libsodium runtime | `1.0.18-1+deb12u1` |
| libcrypt runtime/development package | `4.4.33-2` |
| SpamAssassin/spamc | `4.0.1-1~deb12u1` |
| couriergrey | `0.3.2-5+b4` |

Courier authentication, IMAP, IMAPS, MTA, MTA SSL, MSA, main Courier and filter
services are active. These Courier units are `Type=forking`, have no explicit
systemd User/Group, and report `CanReload=no`. Runtime processes include both
root and `courier:courier` identities; do not run the API as root merely because
privileged Courier processes exist. The mapping of mailbox UID 107/GID 113 to
account/group names is confirmed as `courier:courier` by subsequent `getent` output.

MariaDB is active as `mysql:mysql`; its main socket unit is also active. The
listener snapshot shows wildcard listeners on 25, 143, 465, 587, 993 and 3306, with
no listeners shown on 110 or 995. This does not establish firewall reachability.

`makealiases`, `makehosteddomains`, `makeacceptmailfor`, `makesmtpaccess` and
`courier` resolve under `/usr/sbin`; `maildirmake` resolves under `/usr/bin`.
`maildrop` and `mailbot` did not resolve via `command -v`; subsequent explicit
checks confirm `/usr/bin/maildrop` and `/usr/bin/mailbot` do not exist. This is
not proof that equivalents do not exist elsewhere. Current auto-reply execution
is unconfirmed. Verify per-mailbox delivery/filter settings before deciding which
runtime packages must be restored or provided for auto-replies. Installed userdb
support likewise does not prove it is enabled instead of or alongside authmysql.

The fact-collection loop tried uninstantiated `mariadb@.service` and socket
templates and received unit-name errors. Those errors are a collection-command
issue, not evidence of failing MariaDB instances; the loop now skips templates.

### Active authentication and delivery configuration

The selected configuration supplied by the owner establishes:

- `authmodulelist="authmysql"` with five authdaemon workers. The longer
  `authmodulelistorig` is not the selected module list.
- authmysql connects to `localhost:3306`, database `mail`, table `passwd`.
  Although the selected directives explicitly name `MYSQL_CRYPT_PWFIELD crypt`
  and do not name a plaintext field, the custom `MYSQL_SELECT_CLAUSE` explicitly
  returns **both** `P.crypt` and `P.clear`. Do not conclude plaintext is ignored.
- The query returns primary address, hash, clear password, UID/GID, home, Maildir,
  quota and display name in Courier's expected order. It joins organization by
  numeric ID, requires `O.hosted_here='yes'` and `P.active='yes'`, and matches
  either the full primary address or `old_popname` specifically when the requested
  domain is `nodomain.jgaa.com`.
- Preserve that existing lookup during takeover. Excluding management of old POP
  migration features does not authorize removing a working authentication branch.
  New explicit domain records must remain compatible with the current primary
  address lookup and organization hosting check, or change the query deliberately.
- Selected `IMAP_CAPABILITY` contains no `AUTH=CRAM-*`; the TLS value adds
  `AUTH=PLAIN`. `IMAP_CAPABILITY_ORIG` contains CRAM mechanisms, but is distinct
  from the selected capability value. SMTP sets `ESMTPAUTH="LOGIN"` and
  `ESMTPAUTH_TLS=""`; SMTP/SSL has no additional selected auth directive in the
  supplied output. Do not infer effective post-TLS behavior from the variable
  names alone; inspect live capabilities/startup behavior if needed.
- The reported selected mechanisms suggest hash-only storage is feasible, but
  do not prove every active listener/client path is compatible. Verify the hash
  format with a controlled account and real authentication before bulk conversion;
  keep the eight existing hashed accounts unchanged during initial takeover.
- `DEBUG_LOGIN=1` enables authentication diagnostics. Courier documents level 2
  as additionally displaying passwords; do not enable it for this work.
  See [Courier authentication debugging](https://www.courier-mta.org/authlib/README.authdebug.html).
- `DEFAULTDELIVERY="| /usr/bin/spamc | /usr/bin/maildrop"` is the configured
  delivery pipeline. `/usr/bin/maildrop` is confirmed absent, so the configuration
  references a missing executable. Whether current deliveries actually use this
  default depends on mailbox-specific instructions and runtime behavior; inspect
  those before concluding that all delivery is broken or changing the pipeline.

### Configuration filesystem boundary

Observed metadata (UID/GID are numeric filesystem owners):

| Path | Mode | UID:GID |
| --- | --- | --- |
| `/etc/courier/aliases` | `0750` | `0:113` |
| `/etc/courier/hosteddomains` | `0750` | `0:113` |
| `/etc/courier/esmtpacceptmailfor.dir` | `0750` | `0:113` |
| Their `mailadm_generated` files | `0644` | `0:0` |
| `/etc/courier/aliases/system` | `0640` | `0:113` |
| `/etc/courier/smtpaccess` | `0755` | `0:0` |
| `/etc/courier/smtpaccess/default` | `0644` | `0:0` |
| `/etc/courier/authdaemonrc`, `/etc/courier/authmysqlrc` | `0640` | `0:113` |

UID 107/GID 113 alone cannot write these managed files or replace them atomically
in the current directories. A writable container mount alone does not override
host file permissions. Agree on narrowly scoped write access to dedicated managed
configuration files/directories and on how rebuild commands publish their derived
files. Keep authentication configuration and unrelated operator-managed files out
of that write scope. No permission changes are made by this research.

The owner has selected a dedicated administration group for this boundary.
Proposed group name: `mailadmin-config`, with its actual host GID supplied to the
container as a supplementary group (for example through container `group_add`).
Keep the process UID/GID at 107:113 for spool operations. Do **not** add that group
to the host `courier` user's account membership: only the container process gets
the extra group. Keep managed configuration root-owned with the admin group
providing write permission, and give ordinary Courier processes only the read/
traverse permissions they need. Making Courier the file owner would let that
shared UID change mode bits, undermining the intended write boundary.

Limit writable mounts and group permissions to service-managed configuration
sources and required generated artifacts. Keep `authmysqlrc`, `authdaemonrc`,
private keys, binaries and unrelated configuration outside this writable scope.
For atomic replacement the writer also needs write/traverse access to the
containing directory; group-writable files alone are insufficient. Writable
directories permit replacement of their entries, so isolate managed entries
from unrelated root-owned files rather than broadly opening `/etc/courier`.
The exact source-directory layout, read permissions/ACLs and rebuild output paths
must be checked against Courier's tools before issuing ownership/mode commands.
Do not assume those tools support an arbitrary nested directory layout.

File access and process activation are separate: granting the admin group write
access does not grant host service-control privileges or permission to publish
every derived Courier database. Decide the minimal rebuild/activation mechanism
without giving the HTTP service general root privileges.

For Debian 12, the
[`maildrop` package](https://packages.debian.org/bookworm/maildrop) supplies both
maildrop and mailbot. The owner is willing to install missing runtime packages;
installation is not yet confirmed. Recheck binaries and verify delivery/filter
behavior after installation before enabling generated auto-replies.

### Mailbox inspection and maintenance status

The initial mailbox-home prompt was cancelled with Ctrl-C and did not establish
mailbox permissions. A subsequent successful inspection selected an active
mailbox directly from MariaDB and confirmed:

| Path | Mode | Owner |
| --- | --- | --- |
| `/var/lib/courier` | `0750` | `courier:courier` |
| `/var/lib/courier/spool-virtusers` | `0750` | `courier:courier` |
| `/var/lib/courier/spool-virtusers/6` | `0700` | `courier:courier` |
| `/var/lib/courier/spool-virtusers/6/30` | `0700` | `courier:courier` |
| Its `.mailfilter` | `0600` | `107:113` (`courier:courier`) |
| Its `Maildir`, `Maildir/tmp`, `Maildir/new`, `Maildir/cur` | `0700` | `107:113` (`courier:courier`) |

No symlink components were reported along these paths. The mailbox resides on
`/dev/sda`, mounted as `/` with ext4 and `rw,relatime,errors=remount-ro`. No
`.courier` entry was reported by the existence check. The `.mailfilter` contents
and actual execution were not inspected, so its presence alone does not prove
maildrop is being invoked successfully.

This sample matches the legacy numeric organization/mailbox path layout and
supports using UID 107/GID 113 with a writable spool mount in the container.
Preserve `/var/lib/courier/spool-virtusers` and recorded account paths during
takeover; ensure the configured container paths match Courier's absolute paths.
This does not resolve write access to `/etc/courier`, nor establish ownership
or filter behavior for the other fifteen accounts. No broader spool scan is
needed for this initial specification; validate managed paths during takeover.

The filename scan found `/etc/cron.daily/couriergrey` and
`/etc/cron.daily/spamassassin`. Neither a mailadmin-specific cleanup cron filename
nor a mail-related cleanup systemd timer was shown. These results do not rule out
root/user crontab entries, differently named jobs or external scheduling, and do
not establish the contents/purpose of either cron file. Existing spam cleanup
scheduling remains unconfirmed.

## 6. Rewrite requirements and implementation proposals

Owner decisions in section 1 are confirmed. Details below implement those
decisions or remain proposals where indicated. See section 8 for implementation
status; unfinished requirements are not implied to be working endpoints.

### Domain model and API capabilities

- Represent domains explicitly, with exactly one organization owner and multiple
  domains allowed per organization. Map existing organization defaults and address
  domains into that model; report conflicting ownership rather than guessing.
  Preserve existing identifiers and spool paths during takeover; do not relocate
  mail merely to fit a new schema.
- Offer administrative operations for organization/domain inventory, mailbox
  lifecycle, enable/disable, credential reset, aliases, quotas, filter policies,
  forwarding, and out-of-office settings. Expose configured overrides alongside
  effective inherited policy so clients can explain behavior.
- Provide an administrator-triggered reconciliation operation and report which
  Courier/DNS steps succeeded or failed. Do not report complete success when only
  a database write succeeded. Prefer direct coroutine operations with clear
  results; introduce background jobs only if operation duration requires them.
- Use a versioned JSON HTTP API with validated request schemas, bounded lists,
  explicit errors, and documented conflict/not-found semantics. Route names,
  schema details, pagination and long-operation response conventions remain open.
- Expose mailbox settings through administrator operations only. Do not implement
  mailbox-owner authentication, self-service routes or organization delegation.
- Account deletion immediately removes/disables the authentication identity and
  its aliases/forwarding; only physical file removal may be deferred. Report
  cleanup status separately. Do not delay logical deletion for filesystem scans.
- Validate domain/address ownership and address collisions across primary
  mailboxes and aliases, not just within separate tables. Cross-domain aliases
  must reference deliberately managed/authorized domains.

### Persistence and applying configuration

- Use the existing MariaDB deployment, with Courier authentication compatibility
  in mind. Prefer reusing the existing database and Courier authentication
  contract for a simple takeover. Conversion of the four legacy tables to InnoDB
  is approved. Plan backup, validation and the activation window before applying
  it. Enforcing uniqueness of numeric organization IDs is still a proposed schema
  change. Keep existing identities, paths, columns and collation; no production
  schema changes have been executed in this research session.
- Keep database changes transactional where possible. Database, filesystem,
  Courier activation and remote DNS cannot share one transaction. Report partial
  failure and make reconciliation safe to rerun after interruption. Do not add
  a separate workflow engine, broker or distributed transaction mechanism.
- Make provisioning and reconciliation idempotent. Serialize conflicting changes
  to shared Courier files; generate deterministic output and replace files
  atomically. Preserve unrelated operator-managed configuration.
- Establish which configuration/filter files the service owns. Respect manual
  filter control and define what happens when managed control is disabled.
- Restrict filesystem operations to configured roots and service-generated
  paths; prevent traversal and symlink escapes, especially during deletion.
  Validate values embedded into maildrop syntax and avoid shell interpolation.
- Validate runtime UID/GID and mount permissions. Decide how Courier's compiled
  configuration is rebuilt and activated from the container: sharing files
  does not itself provide host process control or root privileges. Do not assume
  the old setuid helper or a full Courier restart is the required solution.
- Choose one authoritative source of desired configuration. Direct third-party
  database writes and migration imports were supported historically, but new
  import tools and ongoing support for external database writers are not required.
  Reading and validating the existing database for takeover is required.

### Deletion and resumable cleanup

- Proposed simple design: one cleanup worker thread in the service and an InnoDB
  cleanup table in the existing MariaDB database, paired with the approved
  conversion of legacy tables to InnoDB. An InnoDB cleanup table alone cannot
  make deletion from the current MyISAM tables transactional; verify conversion
  has completed before enabling transactional deletion.
- Commit logical deletion and cleanup intent in one transaction, preserving
  the mailbox identity and cleanup paths in the journal before deleting records.
  Keep Courier file activation outside that transaction with explicit retry state.
- Remove authentication/delivery eligibility immediately, including generated
  Courier alias changes. Do not return successful deletion if activation fails;
  retain the disabled state and enough information to retry. Verify existing
  session/authentication-cache and queued-delivery behavior on the actual server;
  the filesystem worker alone cannot invalidate sessions or cached identities.
- At startup, resume pending and interrupted cleanup. A missing file/directory is
  already cleaned; retry failures with bounded backoff and log/report them. Mark
  the item complete only after removal succeeds or the target is absent.
- Store immutable account identity and validated cleanup paths. Prevent a retry
  from deleting a replacement account's data: do not reuse identities/paths while
  cleanup is pending. Confine removal to configured roots without following
  symlinks. Coordinate in-flight mail delivery and filter updates with deletion.
- Keep this worker limited to necessary cleanup. It does not require a general
  job system. Organization/domain deletion must apply the same guarantees to
  affected accounts; API cascade details remain to be specified.

### Auto-replies and spam cleanup

- Include administrator-controlled auto-reply text and enable/disable behavior,
  forwarding/keep-copy, and inherited spam policy. Reuse deployed Courier/maildrop
  facilities where compatible, respecting manually managed filters.
- Add repeatable scheduled spam cleanup off HTTP I/O threads. Define folder,
  retention age and move-versus-delete behavior explicitly; do not copy the
  legacy bug that deletes messages despite a configured trash destination.
- Do not delete ordinary delivery-status reports merely because the old sample
  spam regex matched them. Confirm the deployed spam tagging and retention policy
  before enabling cleanup. Cleanup scheduling and defaults remain open.

### Asynchronous implementation and dependencies

- Use Asio C++20 coroutine service operations. Boost.Asio is the proposed flavor
  because the selected HTTP server uses it. Define executors, operation lifetimes,
  deadlines, shutdown and cancellation behavior explicitly.
- Keep blocking database calls, filesystem walks and child-process waits off
  HTTP I/O threads; use asynchronous facilities or bounded worker execution.
- The inspected yahat API uses `Response onReqest(const Request&)`, a request
  `yield_context` pointer, and stackful `boost::asio::spawn` internally. It does
  not expose a native `awaitable<Response>` REST override. Follow the
  `yahat-rest-api` skill: keep a thin REST handler, obtain
  `auto yield = *request.yield`, and invoke named C++20 coroutine operations with
  `boost::asio::co_spawn(yield.get_executor(), operation, as_tuple(yield))`
  and explicitly propagate a non-null completion exception. This avoids a
  Boost 1.83 null-exception rethrow on successful direct `yield` completions. It suspends
  the HTTP coroutine until completion without blocking its worker. No yahat API
  extension or MCP feature is required.
- Give coroutine operations owned inputs and keep application services alive
  through completion. Do not retain borrowed request views, use temporary
  capturing coroutine lambdas, hold locks across suspension, detach work required
  for the response, or block on futures. Shared handlers serve concurrent sessions;
  the session strand does not serialize all access to application state.
- Use yahat route policies for methods, exact matching, authentication and
  explicit CORS. Routes are literal path prefixes; parse and validate resource
  IDs/subpaths in the handler. Use Boost.JSON for validated request bodies and
  owned serialized responses. Ordinary REST disconnects do not guarantee prompt
  operation cancellation, so downstream calls need their own deadlines.
- RESTinCurl documents an Asio completion-token API,
  `AsioAsyncExecute(boost::asio::use_awaitable)`, suitable for the planned client
  boundary. Confirm cancellation, lifetime and executor behavior against the
  selected revision before integrating. Its generic coroutine API and its
  internal worker thread are not a reason to bypass Asio operation ownership.
  See the [upstream README](https://github.com/jgaa/RESTinCurl#coroutines).
- Use CMake for dependency integration and central version information. Fetch
  libraries compiled for the project from their GitHub repositories at build
  time, including yahat-cpp; the inspected `../yahat-cpp` is a research reference,
  not a build dependency path. Do not add local-source overrides. The initial
  CMake/Docker build uses GitHub sources for yahat/logfault and distribution
  binaries for Boost, Connector/C, OpenSSL and zlib.

### Automatic DNS provisioning

Deferred until the owner adds the necessary features to the DNS server. No DNS
client, background retries, provider framework or placeholder endpoints are
required now. New domains must be manageable with DNS configured separately.
The following are notes for the future integration, not current release work:

- Isolate DNS behind a provider interface using RESTinCurl. No provider URL,
  authentication scheme, request payload or concrete endpoints are assumed.
- Report DNS configuration outcomes separately from local Courier activation,
  including failures that need an administrator retry. Define when a domain is
  ready for use and whether propagation checking is needed. Start with a simple
  status representation rather than a general provisioning state machine.
- Make retries bounded, observable and safe against duplicate remote changes;
  reconcile ambiguous timeouts before repeating non-idempotent operations.
- Change only records the service is authorized to manage. Store credentials
  outside responses/logs and validate TLS for outgoing requests.
- Agree on the record set and values: MX, address records, SPF, DKIM and DMARC
  are discussion candidates, not confirmed scope. DKIM additionally needs an
  agreed signer/key lifecycle; DNS provisioning alone cannot supply that.
- Do not automatically reprovision DNS for existing domains during takeover.
  Automatic provisioning is required for new domains; modifying existing DNS
  requires an explicit administrator operation.
- Define behavior for existing zones/records, nonlocal organizations, provider
  outages, domain renames/removal and cleanup. Do not assume domain deletion
  authorizes deletion of an entire zone or unrelated records.

### Metrics for hosted mail domains

Yahat supplies OpenMetrics metrics and a default `/metrics` route, with counters,
gauges and histograms among its supported types. Its built-in HTTP request
metrics describe API traffic; custom collectors are needed to describe hosted
mail domains. The metric label must identify the managed mail domain, rather
than trusting the request's HTTP `Host` header.

Required domain metrics (names and attribution below are proposed API details):

| Domain metric | Type and source |
| --- | --- |
| `mailadmin_email_accounts{domain="example.org"}` | Gauge counting accounts whose primary address belongs to the domain |
| `mailadmin_aliases{domain="example.org"}` | Gauge counting alias addresses belonging to the domain, irrespective of the target account's domain |

Emit both series for every hosted domain, including zero counts. Proposed account
counts include enabled and disabled accounts until logical deletion; aliases do
not inflate account counts. Source these values from database configuration, not
filesystem scans. A collection failure must not be presented as healthy zeros.
Retire series when a domain is removed. Protect metrics with the administrator
bearer credential or a separately agreed scrape boundary. No usage collectors,
storage scans, mail-log parsers or queue/delivery statistics are in scope.

### Security, operations and scope boundaries

- Require `Authorization: Bearer <api-key>` for administrative operations over
  TLS or the agreed trusted proxy boundary. The same binary/container must offer
  a CLI key-creation command; final syntax is to be specified. Generate keys with
  the selected crypto library, print a new key once to CLI output, store only a
  verifier, and keep it out of logs. Provide simple revocation/rotation. No JWT
  issuer, OAuth server, delegated roles or end-user identity system is required.
- Use Courier-compatible modern password hashing, never return stored hashes or
  plaintext passwords in inventory/report responses. Generate every new password,
  token, salt and other secret with OpenSSL linked as a library or libsodium;
  check generation failures and abort the operation rather than falling back to
  insecure randomness. Password hashing must remain compatible with the deployed
  Courier authentication module; selecting a random generator does not select
  the password hash format. Accept administrator-supplied passwords as well as
  generated ones for creation/reset. Return a generated password in cleartext
  only in the creation/reset reply, with `Cache-Control: no-store`; never persist
  it for later retrieval or log request/response bodies containing it.
- Conversion of existing plaintext to hashes is permitted only after checking
  Courier's configured hash support and enabled authentication mechanisms. Some
  mechanisms need plaintext; migration must not break those clients. Convert and
  clear the plaintext atomically where safe, preserving the password. Password
  rotation remains an explicit administrator operation, not an automatic takeover
  action. See [Courier authmysql documentation](https://www.courier-mta.org/authlib/README.authmysql.html).
- Bound request bodies, list sizes, concurrent work and external-call duration.
  Configure TLS or an agreed trusted reverse-proxy boundary and explicit CORS.
- Log configuration changes and operational outcomes with correlation IDs and
  domain context; provide DEBUG/TRACE diagnostics without credentials, message
  contents or sensitive remote payloads. Log API failures and return an
  appropriate failing HTTP status with a useful, sanitized JSON error and the
  same correlation ID. Do not expose raw SQL, credentials, authorization headers
  or internal exception dumps in replies. Define health/readiness signals.
- Build future verification around temporary spool/config roots, a test database
  and fake Courier activation commands. Cover inheritance, collisions, manual
  filter ownership, destructive path containment and cleanup recovery across
  process restarts. DNS verification belongs to the deferred integration.
- Legacy PHP pages, `.ins` exports, general Courier web administration, mailbox
  browsing, end-user access, alternate old POP login management, migration from
  other mail servers, new import tools, message transport and spam classification
  are outside this draft's implementation scope. Auto-replies and safe spam
  cleanup are included.
- MCP impact reviewed: domain administration could be exposed through yahat's
  optional MCP support, but no MCP interface has been requested or approved.
  This specification proposes HTTP only; new MCP tools require owner approval
  under the project rules.

## 7. Remaining facts and details

Collect the deployed configuration using [server fact collection](server-facts.md).
The commands are read-only and exclude credential values and message contents.
The database name/version, legacy fields, table engines/collations, organization
indexes, account counts, configured mailbox UID/GID, credential-storage counts,
selected Courier authentication query/mechanisms, default delivery pipeline and
configuration path permissions and the `courier:courier` identity are known.
The absence of `/usr/bin/maildrop` and `/usr/bin/mailbot` is confirmed. One active
mailbox's paths, filesystem and ownership are also verified. Remaining integration
details are actual delivery/filter execution after package installation, live
authentication/hash compatibility, maintenance scheduling, and the exact managed
configuration directories/rebuild/activation mechanism. The dedicated admin-group
permission model is agreed. These can be resolved in targeted
takeover preparation; the initial inventory and filesystem research are sufficient
for this specification.

After collection, specify the minimal domain-table migration, CLI syntax/API
payloads, supported password hash format, cleanup schedule, and rollback steps.
Ownership, admin-only access, bearer authentication, count-only domain metrics,
deferred DNS and durable cleanup are settled as described above.

## 8. Administration implementation

The backend now implements organization/domain/account/alias CRUD, per-account
forwarding and quotas, supplied/generated passwords, explicit plaintext conversion,
Maildir lifecycle, inherited filtering/spam policies, auto-replies, scheduled spam
cleanup and persistent reconciliation. See [the API contract](api.md) and
[administration/recovery runbook](administration.md) for implemented behavior,
limits and rollout. DNS remains deferred; no MCP tools are added.

The explicit offline schema CLI requires InnoDB, validates legacy ownership and
address conflicts, adds domain ownership and durable activation/cleanup tables,
and preserves existing mail/passwords. Runtime startup does not implicitly apply
DDL. The root publisher accepts only validated domain/alias data and fixed rebuild
commands; the ordinary host Courier user receives no configuration write access.
The verifier and writable DB credential now live behind root-owned boundaries.

Source dependencies remain GitHub-fetched. Images are built/tested locally or by
the existing GitHub Actions workflow and only deployed on staging. Production has
not been contacted or changed. Test coverage and the active staging state are
recorded in [staging verification](staging.md).
