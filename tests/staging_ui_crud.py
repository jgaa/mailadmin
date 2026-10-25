#!/usr/bin/env python3
"""Exercise classic Wt Save/Cancel CRUD with both JavaScript modes on staging.

Creates and removes only its own synthetic names below mailtest-cycle.test.
Uses the authorized staging DNS provider; dedicated test host addresses are
retained by the service's preserve-IP deletion policy.
"""
import argparse
import json
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from playwright.sync_api import expect, sync_playwright
from staging_ui import fixture

phase = 'setup'


def require(value):
    if not value:
        raise RuntimeError('CRUD assertion failed')


def main():
    global phase
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://192.168.1.199:8081')
    parser.add_argument('--ssh-host', default='staging')
    parser.add_argument('--mail-ip', default='192.168.1.199')
    args = parser.parse_args()
    data = fixture(args.ssh_host)
    token = data['token']
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    tunnel = subprocess.Popen(['ssh', '-N', '-o', 'ExitOnForwardFailure=yes', '-L', f'127.0.0.1:{port}:127.0.0.1:8080', args.ssh_host],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def api(method, path, value=None):
        request = urllib.request.Request(f'http://127.0.0.1:{port}/api/v1/' + path, method=method,
            data=None if value is None else json.dumps(value).encode(),
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=15) as response:
            return json.load(response)

    def wait_dns(domain):
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            value = api('GET', 'domains/' + domain + '/dns')['dns']
            if value['status'] == 'complete':
                return value
            require(value['status'] not in ('failed', 'unknown', 'superseded'))
            time.sleep(.25)
        raise RuntimeError('DNS did not converge')

    def idle(page, javascript):
        button = page.get_by_role('button', name='New organization', exact=True)
        if javascript:
            expect(button).to_be_enabled(timeout=45000)
        else:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if button.is_enabled():
                    return
                check = page.get_by_role('button', name='Check for results', exact=True)
                if check.is_enabled():
                    try: check.click(timeout=1000)
                    except Exception: pass
                page.wait_for_timeout(200)
            raise RuntimeError('Plain HTML save did not finish')

    def press(page, name):
        page.get_by_role('button', name=name, exact=True).click()

    def form(page, name):
        press(page, name)
        page.get_by_role('button', name='Save', exact=True).wait_for()

    def save(page, javascript):
        press(page, 'Save')
        idle(page, javascript)
        require(page.get_by_role('button', name='Save', exact=True).count() == 0)

    def select(page, title):
        page.locator('.navigation').get_by_text(title, exact=True).click()
        expect(page.locator('.details .heading')).to_have_text(title, timeout=45000)

    def delete(page, name, javascript):
        press(page, name)
        page.get_by_role('button', name='Delete', exact=True).wait_for()
        press(page, 'Delete')
        idle(page, javascript)

    def mailbox(page, address, javascript):
        form(page, 'New mailbox')
        page.get_by_label('Email address', exact=True).fill(address)
        page.get_by_label('Display name', exact=True).fill('UI mailbox')
        page.get_by_label('Quota (bytes S, messages C; empty means unlimited)', exact=True).fill('1000000S')
        save(page, javascript)
        require('Generated password' in page.locator('.notice').inner_text())
        require(token not in page.content())
        press(page, 'Dismiss password')

    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                api('GET', 'health'); break
            except Exception:
                if time.monotonic() > deadline: raise RuntimeError('API tunnel did not start') from None
                time.sleep(.1)
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                for javascript in (True, False):
                    tag = 'crud' + hex(time.time_ns())[-8:]
                    title = 'UI ' + tag
                    base = tag + '.mailtest-cycle.test'
                    extra = 'extra-' + base
                    dedicated = 'host-' + base
                    domains = {base, extra, dedicated}
                    context = browser.new_context(java_script_enabled=javascript)
                    page = context.new_page()
                    page.set_default_timeout(15000)
                    try:
                        phase = 'login'
                        page.goto(args.url)
                        page.get_by_label('Username', exact=True).fill(data['ui_username'])
                        page.get_by_label('Password', exact=True).fill(data['ui_password'])
                        press(page, 'Sign in')
                        if javascript:
                            page.locator('.status').filter(has_text=re.compile(r'^Loaded')).wait_for(timeout=45000)
                        else:
                            idle(page, False)
                        phase = 'organization cancel'
                        form(page, 'New organization')
                        page.get_by_label('Organization name', exact=True).fill(title + ' cancelled')
                        press(page, 'Cancel')
                        idle(page, javascript)
                        require(not any(org['name'] == title + ' cancelled' for org in api('GET', 'organizations')['organizations']))
                        phase = 'organization create and update'
                        form(page, 'New organization')
                        page.get_by_label('Organization name', exact=True).fill(title)
                        page.get_by_label('Default domain', exact=True).fill(base)
                        save(page, javascript)
                        form(page, 'Edit organization')
                        title += ' edited'
                        page.get_by_label('Organization name', exact=True).fill(title)
                        save(page, javascript)
                        organization = next(org for org in api('GET', 'organizations')['organizations'] if org['name'] == title)
                        phase = 'domain create and owner update'
                        form(page, 'New domain')
                        page.get_by_label('Domain name', exact=True).fill(extra)
                        save(page, javascript)
                        form(page, 'Edit domain')
                        page.get_by_label('Owning organization', exact=True).select_option(label='Empty staging organization')
                        save(page, javascript)
                        require(api('GET', 'domains/' + extra)['domain']['organization_id'] == 2)
                        form(page, 'Edit domain')
                        page.get_by_label('Owning organization', exact=True).select_option(label=title)
                        save(page, javascript)
                        phase = 'mailbox create, generated password and basic properties'
                        first = 'person@' + extra
                        second = 'second@' + extra
                        mailbox(page, first, javascript)
                        form(page, 'Edit mailbox')
                        page.get_by_label('Display name', exact=True).fill('Changed display name')
                        page.get_by_label('Enabled', exact=True).select_option(label='No')
                        page.get_by_label('Auto-reply (empty disables)', exact=True).fill('Back tomorrow')
                        save(page, javascript)
                        account = next(item for item in api('GET', 'accounts')['accounts'] if item['address'] == first)
                        require(account['name'] == 'Changed display name' and not account['desired_active'] and account['auto_reply'] == 'Back tomorrow')
                        phase = 'forwarding create, update and delete'
                        for destinations in ('one@example.test\ntwo@second.example.test', 'one@example.test', ''):
                            form(page, 'Edit forwarding')
                            page.get_by_label('Forwarding destinations (one per line)', exact=True).fill(destinations)
                            page.get_by_label('Keep a local copy when forwarding', exact=True).select_option(label='Yes')
                            save(page, javascript)
                            current = api('GET', 'accounts/' + str(account['id']))['account']
                            require(current['forwards'] == sorted(destinations.splitlines()))
                        select(page, extra)
                        mailbox(page, second, javascript)
                        second_account = next(item for item in api('GET', 'accounts')['accounts'] if item['address'] == second)
                        phase = 'alias create, rename, retarget and delete'
                        form(page, 'Add alias')
                        alias = 'alias@' + extra
                        page.get_by_label('Alias address', exact=True).fill(alias)
                        page.get_by_label('Deliver to mailbox', exact=True).select_option(label=first)
                        save(page, javascript)
                        form(page, 'Edit alias')
                        alias = 'renamed@' + extra
                        page.get_by_label('Alias address', exact=True).fill(alias)
                        page.get_by_label('Deliver to mailbox', exact=True).select_option(label=second)
                        save(page, javascript)
                        record = next(item for item in api('GET', 'aliases')['aliases'] if item['address'] == alias)
                        require(record['account_id'] == second_account['id'])
                        delete(page, 'Delete alias', javascript)
                        require(not any(item['address'] == alias for item in api('GET', 'aliases')['aliases']))
                        phase = 'dedicated mail host provisioning'
                        select(page, title)
                        form(page, 'New domain')
                        page.get_by_label('Domain name', exact=True).fill(dedicated)
                        page.get_by_label('Mail DNS provisioning', exact=True).select_option(label='Provision a dedicated mail host')
                        page.get_by_label('Dedicated mail hostname', exact=True).fill('mx.' + dedicated)
                        page.get_by_label('Dedicated host IPv4 addresses (one per line)', exact=True).fill(args.mail_ip)
                        save(page, javascript)
                        dns = wait_dns(dedicated)
                        require(dns['desired']['mail_servers'][0]['ipv4'] == [args.mail_ip])
                        press(page, 'DNS settings')
                        expect(page.locator('.details .heading')).to_have_text('DNS for ' + dedicated, timeout=45000)
                        form(page, 'Configure DNS')
                        config = dns['desired']
                        config['dmarc']['policy'] = 'quarantine'
                        page.get_by_label('Complete DNS configuration (JSON)', exact=True).fill(json.dumps(config))
                        save(page, javascript)
                        dns = wait_dns(dedicated)
                        require(dns['desired']['dmarc']['policy'] == 'quarantine')
                        phase = 'mailbox, domain and organization deletion'
                        select(page, second)
                        delete(page, 'Delete mailbox', javascript)
                        select(page, first)
                        delete(page, 'Delete mailbox', javascript)
                        select(page, extra)
                        delete(page, 'Delete domain', javascript)
                        select(page, dedicated)
                        delete(page, 'Delete domain', javascript)
                        select(page, title)
                        delete(page, 'Delete organization', javascript)
                        require(not any(org['id'] == organization['id'] for org in api('GET', 'organizations')['organizations']))
                        for domain in domains:
                            wait_dns(domain)
                        print('Staging Save/Cancel CRUD and live nsblast checks passed: JavaScript ' + ('enabled' if javascript else 'disabled'))
                    finally:
                        context.close()
                        # Remove only this cycle's synthetic records, including
                        # a domain temporarily assigned to another organization.
                        for alias in api('GET', 'aliases')['aliases']:
                            if alias['address'].split('@')[-1] in domains:
                                api('DELETE', 'aliases/' + str(alias['id']))
                        for account in api('GET', 'accounts')['accounts']:
                            if account['address'].split('@')[-1] in domains:
                                api('DELETE', 'accounts/' + str(account['id']))
                        for org in api('GET', 'organizations')['organizations']:
                            if org['name'].startswith('UI ' + tag):
                                api('DELETE', 'organizations/' + str(org['id']))
                        for domain in api('GET', 'domains')['domains']:
                            if domain['domain'] in domains:
                                api('DELETE', 'domains/' + domain['domain'])
            finally:
                browser.close()
    finally:
        tunnel.terminate()
        tunnel.wait(timeout=10)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        print('Staging UI CRUD check failed during ' + phase + '; raw browser and API diagnostics suppressed', file=sys.stderr)
        sys.exit(1)
