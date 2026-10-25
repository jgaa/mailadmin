# Experimental Wt UI

`mailadmin_ui` is a separate C++20 administration application. It uses Wt's built-in
HTTP connector and RESTinCurl's Asio coroutine interface. The UI has no database,
Courier configuration or mailbox filesystem access, and consumes the administrator
API using GET, POST, PATCH, PUT and DELETE.
Editors use ordinary inputs with explicit Save and Cancel; changes remain local
until Save. Validation failures preserve the form. Ambiguous save outcomes
disable another save until Cancel reloads the current record. DNS operations are
provided by the administration API; there is no browser-facing provider credential
or MCP interface.

The tree contains organizations, their domains (including empty domains), and
primary email accounts. Selecting an email account fetches its current account
record, its organization's current defaults, and its aliases. The details panel
shows enabled and desired enabled states, quota, forwarding, keep-copy, auto-reply,
mail servers, account policy overrides and effective inherited policy values.
Aliases can belong to another domain in the same organization and appear in the
mailbox's alias list. Passwords are not exposed by the account API.

## Build inputs

CMake fetches Wt from `https://github.com/emweb/wt` and RESTinCurl from
`https://github.com/jgaa/RESTinCurl`, using shallow default-branch checkouts.
Local sibling checkouts are documentation references only. Wt is compiled from
source. RESTinCurl is header-only and compiled into the UI application; it uses
the system libcurl. Logfault is also fetched from GitHub. No preinstalled Wt or
RESTinCurl is needed.

In addition to the root project's C++20 compiler/library and CMake requirements,
install system development packages for Boost JSON, Program_options, Thread and
Filesystem, OpenSSL, libcurl and zlib. On Debian 13 these include
`libboost-json-dev libboost-program-options-dev libboost-thread-dev
libboost-filesystem-dev libssl-dev libcurl4-openssl-dev zlib1g-dev`.
Wt's examples, tests, database backends, Qt bridges, PDF and graphics integrations
are disabled. The UI can be configured independently of the API and MariaDB:

```sh
cmake -S . -B build-ui -DCMAKE_BUILD_TYPE=Debug \
  -DMAILADMIN_BUILD_API=OFF -DMAILADMIN_BUILD_UI=ON -DMAILADMIN_BUILD_TESTS=ON
cmake --build build-ui --target mailadmin_ui mailadmin_ui_data_test \
  mailadmin_ui_credentials_test mailadmin_ui_authentication_test --parallel 2
ctest --test-dir build-ui --output-on-failure
./build-ui/ui-server/src/mailadmin_ui --api-url http://127.0.0.1:8080 \
  --api-key-file /private/api-key --credentials-file /private/ui-admin.json \
  --initial-password-file /private/ui-initial-password
```

Configure the UI API credential and its persistent authentication paths as below.
Open `http://127.0.0.1:8081` and sign in as `admin` with the UI password. The API must have
administration enabled and its schema migrated. Inventory-only mode cannot serve
account details. Use `--help` for port, resource and Wt configuration overrides.
Build-tree defaults point to an empty public document root, the fetched Wt
resources and the generated copy of `ui-server/wt_config.xml`.

For an installation, supply `--wt-config <prefix>/share/mailadmin-ui/wt_config.xml`
and `--resources-dir <prefix>/share/mailadmin-ui/resources`, and point `--docroot`
at a separate empty public directory. Never use a source tree or a directory
containing credentials/configuration as a document root. The existing Dockerfile
remains the API image and explicitly disables the UI build. `Dockerfile.ui`
builds a separate Debian 13 UI image, compiles Wt, executes the UI contract test,
and copies only the application, Wt shared libraries/resources and configuration
into its runtime stage.

Build the image on the controller and deploy the exported archive; no compiler
or source checkout is required on staging:

```sh
./deploy/build-image.sh mailadmin-ui:staging \
  "$HOME/.local/state/mailadmin/images/ui-staging.tar" Dockerfile.ui
ansible-playbook -i deploy/ansible/inventory.staging.yml \
  deploy/ansible/deploy-ui.yml --limit staging
```

