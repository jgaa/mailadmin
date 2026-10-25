#!/usr/bin/env python3
"""Isolated staging DNS-worker checks using a private DB/container and mock nsblast.

Run as root on staging with the controller-built API image already loaded.
No requests from this test reach the real nsblast instance or mail database.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import pymysql

phase = "setup"

class CheckFailure(Exception):
    pass


def require(value, message):
    if not value:
        raise CheckFailure(message)


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise CheckFailure('Isolated container command failed')
    return result


def main():
    global phase
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='mailadmin:staging')
    parser.add_argument('--legacy-schema', required=True)
    args = parser.parse_args()
    suffix = ssl.RAND_bytes(5).hex()
    database = 'mailadmin_dns_' + suffix
    container = 'mailadmin-dns-' + suffix
    work = Path('/var/cache/mailadmin') / ('dns-check-' + suffix)
    work.mkdir(mode=0o700)
    password = ssl.RAND_bytes(32).hex()
    dns_key = ssl.RAND_bytes(32).hex()
    token = Path('/root/.config/mailadmin-staging/api-key').read_text().strip()
    root = pymysql.connect(unix_socket='/run/mysqld/mysqld.sock', user='root', autocommit=True)
    requests = []
    request_lock = threading.Lock()

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def serve(self):
            if self.headers.get('Authorization') != 'Bearer ' + dns_key:
                self.send_error(403)
                return
            body = b''
            if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
                while True:
                    length = int(self.rfile.readline().split(b';')[0], 16)
                    if not length:
                        self.rfile.readline()
                        break
                    body += self.rfile.read(length)
                    self.rfile.read(2)
                    require(len(body) <= 16384, 'Mock received oversized payload')
            else:
                body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            path = urllib.parse.urlsplit(self.path)
            parts = path.path.split('/')
            domain = parts[5]
            status_query = path.path.endswith('/status')
            with request_lock:
                requests.append((self.command, domain, body, path.query))
                count = sum(1 for method, name, *_ in requests if method == self.command and name == domain)
            status = 'complete' if status_query else 'deleted' if self.command == 'DELETE' else 'provisioned'
            code = 201 if self.command == 'POST' else 200
            value = {'domain': domain, 'status': status, 'warnings': []}
            if domain.startswith('ambiguous.') and self.command == 'POST':
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            if domain.startswith('retry.') and self.command == 'PUT' and count == 1:
                code = 503
            if domain.startswith('conflict.'):
                code = 409
            if domain.startswith('failed.'):
                value.update(status='failed', failure='Mock terminal transition failure')
            if domain.startswith(('pending.', 'superseded.', 'timeout.')) and self.command == 'POST':
                value.update(status='pending', transition='original', earliest_completion=int(time.time()) + 1)
                code = 200
            if domain.startswith('superseded.') and status_query:
                value.update(status='pending', transition='different', earliest_completion=int(time.time()) + 1)
            if domain.startswith('retire.') and self.command == 'DELETE':
                value.update(status='pending', transition='retirement', earliest_completion=int(time.time()) + 1)
            if domain.startswith('wrongack.') and self.command == 'DELETE':
                value['status'] = 'provisioned'
            response = json.dumps({'error': code >= 400, 'status': code, 'value': value}).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        do_GET = serve
        do_POST = serve
        do_PUT = serve
        do_DELETE = serve

    provider = http.server.ThreadingHTTPServer(('127.0.0.1', 18181), Provider)
    provider.daemon_threads = True
    provider_thread = threading.Thread(target=provider.serve_forever, daemon=True)
    provider_thread.start()

    def api(method, path, value=None, expected=200):
        request = urllib.request.Request('http://127.0.0.1:18082/api/v1/' + path,
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'},
            data=None if value is None else json.dumps(value).encode(), method=method)
        try:
            response = urllib.request.urlopen(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            require(response.status == expected, 'Unexpected isolated API status')
            return json.load(response)

    def sql(statement, parameters=()):
        with root.cursor() as cursor:
            cursor.execute(statement, parameters)
            return cursor.fetchall()

    def wait_state(domain, expected):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            try:
                state = api('GET', 'domains/' + domain + '/dns')['dns']
            except (urllib.error.URLError, ConnectionError):
                time.sleep(.25)
                continue
            if state['status'] == expected:
                return state
            time.sleep(.25)
        raise CheckFailure('DNS state did not converge to ' + expected + '; last state: ' + state['status'])

    def wake(domain):
        sql('UPDATE `' + database + '`.admin_dns SET next_attempt=0 WHERE domain=%s', (domain,))

    def add(label, configuration=None):
        domain = label + '.mailtest-cycle.test'
        value = {'domain': domain, 'organization_id': organization_id, 'provision_dns': False}
        if configuration:
            value.pop('provision_dns')
            value['dns'] = configuration
        api('POST', 'domains', value, 201)
        return domain

    desired = {'mail_servers': [{'hostname': 'mail.example.test', 'priority': 10}],
               'spf': {'enabled': True, 'policy': 'v=spf1 mx -all'},
               'dmarc': {'enabled': True, 'policy': 'none'}, 'dkim': [],
               'mta_sts': {'enabled': False}, 'tls_reporting': {'enabled': False}, 'ttl': 3600}
    try:
        sql('CREATE DATABASE `' + database + '`')
        sql("CREATE USER %s@'localhost' IDENTIFIED BY %s", (database, password))
        sql("GRANT SELECT,INSERT,UPDATE,DELETE ON `" + database + "`.* TO %s@'localhost'", (database,))
        schema = Path(args.legacy_schema).read_text().replace('{{ mailadmin_uid }}', '107').replace('{{ mailadmin_gid }}', '113')
        result = subprocess.run(['mariadb', '--database', database], input=schema, text=True, capture_output=True, timeout=30)
        require(result.returncode == 0, 'Isolated schema initialization failed')
        migration = work / 'migration.json'
        migration.write_text(json.dumps({'key_file': '/verifier', 'database_user': 'root', 'database_name': database}))
        migration.chmod(0o600)
        run('docker', 'run', '--rm', '--network=none', '--user=0:0', '--read-only', '--cap-drop=ALL',
            '--mount', f'type=bind,src={migration},dst=/migration.json,readonly',
            '--mount', 'type=bind,src=/run/mysqld,dst=/run/mysqld,readonly',
            args.image, 'schema', 'migrate', '--config', '/migration.json')
        key_file = work / 'dns.key'
        key_file.write_text(dns_key); key_file.chmod(0o640); os.chown(key_file, 0, 2107)
        for directory in ('spool', 'generated'):
            path = work / directory; path.mkdir(mode=0o700); os.chown(path, 107, 113)
        config = work / 'config.json'
        config.write_text(json.dumps({'key_file': '/verifier', 'database_user': database, 'database_name': database,
            'database_password': password, 'administration': True, 'port': '18082', 'spool_root': '/spool',
            'generated_directory': '/generated', 'nsblast_api_url': 'http://127.0.0.1:18181/api/v1',
            'nsblast_api_key_file': '/dns.key', 'default_mail_hostname': 'mail.example.test'}))
        config.chmod(0o640); os.chown(config, 0, 2107)
        run('docker', 'run', '-d', '--name', container, '--network=host', '--read-only', '--user=107:113',
            '--group-add=2107', '--cap-drop=ALL', '--security-opt=no-new-privileges',
            '--mount', f'type=bind,src={config},dst=/config.json,readonly',
            '--mount', f'type=bind,src={key_file},dst=/dns.key,readonly',
            '--mount', 'type=bind,src=/etc/mailadmin/credentials/api-key.sha256,dst=/verifier,readonly',
            '--mount', 'type=bind,src=/run/mysqld,dst=/run/mysqld,readonly',
            '--mount', f'type=bind,src={work}/spool,dst=/spool',
            '--mount', f'type=bind,src={work}/generated,dst=/generated', args.image, 'serve', '--config', '/config.json')
        deadline = time.monotonic() + 20
        while True:
            try:
                api('GET', 'health'); break
            except Exception:
                if time.monotonic() > deadline: raise RuntimeError('Isolated API did not start') from None
                time.sleep(.25)
        organization_id = api('POST', 'organizations', {'name': 'DNS worker checks', 'default_domain': 'worker.test', 'provision_dns': False}, 201)['organization']['id']
        phase = 'success'
        domain = add('success', desired)
        wait_state(domain, 'complete')
        before = api('GET', 'domains/' + domain + '/dns')['dns']['revision']
        api('PUT', 'domains/' + domain + '/dns', {}, 400)
        require(api('GET', 'domains/' + domain + '/dns')['dns']['revision'] == before, 'Invalid replacement changed the durable intent')
        phase = 'pending'
        domain = add('pending', desired)
        state = wait_state(domain, 'pending')
        require(state['transition'] == 'original' and isinstance(state['earliest_completion'], int), 'Pending metadata was not retained')
        require(state['managed'], 'Accepted pending intent did not retain ownership')
        changed = dict(desired, ttl=7200)
        api('POST', 'domains/' + domain + '/dns', changed, 409)
        require(api('GET', 'domains/' + domain + '/dns')['dns']['revision'] == state['revision'], 'A conflicting create lost pending-transition tracking')
        wake(domain); wait_state(domain, 'complete')
        require(sum(method == 'POST' and name == domain for method, name, *_ in requests) == 1, 'Status polling repeated a mutation')
        phase = 'ambiguous'
        domain = add('ambiguous', desired)
        wait_state(domain, 'unknown')
        run('docker', 'restart', '--time', '30', container)
        time.sleep(6)
        require(sum(method == 'POST' and name == domain for method, name, *_ in requests) == 1, 'A crashed/ambiguous create was replayed')
        api('DELETE', 'domains/' + domain, expected=409)
        require(not any(method == 'DELETE' and name == domain for method, name, *_ in requests), 'Unknown create triggered an automatic destructive cleanup')
        api('DELETE', 'domains/' + domain + '/dns')
        wait_state(domain, 'complete')
        api('DELETE', 'domains/' + domain)
        phase = 'retry'
        domain = add('retry')
        api('PUT', 'domains/' + domain + '/dns', desired)
        wait_state(domain, 'retry')
        run('docker', 'restart', '--time', '30', container)
        wake(domain); wait_state(domain, 'complete')
        sent = [body for method, name, body, _ in requests if method == 'PUT' and name == domain]
        require(len(sent) == 2 and sent[0] == sent[1], 'Retry did not preserve complete desired configuration')
        phase = 'failed'
        domain = add('failed')
        api('PUT', 'domains/' + domain + '/dns', desired)
        wait_state(domain, 'failed')
        count = len(requests); wake(domain); time.sleep(6)
        require(len(requests) == count, 'Terminal provider failure was retried')
        phase = 'superseded'
        domain = add('superseded', desired)
        wait_state(domain, 'pending'); wake(domain); wait_state(domain, 'superseded')
        phase = 'timeout'
        domain = add('timeout', desired)
        wait_state(domain, 'pending')
        sql('UPDATE `' + database + '`.admin_dns SET next_attempt=0,poll_deadline=1 WHERE domain=%s', (domain,))
        wait_state(domain, 'poll_timeout')
        phase = 'conflict'
        domain = add('conflict', desired)
        wait_state(domain, 'failed')
        require(not api('GET', 'domains/' + domain + '/dns')['dns']['managed'], 'A create conflict claimed ownership')
        api('DELETE', 'domains/' + domain)
        time.sleep(6)
        require(not any(method == 'DELETE' and name == domain for method, name, *_ in requests), 'Local deletion removed conflicting pre-existing DNS')
        phase = 'retire'
        domain = add('retire', desired)
        wait_state(domain, 'complete')
        api('DELETE', 'domains/' + domain)
        wait_state(domain, 'pending')
        api('POST', 'domains', {'domain': domain, 'organization_id': organization_id, 'provision_dns': False}, 409)
        wake(domain); wait_state(domain, 'complete')
        api('POST', 'domains', {'domain': domain, 'organization_id': organization_id, 'provision_dns': False}, 201)
        require(any(method == 'DELETE' and name == domain and query == 'preserve_ip=true' for method, name, _, query in requests), 'Removal did not preserve host addresses')
        phase = 'invalid deletion acknowledgement'
        domain = add('wrongack', desired)
        wait_state(domain, 'complete')
        api('DELETE', 'domains/' + domain)
        require(wait_state(domain, 'failed')['managed'], 'Invalid deletion acknowledgement relinquished DNS ownership')
        phase = 'deleted-domain repair'
        domain = 'success.mailtest-cycle.test'
        api('DELETE', 'domains/' + domain)
        wait_state(domain, 'complete')
        api('PUT', 'domains/' + domain + '/dns', desired)
        require(wait_state(domain, 'complete')['managed'], 'Deleted-domain DNS could not be explicitly repaired')
        api('DELETE', 'domains/' + domain + '/dns')
        wait_state(domain, 'complete')
        logs = run('docker', 'logs', container)
        for secret in (token, dns_key, password):
            require(secret not in logs.stdout + logs.stderr, 'Credential appeared in test-container logs')
        print('Isolated DNS worker checks passed: create, complete replacement, validation atomicity, pending polling, unknown create, restart/retry, terminal failure, supersession, polling deadline and deletion ordering')
    finally:
        subprocess.run(['docker', 'rm', '-f', container], capture_output=True, timeout=30)
        provider.shutdown(); provider.server_close(); provider_thread.join(timeout=5)
        sql('DROP DATABASE IF EXISTS `' + database + '`')
        sql("DROP USER IF EXISTS %s@'localhost'", (database,))
        root.close()
        shutil.rmtree(work)


if __name__ == '__main__':
    try:
        main()
    except CheckFailure as error:
        raise SystemExit('Isolated DNS worker check failed during ' + phase + ': ' + str(error)) from None
    except Exception:
        raise SystemExit('Isolated DNS worker check failed during ' + phase + '; raw exceptions and responses suppressed to protect credentials') from None
