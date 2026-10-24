#!/usr/bin/env bash
set -euo pipefail
umask 077

mailadmin_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mailadmin_image=${1:-mailadmin:staging}
mailadmin_output=${2:-${XDG_STATE_HOME:-$HOME/.local/state}/mailadmin/images/staging.tar}
if [[ "$mailadmin_image" == -* || "$mailadmin_image" == *[[:space:]]* ]]; then
    printf '%s\n' 'Invalid image reference' >&2
    exit 2
fi
mailadmin_builder=${MAILADMIN_BUILDER:-auto}
if [[ "$mailadmin_builder" == auto ]]; then
    if command -v buildah >/dev/null 2>&1; then
        mailadmin_builder=buildah
    elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        mailadmin_builder=docker
    else
        printf '%s\n' 'Install rootless Buildah or enable a local Docker engine. Build on the controller, not staging.' >&2
        exit 1
    fi
fi
mkdir -p -- "$(dirname -- "$mailadmin_output")"
mailadmin_temporary=$(mktemp "${mailadmin_output}.XXXXXX")
trap 'rm -f -- "$mailadmin_temporary"' EXIT
case "$mailadmin_builder" in
    buildah)
        buildah build --layers --force-rm --network slirp4netns --tag "$mailadmin_image" "$mailadmin_repo"
        buildah push --quiet "$mailadmin_image" "docker-archive:${mailadmin_temporary}:${mailadmin_image}"
        ;;
    docker)
        docker build --tag "$mailadmin_image" "$mailadmin_repo"
        docker save --output "$mailadmin_temporary" "$mailadmin_image"
        ;;
    *)
        printf '%s\n' 'MAILADMIN_BUILDER must be auto, buildah or docker' >&2
        exit 2
        ;;
esac
mv -f -- "$mailadmin_temporary" "$mailadmin_output"
printf 'Built %s; deployable image archive: %s\n' "$mailadmin_image" "$mailadmin_output"
