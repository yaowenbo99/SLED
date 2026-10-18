#!/usr/bin/env bash
set -euo pipefail

DUCKDB_VERSION="${1:-v1.5.3}"
BOOST_VERSION="${2:-1.85.0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="${THIRD_PARTY_DIR:-$ROOT/third_party}"
DUCKDB_DIR="$THIRD_PARTY/duckdb"
JSON_DIR="$ROOT/third_party/nlohmann/include/nlohmann"
BOOST_DIR="$THIRD_PARTY/boost/include/boost"

mkdir -p "$DUCKDB_DIR" "$JSON_DIR" "$(dirname "$BOOST_DIR")"

case "$(uname -s)-$(uname -m)" in
  Darwin-*)
    DUCKDB_ASSET="libduckdb-osx-universal.zip"
    ;;
  Linux-x86_64)
    DUCKDB_ASSET="libduckdb-linux-amd64.zip"
    ;;
  Linux-aarch64|Linux-arm64)
    DUCKDB_ASSET="libduckdb-linux-aarch64.zip"
    ;;
  *)
    echo "unsupported platform for automatic DuckDB bootstrap: $(uname -s)-$(uname -m)" >&2
    echo "install DuckDB manually and configure with -DDUCKDB_ROOT=/path/to/duckdb" >&2
    exit 1
    ;;
esac

if [[ ! -f "$DUCKDB_DIR/include/duckdb.h" || ! -f "$DUCKDB_DIR/lib/libduckdb.dylib" && ! -f "$DUCKDB_DIR/lib/libduckdb.so" ]]; then
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "$TMP_DIR"' EXIT
  URL="https://github.com/duckdb/duckdb/releases/download/$DUCKDB_VERSION/$DUCKDB_ASSET"
  echo "Downloading DuckDB C library: $URL"
  curl -fL --retry 3 "$URL" -o "$TMP_DIR/duckdb.zip"
  unzip -q "$TMP_DIR/duckdb.zip" -d "$TMP_DIR/duckdb"
  mkdir -p "$DUCKDB_DIR/include" "$DUCKDB_DIR/lib"
  cp "$TMP_DIR/duckdb/duckdb.h" "$DUCKDB_DIR/include/"
  if [[ -f "$TMP_DIR/duckdb/libduckdb.dylib" ]]; then
    cp "$TMP_DIR/duckdb/libduckdb.dylib" "$DUCKDB_DIR/lib/"
  elif [[ -f "$TMP_DIR/duckdb/libduckdb.so" ]]; then
    cp "$TMP_DIR/duckdb/libduckdb.so" "$DUCKDB_DIR/lib/"
  else
    echo "downloaded DuckDB archive did not contain libduckdb" >&2
    exit 1
  fi
fi

if [[ ! -f "$JSON_DIR/json.hpp" ]]; then
  echo "Downloading nlohmann/json header"
  curl -fL --retry 3 "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" -o "$JSON_DIR/json.hpp"
fi

if [[ ! -f "$BOOST_DIR/geometry.hpp" ]]; then
  BOOST_ARCHIVE_VERSION="${BOOST_VERSION//./_}"
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "$TMP_DIR"' EXIT
  echo "Downloading Boost headers $BOOST_VERSION"
  curl -fL --retry 3 "https://archives.boost.io/release/$BOOST_VERSION/source/boost_$BOOST_ARCHIVE_VERSION.tar.bz2" \
    -o "$TMP_DIR/boost.tar.bz2"
  tar -xjf "$TMP_DIR/boost.tar.bz2" -C "$TMP_DIR"
  cp "$TMP_DIR/boost_$BOOST_ARCHIVE_VERSION/LICENSE_1_0.txt" "$THIRD_PARTY/boost/"
  cp -R "$TMP_DIR/boost_$BOOST_ARCHIVE_VERSION/boost" "$(dirname "$BOOST_DIR")/"
fi

echo "Dependencies are available under $THIRD_PARTY"
