#!/bin/sh
# Usage:
#   ./trace.sh OUTPUTFILE command [args...]
#
# Records all files opened and exec'd by command.
# Writes one pathname per line to OUTPUTFILE.
# Tries to work with both newer and older strace.

set -eu

if [ $# -lt 2 ]; then
  echo "usage: $0 OUTPUTFILE command [args...]" >&2
  exit 1
fi

outfile=$1
shift

get_interp() {
LC_ALL=C readelf -l "$1" | awk '/program interpreter/ { sub(/^.*: /, "", $0); sub(/\]$/, "", $0); print }'
}

tmp=${outfile}.tmp.$$
trap 'rm -f "$tmp"' EXIT HUP INT TERM

awk_filter='
function emit_path(s) {
  if (s != "" && s != "AT_FDCWD" && s !~ /^<.*>$/)
    print s
}

function first_quoted_arg(line,    a, b, c) {
  a = index(line, "\"")
  if (!a) return ""
  b = a + 1
  while (b <= length(line)) {
    c = substr(line, b, 1)
    if (c == "\"" && substr(line, b - 1, 1) != "\\") break
    b++
  }
  if (b > length(line)) return ""
  return substr(line, a + 1, b - a - 1)
}

function strip_pid_prefix(line) {
  sub(/^\[pid [0-9]+\] /, "", line)
  sub(/^ +/, "", line)
  return line
}

{
  $0 = strip_pid_prefix($0)

  if ($0 ~ /^(execve|open|openat)\(/) {
    emit_path(first_quoted_arg($0))
  }
}
'

if strace -h 2>&1 | grep -q -- '-e trace='; then
  # Newer strace: direct syscall filtering.
  strace -qq -f -s 4096 \
    -e trace=execve,open,openat \
    "$@" 2>&1 |
  awk "$awk_filter" > "$tmp"
else
  # Older strace: trace file-related syscalls broadly, then filter in awk.
  strace -qq -f -s 4096 \
    "$@" 2>&1 |
  awk "$awk_filter" > "$tmp"
fi

mv -f "$tmp" "$outfile"
get_interp "$(head -n 1 "$outfile")" >> "$outfile"
