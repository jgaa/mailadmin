#!/usr/bin/env bash
set -euo pipefail
umask 077
mailadmin_test_base=${MAILADMIN_TEST_MARIADB_BASEDIR:-/usr}
mailadmin_test_server="$mailadmin_test_base/sbin/mariadbd"
mailadmin_test_client="$mailadmin_test_base/bin/mariadb"
mailadmin_test_install="$mailadmin_test_base/bin/mariadb-install-db"
if [[ ! -x "$mailadmin_test_server" || ! -x "$mailadmin_test_client" || ! -x "$mailadmin_test_install" ]]; then
    printf '%s\n' 'Isolated MariaDB fixture unavailable (requires server and client tools)'
    exit 77
fi
mailadmin_test_dir=$(mktemp -d /tmp/mailadmin-legacy-db.XXXXXXXX)
mailadmin_test_pid=''
mailadmin_test_cleanup() {
    if [[ -n "$mailadmin_test_pid" ]]; then
        kill "$mailadmin_test_pid" 2>/dev/null || true
        wait "$mailadmin_test_pid" 2>/dev/null || true
    fi
    rm -rf -- "$mailadmin_test_dir"
}
trap mailadmin_test_cleanup EXIT
"$mailadmin_test_install" --no-defaults --basedir="$mailadmin_test_base" --datadir="$mailadmin_test_dir/data" \
    --auth-root-authentication-method=normal --skip-test-db > "$mailadmin_test_dir/init.log" 2>&1
"$mailadmin_test_server" --no-defaults --user="$(id -un)" --basedir="$mailadmin_test_base" --datadir="$mailadmin_test_dir/data" \
    --socket="$mailadmin_test_dir/socket" --pid-file="$mailadmin_test_dir/pid" --skip-networking \
    --log-error="$mailadmin_test_dir/server.log" &
mailadmin_test_pid=$!
mailadmin_test_ready=false
for ((mailadmin_test_attempt=0; mailadmin_test_attempt<100; ++mailadmin_test_attempt)); do
    if "$mailadmin_test_client" --no-defaults --socket="$mailadmin_test_dir/socket" -uroot -e 'SELECT 1' > /dev/null 2>&1; then
        mailadmin_test_ready=true
        break
    fi
    sleep 0.1
done
if [[ "$mailadmin_test_ready" != true ]]; then
    printf '%s\n' 'Isolated MariaDB fixture did not become ready' >&2
    exit 1
fi
"$mailadmin_test_client" --no-defaults --socket="$mailadmin_test_dir/socket" -uroot -e 'CREATE DATABASE mailadmin_legacy_test'
"$mailadmin_test_client" --no-defaults --socket="$mailadmin_test_dir/socket" -uroot mailadmin_legacy_test < "$2"
"$1" "$mailadmin_test_dir/socket"
