#!/usr/bin/env python3
"""Read-only browser checks against the staging UI and its live API.

Run on the controller with Playwright/Chromium installed. The API key is read
over SSH into memory, never put in arguments, screenshots, traces or test logs.
"""

import argparse
import json
import re
import subprocess
import sys
import time

from playwright.sync_api import expect, sync_playwright

stage = 'fixture'


class CheckFailure(Exception):
    pass


def fixture(host):
    script = r'''
import json
from pathlib import Path
import urllib.request
token = Path('/root/.config/mailadmin-staging/api-key').read_text().strip()
def get(path):
    request = urllib.request.Request('http://127.0.0.1:8080/api/v1/' + path,
        headers={'Authorization': 'Bearer ' + token})
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.load(response)
data = {name: get(name)[name] for name in ('organizations', 'domains', 'accounts', 'aliases')}
data['token'] = token
data['ui_username'] = json.loads(Path('/var/lib/mailadmin-ui/admin.json').read_text())['username']
data['ui_password'] = Path('/var/lib/mailadmin-ui/initial-password').read_text().strip()
print(json.dumps(data))
'''
    reply = subprocess.run(['ssh', host, 'python3 -'], input=script, text=True,
                           capture_output=True, timeout=30, check=True)
    return json.loads(reply.stdout)


def require(condition, description):
    if not condition:
        raise CheckFailure(description)


def ready(page, expected, javascript):
    status = page.locator('.status')
    if javascript:
        # A click returns before Wt applies its Ajax response. Observe the
        # new busy state before accepting a status left by the previous load.
        expect(page.get_by_role('button', name='Check for results', exact=True)).to_be_enabled(timeout=10000)
        status.filter(has_text=expected).wait_for(timeout=45000)
        return
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if expected.search(status.inner_text()):
            return
        check = page.get_by_role('button', name='Check for results', exact=True)
        if check.is_enabled():
            check.click()
        page.wait_for_timeout(400)
    raise CheckFailure('Plain HTML request did not complete')