The playbook installs `mailadmin-ui.service`, loads the local image and restarts
only the UI when its image or unit changes. The container runs as UID/GID
65532:65532 with all capabilities dropped, a read-only root filesystem, bounded
memory/process counts and a temporary `/tmp`. Its mounts contain a read-only API
key and writable UI authentication state; it has no database/Courier credentials. Host networking lets it consume the existing API
on loopback without changing the API listener or restarting the mail service.

## Sessions and network access

The listener defaults to numeric loopback, with an explicit `--http-address`
override for deployment. Staging uses HTTP on `0.0.0.0:8081`, available at
`http://192.168.1.199:8081`, and consumes `http://127.0.0.1:8080`. Sign in with the separate UI administrator password. The API key is configured
on the server and is not baked into the image or unit.
HTTP is the explicitly selected staging transport. Remote HTTPS API origins
remain supported, but are not used by this deployment.

Alternatively, a local UI can inspect the staging API through an SSH tunnel:

```sh
ssh -N -L 18080:127.0.0.1:8080 staging
# In another terminal:
./build-ui/ui-server/src/mailadmin_ui --api-url http://127.0.0.1:18080
```

The API origin is fixed at process startup. HTTPS uses normal certificate/host
verification; plaintext HTTP is allowed only for numeric loopback origins.
Redirects and environment proxy settings are disabled. The server holds its API
key in memory and includes it only in outgoing API authorization headers. Neither
API keys nor UI passwords are embedded in responses, URLs, JavaScript storage or
logs. Sign out clears displayed records; pending requests retain credentials only
until they finish (each request has a 10-second timeout). Idle sessions expire
after 15 minutes. Session IDs rotate at sign-in and sign-out; password changes
invalidate existing sessions on their next action.

### Server API credential

Set exactly one of `MAILADMIN_API_KEY`, `MAILADMIN_API_KEY_FILE`, or
`--api-key-file PATH`. An explicit file option overrides the file environment
variable. A literal environment value and a file source together are rejected.
Files must be regular, not symlinks, owned by root or the service UID, without
access for others or group writes (0600 or 0640, for example). Trailing CR/LF is
removed. Startup fails with a sanitized error if the key is missing or invalid.
The literal environment variable is removed after loading. Restart the UI after
changing its API key. API 401 errors identify a rejected server credential.

Ansible accepts `mailadmin_ui_api_key_source` (a protected path on the server), or
`vault_mailadmin_ui_api_key`. It copies the key to
`/etc/mailadmin-ui/credentials/api-key`, root:65532 mode 0440, mounted read-only.
Staging uses the existing root-only staging key as the deployment source.

### UI administrator password

The default username is `admin`. On first startup, if no hash exists, OpenSSL
creates a 256-bit random password, writes it to
`/var/lib/mailadmin-ui/initial-password` with mode **0600**, and logs an INFO
message containing only this path. The salted hash is stored separately in
`/var/lib/mailadmin-ui/admin.json` (0600). The directory is persistent, owned by
UID/GID 65532, and mode 0700. Restarting does not reset the password. Interrupted
bootstrap reuses the protected initial-password file before saving the hash.

To read the initial password privately on staging:

```sh
ssh staging 'sudo cat /var/lib/mailadmin-ui/initial-password'
```

Set or rotate the password using the same executable, without an API key:

```sh
mailadmin_ui password set --password 'YOUR_NEW_PASSWORD' --credentials-file /private/admin.json
mailadmin_ui password set --password-file /private/new-password --credentials-file /private/admin.json
# Installed staging container; input file must be readable by UID 65532 and mode 0600:
ssh staging 'docker exec mailadmin-ui /usr/local/bin/mailadmin_ui password set --password-file /var/lib/mailadmin-ui/new-password'
```

The password command saves only the hash and never prints the password. File input
avoids exposing the password in process arguments. Passwords require 12–1024
bytes without NUL/line breaks; input files may end with CR/LF. `--username` changes
the bootstrap/command username. `--credentials-file` and `--initial-password-file`
select persistent paths for local runs. Remove the initial plaintext file after
recording or rotating it; subsequent normal startups need only the hash. Password
rotation takes effect without a service restart.

Hashing uses linked OpenSSL PBKDF2-HMAC-SHA256, a random 256-bit salt and 600,000
iterations, following the [OWASP password storage guidance](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html).
Comparisons use constant time verification. Five attempts per client IP per minute
are allowed, shared across browser sessions; successful sign-in resets that
client's counter. Authentication storage errors deny access. All API actions
check the UI session authentication on the server. This is a single sysop login;
there is no end-user account registration or MCP interface.

