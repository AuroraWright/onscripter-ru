#!/bin/bash
set -euo pipefail

# Pass an sdl2-config path for an onscrlib build, then optionally --unit to skip sockets.
project_dir="$(cd "$(dirname "$0")/.." && pwd)"
text_websocket_sdl_config="${1:-sdl2-config}"
if (( $# )); then shift; fi
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/ons-websocket-tests.XXXXXX")"
trap 'rm -f "$test_dir/tests"; rmdir "$test_dir"' EXIT
platform_library=""
case "$(uname -s)" in
    Darwin) platform_flags=(-DMACOSX) ;;
    MINGW*|MSYS*) platform_flags=(-DWIN32); platform_library="-lws2_32" ;;
    *) platform_flags=(-DLINUX) ;;
esac
"${CXX:-c++}" -std=c++14 -I"$project_dir" "${platform_flags[@]}" \
    $("$text_websocket_sdl_config" --cflags) \
    -I"$("$text_websocket_sdl_config" --prefix)/include" \
    "$project_dir/Tests/TextWebSocket.cpp" \
    $("$text_websocket_sdl_config" --static-libs) ${platform_library:+"$platform_library"} -o "$test_dir/tests"
"$test_dir/tests" "$@"