def session(browser, url, data, javascript, host):
    global stage
    context = browser.new_context(java_script_enabled=javascript)
    page = context.new_page()
    page.set_default_timeout(15000)
    page_errors = []
    page.on('pageerror', lambda _error: page_errors.append(True))
    try:
        stage = 'bootstrap'
        response = page.goto(url, wait_until='domcontentloaded')
        require(response is not None and response.status == 200, 'UI bootstrap failed')
        require('no-store' in response.headers.get('cache-control', ''), 'Missing no-store header')
        stage = 'login'
        require(not page.get_by_role('button', name='New organization', exact=True).is_visible(), 'Administration visible before sign-in')
        require(data['accounts'][0]['address'] not in page.content(), 'Mailbox visible before sign-in')
        page.get_by_label('Username', exact=True).fill(data['ui_username'])
        page.get_by_label('Password', exact=True).fill(data['ui_password'])
        page.get_by_role('button', name='Sign in', exact=True).click()
        ready(page, re.compile(r'^Loaded \d+ organizations'), javascript)
        require(data['token'] not in page.content(), 'Credential was returned in UI markup')
        stage = 'tree'
        navigation = page.locator('.navigation')
        for organization in data['organizations']:
            require(navigation.get_by_text(organization['name'], exact=True).count() == 1,
                    'Organization missing from tree')
        for domain in data['domains']:
            require(navigation.get_by_text(domain['domain'], exact=True).count() == 1,
                    'Domain missing from tree')
        require(data['accounts'], 'No synthetic staging accounts available for UI checks')
        for account in data['accounts']:
            stage = 'mailbox selection'
            navigation.get_by_text(account['address'], exact=True).click()
            ready(page, re.compile(r'^Current API configuration loaded'), javascript)
            details = page.locator('.details')
            require(details.get_by_text(account['address'], exact=True).count() == 1,
                    'Selected mailbox heading is missing')
            require('Login currently enabled' in details.inner_text(), 'Mailbox settings not displayed')
            require('Effective setting' in details.inner_text(), 'Inherited policy table not displayed')
            require('Inherit from organization' in details.inner_text(), 'Policy inheritance not displayed')
            for alias in data['aliases']:
                visible = details.get_by_text(alias['address'], exact=True).count() > 0
                require(visible == (alias['account_id'] == account['id']), 'Alias isolation is incorrect')
            require(data['token'] not in page.content(), 'Credential was returned with mailbox data')
        stage = 'mailbox refresh'
        page.get_by_role('button', name='Refresh mailbox', exact=True).click()
        ready(page, re.compile(r'^Current API configuration loaded'), javascript)
        stage = 'tree refresh'
        page.get_by_role('button', name='Refresh tree', exact=True).click()
        ready(page, re.compile(r'^Loaded \d+ organizations'), javascript)
        require('Login currently enabled' not in page.locator('.details').inner_text(), 'Refresh kept stale mailbox data')
        stage = 'sign out'
        page.get_by_role('button', name='Sign out', exact=True).click()
        page.locator('.status').filter(has_text=re.compile(r'^Signed out\.')).wait_for()
        expect(page.get_by_label('Password', exact=True)).to_be_visible()
        require('Login currently enabled' not in page.content(), 'Sign out kept mailbox data')
        require(not navigation.get_by_text(data['accounts'][0]['address'], exact=True).count(), 'Sign out kept tree records')
        stage = 'rejected password'
        page.get_by_label('Password', exact=True).fill(data['ui_password'] + 'wrong')
        page.get_by_role('button', name='Sign in', exact=True).click()
        page.locator('.status').filter(has_text=re.compile(r'^Username or password incorrect')).wait_for()
        require(page.get_by_label('Password', exact=True).is_visible(), 'Rejected password did not return to login')
        require(data['ui_password'] not in page.content(), 'Password returned in UI markup')
        require(not page.get_by_role('button', name='New organization', exact=True).is_visible(), 'Administration visible after rejected sign-in')
        stage = 'password rotation'
        page.get_by_label('Password', exact=True).fill(data['ui_password'])
        page.get_by_role('button', name='Sign in', exact=True).click()
        ready(page, re.compile(r'^Loaded \d+ organizations'), javascript)
        # Re-hash the same bootstrap password: session revocation without changing
        # the staging operator's password or exposing it to command arguments.
        subprocess.run(['ssh', host, 'docker exec mailadmin-ui /usr/local/bin/mailadmin_ui password set --password-file /var/lib/mailadmin-ui/initial-password'],
                       capture_output=True, text=True, timeout=30, check=True)
        page.get_by_role('button', name='Refresh tree', exact=True).click()
        page.locator('.status').filter(has_text=re.compile(r'^Administrator password changed')).wait_for()
        expect(page.get_by_label('Password', exact=True)).to_be_visible()
        require(data['accounts'][0]['address'] not in page.content(), 'Rotation kept mailbox records')
        require(not page_errors, 'Browser JavaScript errors occurred')
    finally:
        context.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://192.168.1.199:8081')
    parser.add_argument('--ssh-host', default='staging')
    args = parser.parse_args()
    data = fixture(args.ssh_host)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        try:
            for javascript in (True, False):
                session(browser, args.url, data, javascript, args.ssh_host)
                print('Staging UI browser checks passed: JavaScript ' + ('enabled' if javascript else 'disabled'))
        finally:
            browser.close()


if __name__ == '__main__':
    try:
        main()
    except CheckFailure as error:
        print('Staging UI check failed during ' + stage + ': ' + str(error), file=sys.stderr)
        sys.exit(1)
    except Exception:
        # Browser call logs and SSH stdout can contain a credential. Never
        # print the raw exception or enable Playwright tracing for this test.
        print('Staging UI browser check failed during ' + stage + '; inspect the service and UI without logging credentials', file=sys.stderr)
        sys.exit(1)
