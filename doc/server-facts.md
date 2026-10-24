# Existing Debian 12 mail server: read-only fact collection

Run these blocks in **Bash on the mail server**, in order, and send back the
output. No installation, restart, configuration rebuild, SQL write, password
change or mail deletion is performed. `sudo mariadb` assumes Debian's local
administrative socket login; if it fails, report that rather than sharing a
database password. Python 3 is used only if already installed.

The commands avoid password/hash values, message contents and complete account
rows. Output includes domains, paths and service identities; review it before
sharing. Do not send raw `authmysqlrc`, SQL dumps, full service environments,
private keys, message files or authentication-test output.

## 1. Packages, services and running identities

```bash
cat /etc/os-release
dpkg-query -W -f='${binary:Package}\t${Version}\t${db:Status-Status}\n' |
  awk '$3 == "installed" && $1 ~ /^(courier|mariadb|libmariadb|maildrop|libcrypt|openssl|libsodium|spamassassin|spamc)/'

systemctl list-unit-files --no-pager --no-legend 'courier*' 'mariadb*'
systemctl list-units --all --no-pager --no-legend 'courier*' 'mariadb*'
while read -r mailadmin_unit _; do
  case "$mailadmin_unit" in *@.*) continue ;; esac
  systemctl show --no-pager "$mailadmin_unit" \
    -p Id -p ActiveState -p Type -p User -p Group -p FragmentPath -p CanReload
done < <(systemctl list-unit-files --no-pager --no-legend 'courier*' 'mariadb*')

ps -eo user,group,comm |
  awk 'NR == 1 || $3 ~ /(courier|authdaemon|imapd|pop3d|mariadbd|mysqld|maildrop|spamd)/'
for mailadmin_tool in makealiases makehosteddomains makeacceptmailfor makesmtpaccess maildrop mailbot maildirmake courier; do
  command -v "$mailadmin_tool" || true
done
sudo ss -lntp '( sport = :25 or sport = :110 or sport = :143 or sport = :465 or sport = :587 or sport = :993 or sport = :995 or sport = :3306 )'
```

This establishes package versions, the actual split between privileged Courier
processes and mailbox delivery users, available activation commands, and exposed
listeners. We will not assume that a systemd unit's user is the mailbox UID.

## 2. Selected configuration and metadata

This prints only selected directives. Backslash continuations in custom SQL
queries are joined. Database passwords and TLS key material are not selected.
If custom SQL embeds a secret as a literal, redact that literal before sharing.

