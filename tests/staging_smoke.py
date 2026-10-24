#!/usr/bin/env python3
"""Read-only wire checks against a running staging daemon. Never prints keys."""
import argparse
import http.client
import json
from contextlib import closing
from pathlib import Path
import urllib.error
import urllib.request
import urllib.parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8080')
    parser.add_argument('--token-file', required=True)
    parser.add_argument('--staging-fixture', action='store_true')
    args = parser.parse_args()
    token = Path(args.token_file).read_text().strip()
    if not token.startswith('ma_') or len(token) != 67:
        raise SystemExit('Invalid API key file')

    def request(path, expected, credential=None, method='GET'):
        headers = {'Authorization': 'Bearer ' + credential} if credential else {}
        req = urllib.request.Request(args.url.rstrip('/') + path, headers=headers, method=method)
        # Do not forward credentials through redirects to a different origin.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *_args, **_kwargs):
                return None
        opener = urllib.request.build_opener(NoRedirect)
        try:
            response = opener.open(req, timeout=20)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            body = response.read().decode('utf-8')
            if response.code != expected:
                raise RuntimeError(f'Unexpected status for {path}: {response.code}')
            if token in body:
                raise RuntimeError('Credential appeared in response')
            if not response.headers.get('X-Request-ID'):
                raise RuntimeError('Missing request correlation ID')
            return body

    request('/api/v1/health', 401)
    request('/api/v1/health', 401, 'ma_' + '0' * 64)
    parsed = urllib.parse.urlsplit(args.url)
    connection_type = http.client.HTTPSConnection if parsed.scheme == 'https' else http.client.HTTPConnection
    with closing(connection_type(parsed.hostname, parsed.port, timeout=20)) as connection:
        connection.putrequest('GET', '/api/v1/health')
        connection.putheader('Authorization', 'Bearer ' + token)
        connection.putheader('Authorization', 'Bearer ' + token)
        connection.endheaders()
        response = connection.getresponse()
        response.read()
        if response.status != 401:
            raise RuntimeError('Duplicate Authorization headers were accepted')
    health = json.loads(request('/api/v1/health', 200, token))
    if health.get('status') != 'ready':
        raise RuntimeError('Database readiness check failed')
    request('/api/v1/health', 405, token, method='POST')
    request('/api/v1/missing', 404, token)
    json.loads(request('/api/v1/organizations', 200, token))
    domains = json.loads(request('/api/v1/domains', 200, token))['domains']
    metrics = request('/metrics', 200, token)
    if not metrics.endswith('# EOF\n'):
        raise RuntimeError('Invalid OpenMetrics termination')
    for name in ('mailadmin_email_accounts', 'mailadmin_aliases'):
        if name not in metrics:
            raise RuntimeError('Missing required metric family')
    if args.staging_fixture:
        expected = {'example.test': (1, 1, 1), 'second.example.test': (1, 1, 0), 'empty.test': (2, 0, 0)}
        actual = {item['domain']: item for item in domains}
        for name, (owner, accounts, aliases) in expected.items():
            item = actual[name]
            if (item['organization_id'], item['email_accounts'], item['aliases']) != (owner, accounts, aliases):
                raise RuntimeError('Incorrect synthetic fixture ownership/counts')
            samples = {line.split()[0]: float(line.split()[1]) for line in metrics.splitlines()
                       if line and not line.startswith('#')}
            for metric, count in [('mailadmin_email_accounts', accounts), ('mailadmin_aliases', aliases)]:
                if samples.get(metric + '{domain="' + name + '"}') != count:
                    raise RuntimeError('Incorrect domain label or metric count')
    print('Staging HTTP, authentication, inventory and metric checks passed')


if __name__ == '__main__':
    main()
