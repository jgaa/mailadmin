#!/usr/bin/env python3
"""Destructive, synthetic-only administrator integration test on the staging VM.

Requires root on the disposable staging host; never run against production.
Credentials and HTTP bodies are deliberately omitted from test failure output.
"""
import argparse
import http.client
import imaplib
import json
import os
import smtplib
import ssl
import subprocess
import time
from contextlib import closing
from pathlib import Path


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--token-file', default='/root/.config/mailadmin-staging/api-key')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--mail-protocols', action='store_true')
    args = parser.parse_args()
    token = Path(args.token_file).read_text().strip()

    def request(method, path, payload=None, expected=200):
        with closing(http.client.HTTPConnection('127.0.0.1', args.port, timeout=20)) as connection:
            headers = {'Authorization': 'Bearer ' + token}
            body = None if payload is None else json.dumps(payload)
            if body is not None:
                headers['Content-Type'] = 'application/json'
            connection.request(method, '/api/v1/' + path, body, headers)
            response = connection.getresponse()
            data = response.read()
            require(response.status == expected, f'{method} {path}: HTTP {response.status}, expected {expected}')
            require(response.getheader('Cache-Control') == 'no-store', 'Missing no-store')
            return json.loads(data)

    def active():
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            state = request('GET', 'operations')
            if state['activation']['status'] == 'active' and not state['cleanup']:
                return
            time.sleep(1)
        raise RuntimeError('Activation/cleanup did not finish; inspect operations and publisher logs')

    def sql(query):
        return subprocess.check_output(['mariadb', '--batch', '--skip-column-names', '--database=mail', '-e', query], text=True).strip()

    # Fixed .test names plus a unique suffix; no external mail destination.
    suffix = str(time.time_ns())
    host = 'business-' + suffix + '.example.test'
    extra = 'second-' + suffix + '.example.test'
    organization = account = alias = migration_account = None
    auxiliary_accounts = []
    try:
        organization = request('POST', 'organizations', {'name': 'Business test ' + suffix, 'default_domain': host}, expected=201)['organization']['id']
        request('POST', 'domains', {'domain': extra, 'organization_id': organization}, expected=201)
        request('POST', 'domains', {'domain': extra, 'organization_id': organization}, expected=409)
        created = request('POST', 'accounts', {'address': 'owner@' + host, 'name': "O'Brien", 'quota': '10000000S,1000C', 'spam_folder': '__inherit__'}, expected=201)
        require(created['account']['spam_folder'] == '__inherit__', 'Explicit policy value was confused with inheritance')
        account = created['account']['id']
        password = created['generated_password']
        require(len(password) == 64, 'Generated password has wrong entropy encoding')
        require(sql(f"SELECT CONCAT(LEFT(crypt,3),':',LENGTH(clear)) FROM passwd WHERE passwd_id={account}") == '$6$:0', 'Password storage is unsafe')
        require('generated_password' not in request('GET', f'accounts/{account}'), 'Password leaked through account read')
        alias = request('POST', 'aliases', {'address': 'alias@' + extra, 'account_id': account}, expected=201)['alias']['id']
        request('DELETE', 'domains/' + extra, expected=409)
        request('DELETE', f'organizations/{organization}', expected=409)
        request('POST', 'accounts', {'address': 'alias@' + extra}, expected=409)
        request('PATCH', f'accounts/{account}', {'spam_enabled': True, 'spam_folder': '../outside'}, expected=400)
        request('PATCH', f'accounts/{account}', {'quota': "10S\ncommand"}, expected=400)
        request('PATCH', f'accounts/{account}', {'forwards': ['owner@' + host]}, expected=400)
        request('PATCH', f'accounts/{account}', {'password': 'supplied-test-password-123', 'generate_password': True}, expected=400)
        active()
        home = Path(sql(f'SELECT home FROM passwd WHERE passwd_id={account}'))
        require((home / 'Maildir/new').is_dir(), 'Maildir missing')
        require((home / '.courier').read_text().startswith('| /usr/bin/spamc'), 'Delivery pipeline missing')
        require(request('GET', f'accounts/{account}')['account']['active'], 'Account did not activate')
        if args.mail_protocols:
            context = ssl._create_unverified_context()  # Local, disposable staging certificate only.
            with imaplib.IMAP4_SSL('127.0.0.1', 993, ssl_context=context) as imap:
                require(imap.login('owner@' + host, password)[0] == 'OK', 'Hash-only IMAP authentication failed')
            with smtplib.SMTP('127.0.0.1', 25, timeout=20) as smtp:
                smtp.sendmail('', 'alias@' + extra, f'From: sender@{host}\r\nTo: alias@{extra}\r\nSubject: staging delivery\r\n\r\nhello\r\n')
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline and not list((home / 'Maildir/new').iterdir()):
                time.sleep(1)
            require(bool(list((home / 'Maildir/new').iterdir())), 'SMTP alias delivery did not reach Maildir')
        # Exercise all-or-nothing plaintext conversion using bound local SQL,
        # never a password in argv, stdout, task output or application logs.
        import pymysql
        migration_account = request('POST', 'accounts', {'address': 'migration@' + host}, expected=201)['account']['id']
        connection = pymysql.connect(unix_socket='/run/mysqld/mysqld.sock', user='root', database='mail', autocommit=True)
        try:
            with connection.cursor() as cursor:
                cursor.execute("UPDATE passwd SET crypt='',clear=%s WHERE passwd_id=%s", (password, account))
                cursor.execute("UPDATE passwd SET clear='intentionally conflicting plaintext' WHERE passwd_id=%s", (migration_account,))
            command = ['docker', 'run', '--rm', '--network=none', '--read-only', '--user=107:113', '--group-add=2107',
                '--cap-drop=ALL', '--security-opt=no-new-privileges',
                '--mount', 'type=bind,src=/etc/mailadmin/config.json,dst=/etc/mailadmin/config.json,readonly',
                '--mount', 'type=bind,src=/run/mysqld,dst=/run/mysqld,readonly', 'mailadmin:staging',
                'passwords', 'migrate', '--config', '/etc/mailadmin/config.json', '--confirm-hash-compatible']
            failed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            require(failed.returncode != 0, 'Conflicting plaintext/hash conversion was accepted')
            require(sql(f"SELECT LENGTH(crypt)=0 AND LENGTH(clear)>0 FROM passwd WHERE passwd_id={account}") == '1', 'Failed password conversion was not rolled back')
            with connection.cursor() as cursor:
                cursor.execute("UPDATE passwd SET clear='' WHERE passwd_id=%s", (migration_account,))
            converted = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            require(converted.returncode == 0, 'Plaintext conversion failed')
            require(sql(f"SELECT CONCAT(LEFT(crypt,3),':',LENGTH(clear)) FROM passwd WHERE passwd_id={account}") == '$6$:0', 'Plaintext conversion did not clear storage')
        finally:
            connection.close()
        request('DELETE', f'accounts/{migration_account}')
        migration_account = None
        if args.mail_protocols:
            with imaplib.IMAP4_SSL('127.0.0.1', 993, ssl_context=context) as imap:
                require(imap.login('owner@' + host, password)[0] == 'OK', 'Converted password no longer authenticates')
        supplied = 'supplied-test-password-123'
        request('PATCH', f'accounts/{account}', {'password': supplied, 'active': False})
        require(sql(f'SELECT active FROM passwd WHERE passwd_id={account}') == 'no', 'Disable was not immediate')
        if args.mail_protocols:
            denied = False
            with imaplib.IMAP4_SSL('127.0.0.1', 993, ssl_context=context) as imap:
                try:
                    imap.login('owner@' + host, supplied)
                except imaplib.IMAP4.error:
                    denied = True
            require(denied, 'Disabled account still authenticates')
        active()
        request('PATCH', f'organizations/{organization}', {'spam_enabled': True, 'spam_retention_days': 1})
        request('PATCH', f'accounts/{account}', {'active': True, 'spam_enabled': None, 'spam_folder': 'spam', 'spam_trash': 'Trash',
            'spam_retention_days': 1, 'auto_reply': 'Away for staging test.\n', 'keep_copy': True, 'forwards': ['copy@' + host]})
        active()
        filter_text = (home / '.mailfilter').read_text()
        require("cc '!copy@" in filter_text and '/usr/bin/mailbot' in filter_text and '/^X-Spam-Flag:' in filter_text, 'Filter features missing')
        require(request('GET', f'accounts/{account}')['account']['spam_enabled'] is None, 'Policy inheritance was lost')
        old_spam = home / 'Maildir/.spam/new/expired-staging-message'
        old_spam.write_text('Subject: old synthetic spam\n\nspam\n')
        os.chown(old_spam, home.stat().st_uid, home.stat().st_gid)
        os.utime(old_spam, (1, 1))
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and old_spam.exists():
            time.sleep(1)
        require(not old_spam.exists(), 'Scheduled spam cleanup did not move expired mail')
        require(any((home / 'Maildir/.Trash/new').iterdir()), 'Expired spam was lost instead of moved')
        if args.mail_protocols:
            for name in ['copy', 'sender']:
                auxiliary_accounts.append(request('POST', 'accounts', {'address': name + '@' + host}, expected=201)['account']['id'])
            active()
            copy_home = Path(sql(f'SELECT home FROM passwd WHERE passwd_id={auxiliary_accounts[0]}'))
            sender_home = Path(sql(f'SELECT home FROM passwd WHERE passwd_id={auxiliary_accounts[1]}'))
            before_copy = len(list((copy_home / 'Maildir/new').iterdir()))
            # Local injection avoids inventing public DNS for .test senders.
            message = f'From: sender@{host}\nTo: owner@{host}\nSubject: forwarding and auto-reply\n\nhello\n'.encode()
            delivered = subprocess.run(['/usr/sbin/sendmail', '-f', 'sender@' + host, 'owner@' + host], input=message,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            require(delivered.returncode == 0, 'Local mail injection failed')
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if len(list((copy_home / 'Maildir/new').iterdir())) > before_copy and list((sender_home / 'Maildir/new').iterdir()):
                    break
                time.sleep(1)
            require(len(list((copy_home / 'Maildir/new').iterdir())) > before_copy, 'Forwarding did not deliver a copy')
            require(bool(list((sender_home / 'Maildir/new').iterdir())), 'Auto-reply did not reach the synthetic sender')
            request('PATCH', f'accounts/{account}', {'keep_copy': False, 'auto_reply': ''})
            active()
            before_owner = len(list((home / 'Maildir/new').iterdir()))
            before_copy = len(list((copy_home / 'Maildir/new').iterdir()))
            delivered = subprocess.run(['/usr/sbin/sendmail', '-f', 'sender@' + host, 'owner@' + host], input=message,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            require(delivered.returncode == 0, 'Second local mail injection failed')
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline and len(list((copy_home / 'Maildir/new').iterdir())) == before_copy:
                time.sleep(1)
            require(len(list((copy_home / 'Maildir/new').iterdir())) > before_copy, 'Forward-only delivery failed')
            require(len(list((home / 'Maildir/new').iterdir())) == before_owner, 'Forward-only delivery retained a local copy')
            for identity in auxiliary_accounts:
                request('DELETE', f'accounts/{identity}')
            auxiliary_accounts.clear()
            active()
        # A failed publisher leaves a visible durable operation. Restarting the
        # daemon does not lose that generation; the root helper can retry it.
        subprocess.run(['systemctl', 'stop', 'mailadmin-publish.timer'], check=True)
        request('PATCH', f'accounts/{account}', {'name': 'Pending restart'})
        subprocess.run(['systemctl', 'restart', 'mailadmin'], check=True)
        time.sleep(2)
        require(request('GET', 'operations')['activation']['status'] == 'pending', 'Pending operation disappeared after restart')
        subprocess.run(['systemctl', 'start', 'mailadmin-publish.timer'], check=True)
        active()
        subprocess.run(['systemctl', 'stop', 'mailadmin-publish.timer', 'mailadmin-publish.service'], check=True)
        os.chmod(home.parent, 0o770)  # Force safe cleanup to refuse this directory.
        request('DELETE', f'accounts/{account}')
        request('POST', 'accounts', {'address': 'owner@' + host}, expected=409)
        require(sql(f'SELECT COUNT(*) FROM passwd WHERE passwd_id={account}') == '0', 'Delete was not immediate')
        request('GET', f'accounts/{account}', expected=404)
        request('GET', f'aliases/{alias}', expected=404)
        account = alias = None
        subprocess.run(['systemctl', 'start', 'mailadmin-publish.timer'], check=True)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            cleanup = request('GET', 'operations')['cleanup']
            if cleanup and cleanup[0]['attempts'] > 0:
                break
            time.sleep(1)
        require(cleanup and cleanup[0]['attempts'] > 0 and home.exists(), 'Failed cleanup was not journaled safely')
        subprocess.run(['systemctl', 'restart', 'mailadmin'], check=True)
        os.chmod(home.parent, 0o700)
        time.sleep(1)
        active()
        require(not home.exists(), 'Durable physical cleanup did not finish')
        request('DELETE', f'organizations/{organization}')
        organization = None
        active()
        logs = subprocess.check_output(['journalctl', '-u', 'mailadmin', '-u', 'courier-authdaemon', '--since', '-15 min', '--no-pager'], text=True)
        database_secret = json.loads(Path('/etc/mailadmin/config.json').read_text())['database_password']
        require(not any(value in logs for value in [token, password, supplied, database_secret]), 'A credential appeared in the journal')
        print('Staging business checks passed: CRUD, passwords, ownership, activation, filters, restart recovery, deletion' + (', SMTP and IMAP' if args.mail_protocols else ''))
    finally:
        subprocess.run(['systemctl', 'start', 'mailadmin-publish.timer'], check=False)
        if 'home' in locals() and home.parent.is_dir():
            os.chmod(home.parent, 0o700)
        for identity in auxiliary_accounts:
            try:
                request('DELETE', f'accounts/{identity}')
            except Exception:
                print('Synthetic auxiliary cleanup remains pending')
        for resource, identity in [('accounts', migration_account), ('accounts', account), ('organizations', organization)]:
            if identity is not None:
                try:
                    request('DELETE', f'{resource}/{identity}')
                except Exception:
                    print('Synthetic test cleanup remains pending; inspect staging records')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # HTTP bodies, SQL data and protocol exceptions may contain credentials.
        # This test only prints our fixed assertion text; other failures are opaque.
        if type(error) is RuntimeError:
            print(str(error))
        else:
            print('Staging business check failed; inspect server logs (credentials suppressed)')
        raise SystemExit(1)