```bash
sudo python3 - <<'PY'
from pathlib import Path
import re
import stat

root = Path('/etc/courier')
allowed = {
    'authmodulelist', 'authmodulelistorig', 'daemons', 'DEBUG_LOGIN',
    'MYSQL_SERVER', 'MYSQL_PORT', 'MYSQL_SOCKET', 'MYSQL_DATABASE',
    'MYSQL_USER_TABLE', 'MYSQL_LOGIN_FIELD', 'MYSQL_CRYPT_PWFIELD',
    'MYSQL_CLEAR_PWFIELD', 'MYSQL_UID_FIELD', 'MYSQL_GID_FIELD',
    'MYSQL_HOME_FIELD', 'MYSQL_MAILDIR_FIELD', 'MYSQL_NAME_FIELD',
    'MYSQL_QUOTA_FIELD', 'MYSQL_DEFAULTDELIVERY', 'MYSQL_DEFAULTDELIVERY_FIELD',
    'MYSQL_AUXOPTIONS', 'MYSQL_AUXOPTIONS_FIELD', 'MYSQL_WHERE_CLAUSE',
    'MYSQL_SELECT_CLAUSE', 'MYSQL_ENUMERATE_CLAUSE', 'MYSQL_CHPASS_CLAUSE',
    'MYSQL_CHARACTER_SET', 'DEFAULT_DOMAIN',
    'AUTHMODULES', 'ESMTPAUTH', 'ESMTPAUTH_TLS', 'IMAP_CAPABILITY',
    'IMAP_CAPABILITY_TLS', 'IMAP_CAPABILITY_ORIG', 'POP3AUTH', 'POP3AUTH_TLS',
    'IMAPDSTART', 'IMAPDSSLSTART', 'POP3DSTART', 'POP3DSSLSTART',
    'MAILDIRPATH', 'DEFAULTDELIVERY', 'TLS_REQUIRED', 'AUTH_REQUIRED',
    'virtuser-root', 'mailbox-uid', 'mailbox-gid', 'courier-user',
    'courier-group', 'allow-delete', 'makealiases-path',
    'makehosteddomains-path', 'makeacceptmailfor-path', 'makesmtpaccess-path',
    'courier-path',
}
configs = [root / name for name in (
    'authdaemonrc', 'authmysqlrc', 'imapd', 'imapd-ssl', 'pop3d',
    'pop3d-ssl', 'esmtpd', 'esmtpd-ssl', 'courierd', 'maildrop',
)] + [Path('/etc/mailadmutil.conf')]
for path in configs:
    if not path.is_file():
        continue
    print(f'\n[{path}]')
    text = re.sub(r'\\\r?\n', ' ', path.read_text(errors='replace'))
    for line in text.splitlines():
        match = re.match(r'^\s*([A-Za-z0-9_-]+)(?:\s*=\s*|\s+)(.*)$', line)
        if match and match[1] in allowed:
            print(f'{match[1]} {match[2].strip()}')

print('\n[Configuration path metadata, no file contents]')
for name in ('aliases', 'hosteddomains', 'esmtpacceptmailfor.dir',
             'smtpaccess', 'authdaemonrc', 'authmysqlrc'):
    path = root / name
    if not path.exists():
        continue
    paths = [path]
    if path.is_dir():
        paths += sorted(path.iterdir())
    for item in paths:
        info = item.lstat()
        target = f' -> {item.readlink()}' if item.is_symlink() else ''
        print(f'{stat.filemode(info.st_mode)} uid={info.st_uid} gid={info.st_gid} {item}{target}')
PY
```