Wt uses combined cookie/session-ID tracking. The provided configuration enables
progressive bootstrap, no-store responses and a no-referrer policy. Access logs
are disabled to avoid recording session-bearing URLs. API transport logs never
include keys, raw response bodies or libcurl debug traces. All API strings are
rendered as plain text, including auto-replies and organization names.

## JavaScript and evaluation

Wt's [documented progressive bootstrap](https://www.webtoolkit.eu/wt/wt4/doc/reference/html/overview.html)
starts with plain HTML and upgrades to Ajax when available. Tree expansion,
selection, login and buttons use native Wt widgets. With JavaScript, a Wt timer
checks asynchronous API results every second. With JavaScript disabled, press
**Check for results** after a request to retrieve its completion through a normal
form submission. Network requests never block Wt event threads, and background
jobs have no widget pointers. Signing out or closing a browser during a request
cannot cause a callback to access a destroyed application.

Use **Refresh tree** to rebuild the hierarchy and **Refresh mailbox** to reload
the selected account. Multiple GETs do not form a transaction: concurrent API
changes can require a refresh. A tree refresh clears the old selection/data;
failed loads show a sanitized status rather than presenting stale settings as
current. API 401 clears displayed data and reports the server credential failure. Responses are bounded to
32 MiB, lists to 10000 records, and IDs/ownership are validated before display.

Before deciding whether to keep Wt, exercise both JavaScript modes: connect,
expand/collapse the tree, select accounts, compare inherited policies and aliases
with the API, refresh after an API change, sign out, and try a rejected UI password.
The credential and authentication CTests additionally cover protected file sources,
bootstrap recovery, password hashing, session revocation and login throttling.
Also test an unavailable API and switching away/closing the session during a
load. `ui_data` covers API relationship validation, alias isolation, empty domains,
sorting, literal text, inheritance and distinct activation states.

The read-only Playwright test `tests/staging_ui.py` runs on the controller and
reads the staging API key and initial UI password into memory over SSH, without putting it in arguments or
logs. It exercises both JavaScript modes against the deployed UI and real API:

```sh
python3 -m venv /tmp/mailadmin-ui-browser
/tmp/mailadmin-ui-browser/bin/pip install playwright
/tmp/mailadmin-ui-browser/bin/python -m playwright install chromium
/tmp/mailadmin-ui-browser/bin/python tests/staging_ui.py \
  --url http://192.168.1.199:8081 --ssh-host staging
```

The container CTest and both browser modes passed against staging. A repeated
Ansible deployment reported zero changes; API authentication/inventory/metrics
smoke checks also passed. `.github/workflows/ui-container.yml` provides a separate
main-branch GHCR build using `MY_PAT`, publishing `<repository>-ui:latest` and a
full commit-SHA tag. The workflow was validated locally but has not been pushed
or executed.


## Editing

Select an organization or domain to view its settings and create, edit or delete
children. The mailbox panel supports its address, name, quota, desired enabled
state, auto-reply, password action, policy overrides, forwarding and keep-copy.
The forwarding editor replaces the list on Save; removing a line removes that
destination. Alias editors support address changes and retargeting within the
same organization. Deletes require an explicit confirmation screen. Generated
passwords appear in a dismissible notice after a successful save; they are not
retrievable through account reads. Cancel never sends an API mutation.

New organizations create their default domain. New domains use the configured
shared mail hostname for DNS, or can explicitly provision a dedicated host with
supplied IPv4/IPv6 addresses. DNS settings expose the complete desired JSON,
including all DKIM selectors and policies, with create-only versus authoritative
replacement made explicit. The Operations view shows Courier/cleanup and DNS
state, including reports for deleted domains still retiring. See [DNS setup](dns.md).

`tests/staging_ui_crud.py` exercises Save/Cancel CRUD, forwarding, alias rename
and retargeting, generated passwords, dedicated host provisioning and complete
DNS replacement in both browser modes. It creates only unique names below the
authorized staging zone and removes its mailbox/domain records. Host address
records are intentionally retained by the preserve-IP removal policy.
