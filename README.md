# Mailadmin

An administrator API for an existing Courier mail server. C++20/Asio and yahat,
with bearer authentication, OpenSSL key generation and domain-count metrics.

- [Project specification and production facts](doc/project.md)
- [Mail server requirements](doc/mailserver.md)
- [Staging/deployment runbook](deploy/ansible/README.md)
- [Administrator API](doc/api.md)
- [Deployed staging service and verification](doc/staging.md)

The daemon provides organization/domain/account/alias administration, secure
password creation and reset, Maildir lifecycle, forwarding/auto-replies, inherited
spam policies, Courier configuration activation and resumable cleanup.
[Administration and recovery](doc/administration.md) describes the explicit
schema migration and restricted host publisher. [nsblast DNS provisioning](doc/dns.md)
uses a durable job table and scoped provider credentials.

The root CMake project includes `api-server` and `ui-server`. The API daemon
sources live in `api-server/src`. The experimental [Wt UI](doc/ui.md) in
`ui-server/src` browses organizations, domains and mailbox configuration through
the API using RESTinCurl. Shared regression tests remain in `tests`.
The API executable is built as `build/api-server/mailadmin`; the UI executable is
`build/ui-server/src/mailadmin_ui`.

Build on Linux with CMake >= 3.24, a C++20 compiler/library supporting
`std::format` (such as Debian 13's GCC 14), Boost >= 1.81, OpenSSL, libxcrypt, libcurl, zlib,
pkg-config and MariaDB Connector/C development packages. Source dependencies are
fetched from GitHub; sibling source checkouts are not build inputs.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMAILADMIN_BUILD_TESTS=ON -DMAILADMIN_BUILD_UI=OFF
cmake --build build --target mailadmin mailadmin_security_test mailadmin_async_test mailadmin_mailstore_test mailadmin_dns_test --parallel 2
ctest --test-dir build --output-on-failure
```

The Dockerfile supplies a Debian 13 build/runtime environment, also usable on
a Debian 12 Docker host. For a running
staging instance, use the read-only wire checks in `tests/staging_smoke.py` as
described in the deployment runbook. No production configuration is needed for
the isolated security test.

`./deploy/build-image.sh` builds locally with rootless Buildah (or Docker) and
exports a deployment archive. Docker daemon access is not required for Buildah.
For the UI image, pass `Dockerfile.ui` as its third argument; the
[UI guide](doc/ui.md) covers deployment and testing. The staging UI is available
over HTTP at `http://192.168.1.199:8081`, using the separate `admin` UI login; see [UI authentication](doc/ui.md#ui-administrator-password).