We need the authentication query and enabled protocol mechanisms before choosing
hashes. Courier can use hashed and/or plaintext fields, but CRAM-MD5 uses plaintext
credentials; changing storage can affect working clients.
See [Courier's authmysql documentation](https://www.courier-mta.org/authlib/README.authmysql.html).

## 3. MariaDB schema and safe aggregate counts

The confirmed `MYSQL_DATABASE` is `mail`. This prints metadata,
not table data or stored routine/trigger bodies. Keep the same Bash session for
the optional follow-up query.

```bash
mailadmin_db=mail
sudo mariadb --table --database="$mailadmin_db" <<'SQL'
SELECT VERSION() AS mariadb_version, DATABASE() AS mail_database;
SELECT @@socket AS socket_path, @@port AS port, @@bind_address AS bind_address;
SELECT TABLE_NAME, ENGINE, TABLE_COLLATION
  FROM information_schema.TABLES
  WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME;
SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, EXTRA
  FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME, ORDINAL_POSITION;
SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, SEQ_IN_INDEX, COLUMN_NAME
  FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX;
SELECT TABLE_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME
  FROM information_schema.KEY_COLUMN_USAGE
  WHERE TABLE_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL;
SELECT TRIGGER_NAME, EVENT_MANIPULATION, EVENT_OBJECT_TABLE, ACTION_TIMING
  FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE();
SQL
```

Run the next block **only if the schema contains the legacy tables and fields
used below**. Otherwise send the metadata first and we will adapt the queries.
It returns credential categories/counts, never credentials. Nontransactional
tables are not locked, so concurrent changes can make totals differ slightly.

```bash
sudo mariadb --table --database="$mailadmin_db" <<'SQL'
SELECT COUNT(*) AS organizations FROM organization;
SELECT default_domain, COUNT(*) AS organization_count
  FROM organization GROUP BY default_domain ORDER BY default_domain;
SELECT active, COUNT(*) AS accounts FROM passwd GROUP BY active;
SELECT uid, gid, COUNT(*) AS accounts FROM passwd GROUP BY uid, gid;
SELECT COUNT(*) AS aliases FROM alias;
SELECT COUNT(*) AS forwarding_destinations FROM forward;
SELECT CASE
    WHEN COALESCE(crypt,'')='' THEN 'no hash'
    WHEN crypt LIKE '$6$%' THEN 'sha512-crypt'
    WHEN crypt LIKE '$5$%' THEN 'sha256-crypt'
    WHEN crypt LIKE '$1$%' THEN 'md5-crypt'
    WHEN crypt LIKE '$2%' THEN 'bcrypt family'
    WHEN crypt LIKE '$y$%' THEN 'yescrypt'
    ELSE 'other/legacy'
  END AS hash_family,
  (COALESCE(clear,'')<>'') AS has_plaintext, COUNT(*) AS accounts
  FROM passwd GROUP BY hash_family, has_plaintext;
SELECT COUNT(*) AS accounts_with_old_pop_login
  FROM passwd WHERE COALESCE(old_popname,'')<>'';
SELECT COUNT(*) AS accounts_without_both_paths
  FROM passwd WHERE COALESCE(home,'')='' OR COALESCE(maildir,'')='';
SELECT DISTINCT home, maildir FROM passwd
  WHERE home<>'' AND maildir<>'' ORDER BY home LIMIT 3;
SELECT filterctl, spammgt, spamfld, spamdays, spamtrash, COUNT(*) AS organizations
  FROM organization GROUP BY filterctl, spammgt, spamfld, spamdays, spamtrash;
SELECT filterctl, spammgt, COUNT(*) AS accounts
  FROM passwd GROUP BY filterctl, spammgt;
SELECT COUNT(*) AS accounts_with_auto_reply
  FROM passwd WHERE COALESCE(oomsg,'')<>'';
SELECT COUNT(*) AS orphan_aliases FROM alias a
  LEFT JOIN passwd p ON p.passwd_id=a.passwd_id WHERE p.passwd_id IS NULL;
SELECT COUNT(*) AS orphan_forwards FROM forward f
  LEFT JOIN passwd p ON p.passwd_id=f.passwd_id WHERE p.passwd_id IS NULL;
SQL
```

## 4. One mailbox's path permissions and existing maintenance

Select one active mailbox's recorded paths automatically. This inspects directory/file metadata only,
without reading messages, filters, auto-reply text or mailbot databases. It also
shows mount properties needed to plan container access. Do not send directory
listings of message filenames.

```bash
if IFS=$'\t' read -r mailadmin_home mailadmin_maildir < <(
  sudo mariadb --batch --skip-column-names --raw --database=mail -e \
    "SELECT home, maildir FROM passwd WHERE active='yes' AND home<>'' AND maildir<>'' ORDER BY passwd_id LIMIT 1"
) && [[ "$mailadmin_home" == /* && "$mailadmin_maildir" == /* ]] && sudo test -d "$mailadmin_home"; then
  sudo namei -l -- "$mailadmin_home"
  sudo namei -l -- "$mailadmin_maildir"
  sudo findmnt --target "$mailadmin_home" --output TARGET,SOURCE,FSTYPE,OPTIONS
  for mailadmin_path in "$mailadmin_home" \
    "$mailadmin_home/.courier" "$mailadmin_home/.mailfilter" \
    "$mailadmin_maildir" "$mailadmin_maildir/tmp" \
    "$mailadmin_maildir/new" "$mailadmin_maildir/cur"; do
    if sudo test -e "$mailadmin_path" || sudo test -L "$mailadmin_path"; then
      sudo stat -c '%A uid=%u gid=%g %n' -- "$mailadmin_path"
    fi
  done
else
  printf '%s\n' 'No valid existing mailbox paths obtained; no mailbox inspection performed.'
fi
sudo find /etc/cron.d /etc/cron.daily /etc/cron.hourly /etc/cron.weekly \
  -maxdepth 1 -type f \( -iname '*mailadm*' -o -iname '*courier*' -o -iname '*spam*' \) -print
systemctl list-timers --all --no-pager
```

Also describe how spam cleanup currently runs (if at all), whether mailbox
filters are managed by the old program or manually, and whether an authentication
cache such as Courier authdaemon's cache or an external proxy is configured.
Cron filenames/timer names alone may not reveal custom maintenance. Share only
the relevant schedule and command after removing any credentials; do not paste
an entire crontab. We will inspect specific activation scripts or filter rules
only if these results leave something unresolved.
