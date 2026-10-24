#!/usr/bin/python3
"""Restricted root publisher. No arbitrary paths, shell commands or user secrets.

Called by a systemd timer. Constants are installed by Ansible in a root-owned
configuration file; the unprivileged daemon only writes request.json.
"""
import fcntl
import hashlib
import json
import os
import re
import stat
import subprocess
import syslog
import tempfile
from pathlib import Path

CONFIG = Path('/etc/mailadmin/publisher.json')
MAX_BYTES = 8 * 1024 * 1024


def load_unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate_field')
        result[key] = value
    return result


def domain(value):
    if not isinstance(value, str) or not 1 <= len(value) <= 253:
        raise ValueError('invalid_domain')
    if any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label)
           for label in value.split('.')):
        raise ValueError('invalid_domain')
    return value


def address(value):
    if not isinstance(value, str) or len(value) > 128 or value.count('@') != 1:
        raise ValueError('invalid_address')
    local, host = value.split('@')
    if not re.fullmatch(r'[a-z0-9_+][a-z0-9_.+=-]{0,63}', local) or local.endswith('.') or '..' in local:
        raise ValueError('invalid_address')
    domain(host)
    return value


def validate(value):
    if not isinstance(value, dict) or set(value) != {'generation', 'domains', 'aliases'}:
        raise ValueError('invalid_request')
    if type(value['generation']) is not int or not 0 < value['generation'] < 2**63:
        raise ValueError('invalid_generation')
    if not isinstance(value['domains'], list) or not isinstance(value['aliases'], list):
        raise ValueError('invalid_list')
    if len(value['domains']) > 10000 or len(value['aliases']) > 10000:
        raise ValueError('request_limit')
    hosts = [domain(item) for item in value['domains']]
    if len(set(hosts)) != len(hosts):
        raise ValueError('duplicate_domain')
    aliases = []
    seen = set()
    for item in value['aliases']:
        if not isinstance(item, dict) or set(item) != {'address', 'destination'}:
            raise ValueError('invalid_alias')
        source, destination = address(item['address']), address(item['destination'])
        if source in seen or source == destination or source.split('@')[1] not in hosts or destination.split('@')[1] not in hosts:
            raise ValueError('invalid_alias')
        seen.add(source)
        aliases.append((source, destination))
    if any(destination in seen for _, destination in aliases):
        raise ValueError('alias_chain')
    return hosts, aliases


def atomic_write(path, content, gid, mode=0o640):
    descriptor, temporary = tempfile.mkstemp(prefix='.mailadmin-', dir=path.parent)
    try:
        os.fchmod(descriptor, mode)
        os.fchown(descriptor, 0, gid)
        with os.fdopen(descriptor, 'wb') as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def trusted_directory(path, writable_group=False):
    for parent in [path, *path.parents]:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o002:
            raise ValueError('unsafe_directory')
        if info.st_mode & 0o020 and not (parent == path and writable_group):
            raise ValueError('unsafe_directory')


def publish(config):
    generated = Path(config['generated_directory'])
    trusted_directory(generated, writable_group=True)
    request = generated / 'request.json'
    try:
        descriptor = os.open(request, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return
    with os.fdopen(descriptor, 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != config['uid'] or info.st_gid != config['admin_gid'] or info.st_mode & 0o022 or info.st_size > MAX_BYTES:
            raise ValueError('unsafe_request')
        payload = source.read(MAX_BYTES + 1)
    if len(payload) > MAX_BYTES:
        raise ValueError('request_limit')
    digest = hashlib.sha256(payload).hexdigest()
    status_path = generated / 'status.json'
    # A status file is only a cache if it remains root-owned and read-only to
    # the administration group. Never trust an acknowledgement supplied by it.
    try:
        descriptor = os.open(status_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(descriptor, 'rb') as source:
            info = os.fstat(source.fileno())
            previous = json.loads(source.read(8193)) if stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022 and info.st_size <= 8192 else {}
        if previous.get('digest') == digest and previous.get('success') is True:
            return
    except (FileNotFoundError, ValueError, OSError):
        pass
    status = {'digest': digest, 'success': False, 'error': 'courier_activation_failed'}
    try:
        value = json.loads(payload, object_pairs_hook=load_unique)
        hosts, aliases = validate(value)
        status['generation'] = value['generation']
        files = {
            Path('/etc/courier/aliases/mailadm_generated'): ''.join(f'{source}: {target}\n' for source, target in aliases),
            Path('/etc/courier/hosteddomains/mailadm_generated'): ''.join(f'{host}\n' for host in hosts),
            Path('/etc/courier/esmtpacceptmailfor.dir/mailadm_generated'): ''.join(f'{host}\n' for host in hosts),
        }
        for path, text in files.items():
            trusted_directory(path.parent)
            atomic_write(path, text.encode('ascii'), config['gid'])
        for command in ['/usr/sbin/makealiases', '/usr/sbin/makehosteddomains', '/usr/sbin/makeacceptmailfor']:
            subprocess.run([command], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True, timeout=30,
                           env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C'})
        # courierd keeps routing data in its long-running queue process. A
        # database rebuild alone does not invalidate that process's cache.
        # Restart only this fixed core unit. IMAP is independent; Debian may
        # briefly restart dependent SMTP listeners along with the queue daemon.
        subprocess.run(['/usr/bin/systemctl', 'try-restart', 'courier.service'],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True, timeout=30,
                       env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C'})
        status.update(success=True, error='')
        syslog.syslog(syslog.LOG_INFO, f"mailadmin activated generation {value['generation']}")
    except (ValueError, OSError, subprocess.SubprocessError):
        syslog.syslog(syslog.LOG_ERR, 'mailadmin Courier activation failed; check managed sources and rebuild commands')
    atomic_write(status_path, (json.dumps(status) + '\n').encode(), config['admin_gid'])


def main():
    if os.geteuid() != 0:
        raise SystemExit('publisher requires root')
    trusted_directory(CONFIG.parent)
    descriptor = os.open(CONFIG, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor) as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022 or info.st_size > 4096:
            raise ValueError('unsafe_configuration')
        config = json.load(source)
    descriptor = os.open('/run/lock/mailadmin-publish.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        publish(config)
    finally:
        os.close(descriptor)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError):
        syslog.syslog(syslog.LOG_ERR, 'mailadmin publisher failed; check protected configuration and permissions')
        raise SystemExit(1)
