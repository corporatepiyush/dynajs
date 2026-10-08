#!/bin/zsh
# extract clean unique failing-test list from a run-test262 log (progress-bar safe:
# error lines start at line-begin with the filename; take up to the first colon)
for log in "$@"; do
  grep "unexpected error" "$log" | grep "^tools/" | sed -E 's/^([^ :]+\.js):.*/\1/' | sort > "${log}.files"
  n=$(grep -c "unexpected error" "$log")
  echo "$log: error_lines=$n unique_files=$(wc -l < ${log}.files | tr -d ' ')"
done
