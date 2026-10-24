#!/usr/bin/env python3
"""Validate the privileged publisher's untrusted input boundary without root."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import hashlib
import json
import os
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('publisher', Path(__file__).parents[1] / 'deploy/host/mailadmin-publish.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublisherValidation(unittest.TestCase):
    def request(self):
        return {'generation': 3, 'domains': ['example.test'],
                'aliases': [{'address': 'alias@example.test', 'destination': 'owner@example.test'}]}

    def test_valid(self):
        hosts, aliases = publisher.validate(self.request())
        self.assertEqual(hosts, ['example.test'])
        self.assertEqual(aliases, [('alias@example.test', 'owner@example.test')])

    def test_injection_and_traversal(self):
        for address in ['|command@example.test', '-option@example.test', 'a\nb@example.test',
                        'a;command@example.test', '../file@example.test', 'x@../etc', 'a@EXAMPLE.test']:
            with self.subTest(address=address), self.assertRaises(ValueError):
                publisher.address(address)

    def test_unhosted_and_chain(self):
        request = self.request()
        request['aliases'][0]['destination'] = 'owner@other.test'
        with self.assertRaises(ValueError):
            publisher.validate(request)
        request = self.request()
        request['aliases'].append({'address': 'owner@example.test', 'destination': 'another@example.test'})
        with self.assertRaises(ValueError):
            publisher.validate(request)

    def test_generation_and_schema(self):
        for generation in [True, 0, -1, '3', 2**63]:
            request = self.request()
            request['generation'] = generation
            with self.assertRaises(ValueError):
                publisher.validate(request)
        request = self.request()
        request['command'] = 'arbitrary'
        with self.assertRaises(ValueError):
            publisher.validate(request)

    def check_publication(self, fail):
        with tempfile.TemporaryDirectory(prefix='mailadmin-publisher-') as temporary:
            directory = Path(temporary)
            payload = json.dumps(self.request()).encode()
            request = directory / 'request.json'
            request.write_bytes(payload)
            request.chmod(0o600)
            written = {}
            def capture(path, content, _gid):
                written[path] = content
            with patch.object(publisher, 'trusted_directory'), patch.object(publisher, 'atomic_write', side_effect=capture), \
                 patch.object(publisher.syslog, 'syslog'), patch.object(publisher.subprocess, 'run') as run:
                if fail:
                    run.side_effect = subprocess.CalledProcessError(1, ['/usr/sbin/makealiases'])
                publisher.publish({'generated_directory': str(directory), 'uid': os.getuid(),
                                   'gid': os.getgid(), 'admin_gid': os.getgid()})
            status = json.loads(written[directory / 'status.json'])
            self.assertEqual(status['digest'], hashlib.sha256(payload).hexdigest())
            self.assertEqual(status['success'], not fail)
            self.assertEqual(status['error'], 'courier_activation_failed' if fail else '')
            self.assertEqual(run.call_count, 1 if fail else 4)
            if not fail:
                self.assertEqual(run.call_args.args[0], ['/usr/bin/systemctl', 'try-restart', 'courier.service'])

    def test_rebuild_failure_is_not_acknowledged(self):
        self.check_publication(fail=True)

    def test_acknowledgement_after_all_commands(self):
        self.check_publication(fail=False)

    def test_duplicate_json(self):
        with self.assertRaises(ValueError):
            publisher.load_unique([('generation', 1), ('generation', 2)])


if __name__ == '__main__':
    unittest.main()
