#!/usr/bin/env bash
# The dependency pins live in four files that cannot source each other (install.sh runs
# from a pipe before the tree exists, the Dockerfile COPYs one script, the Android build is
# standalone). This fails when any two disagree, naming both values.
#   tools/check-dep-pins.sh [ROOT]
set -u
ROOT=${1:-$(cd "$(dirname "$0")/.." && pwd)}
bad=0

val() {
    local file=$1 key=$2 v
    v=$(grep -E "^${key}=" "$ROOT/$file" | head -1 | sed -E "s/^${key}=//; s/^\"//; s/\"$//")
    [ -n "$v" ] || { echo "check-dep-pins: FAIL $file declares no $key" >&2; bad=1; }
    printf '%s' "$v"
}

same() {
    local what=$1 a=$2 b=$3 an=$4 bn=$5
    if [ "$a" != "$b" ]; then
        echo "check-dep-pins: FAIL $what: $an says '$a', $bn says '$b'" >&2
        bad=1
    fi
}

b_sqlite=$(val build.sh DEP_SQLITE_MIN)
b_ossl=$(val build.sh DEP_OPENSSL_SERIES)
b_sdl=$(val build.sh DEP_SDL3_MIN)
b_ray=$(val build.sh DEP_RAYLIB_MIN)

i_sqlite=$(val install.sh SQLITE_MIN)
i_tar=$(val install.sh SQLITE_TARBALL)
i_sha=$(val install.sh SQLITE_SHA256)
i_ossl=$(val install.sh OPENSSL_SERIES)
i_osslv=$(val install.sh OPENSSL_VERSION)
i_sdl=$(val install.sh SDL3_MIN)
i_sdltag=$(val install.sh SDL3_TAG)
i_ray=$(val install.sh RAYLIB_MIN)
i_raytag=$(val install.sh RAYLIB_TAG)

d_sqlite=$(val docker/build-sqlite.sh SQLITE_VERSION)
d_tar=$(val docker/build-sqlite.sh SQLITE_TARBALL)
d_sha=$(val docker/build-sqlite.sh SQLITE_SHA256)

a_sdl=$(val tools/android/build-apk.sh SDL_VER)

same "SQLite minimum" "$b_sqlite" "$i_sqlite" build.sh install.sh
same "SQLite release" "$b_sqlite" "$d_sqlite" build.sh docker/build-sqlite.sh
same "SQLite tarball" "$i_tar" "$d_tar" install.sh docker/build-sqlite.sh
same "SQLite sha256" "$i_sha" "$d_sha" install.sh docker/build-sqlite.sh
same "OpenSSL series" "$b_ossl" "$i_ossl" build.sh install.sh
same "OpenSSL release series" "$b_ossl" "${i_osslv%.*}" build.sh "install.sh OPENSSL_VERSION"
same "SDL3 minimum" "$b_sdl" "$i_sdl" build.sh install.sh
same "SDL3 source tag" "release-$b_sdl" "$i_sdltag" build.sh install.sh
same "SDL3 Android pin" "$b_sdl" "$a_sdl" build.sh tools/android/build-apk.sh
same "raylib minimum" "$b_ray" "$i_ray" build.sh install.sh
same "raylib source tag" "$b_ray" "$i_raytag" build.sh install.sh

v=$(printf '%s' "$b_sqlite" | awk -F. '{ printf "%d%02d%02d00", $1, $2, $3 }')
same "SQLite tarball names the release" "sqlite-autoconf-$v.tar.gz" "$i_tar" "build.sh (derived)" install.sh

[ "$bad" -eq 0 ] || exit 1
echo "check-dep-pins: ok (SQLite $b_sqlite, OpenSSL $i_osslv, SDL3 $b_sdl, raylib $b_ray)"
