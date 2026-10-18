#!/bin/sh
# Builds the pinned SQLite release from its autoconf amalgamation into PREFIX as a STATIC
# library only, so nothing at run time can hand the binary an older libsqlite3.so.
#   docker/build-sqlite.sh [PREFIX]     (default /usr/local; WORK=<dir> for the scratch tree)
# The version, URL and sha256 are also in install.sh (SQLITE_*), and build.sh's
# DEP_SQLITE_MIN is the same release; tools/check-dep-pins.sh fails when they drift.
# The feature set matches Homebrew's sqlite formula (FTS3/4/5, R*Tree, Geopoly, session,
# dbstat, column metadata, STAT4, ...): tests/net/test_net_sqlite.js requires the core of it.
set -eu
SQLITE_VERSION=3.54.1
SQLITE_TARBALL="sqlite-autoconf-3540100.tar.gz"
SQLITE_URL=https://www.sqlite.org/2026/$SQLITE_TARBALL
SQLITE_SHA256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e

PREFIX=${1:-/usr/local}
WORK=${WORK:-$(mktemp -d)}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CC=${CC:-cc}
CC_FOR_BUILD=${CC_FOR_BUILD:-$CC}
export CC CC_FOR_BUILD

mkdir -p "$WORK"
cd "$WORK"
curl -fsSL -o "$SQLITE_TARBALL" "$SQLITE_URL"
got=$( (sha256sum "$SQLITE_TARBALL" 2>/dev/null || shasum -a 256 "$SQLITE_TARBALL") | cut -d' ' -f1)
[ "$got" = "$SQLITE_SHA256" ] || { echo "build-sqlite: $SQLITE_TARBALL sha256 is $got, expected $SQLITE_SHA256" >&2; exit 1; }
rm -rf "sqlite-autoconf-3540100"
tar -xzf "$SQLITE_TARBALL"
cd sqlite-autoconf-3540100
CFLAGS="-O2 -fPIC -DSQLITE_ENABLE_API_ARMOR -DSQLITE_ENABLE_STAT4 -DSQLITE_ENABLE_UNLOCK_NOTIFY \
-DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_PERCENTILE -DSQLITE_ENABLE_MEMORY_MANAGEMENT \
-DSQLITE_ENABLE_FTS3_PARENTHESIS -DSQLITE_MAX_VARIABLE_NUMBER=250000" \
  ./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --fts3 --fts4 --fts5 --rtree --geopoly --session --dbstat --column-metadata
make -j"$JOBS"
make install
echo "build-sqlite: SQLite $SQLITE_VERSION installed (static) into $PREFIX"
