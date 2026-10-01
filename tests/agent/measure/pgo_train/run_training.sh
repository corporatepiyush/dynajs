#!/bin/zsh
set -u
cd "$(dirname "$0")/../../../.."
DYNA=${1:-./dynajs}
PROFDATA_DIR=pgo-data
LOG=pgo_train.log
: > "$LOG"

echo "== training driver: $DYNA $(date)" | tee -a "$LOG"
rm -rf "$PROFDATA_DIR"; mkdir -p "$PROFDATA_DIR"
export LLVM_PROFILE_FILE="$PROFDATA_DIR/%p-%m.profraw"
mkdir -p scratch
train () {
  local label=$1 tmo=$2; shift 2
  echo "-- train: $label (timeout ${tmo}s)" >> "$LOG"
  timeout "$tmo" "$@" >>"$LOG" 2>&1
  echo "   rc=$? ($label)" >> "$LOG"
}

for f in tests/agent/bbreview/*.js; do
  cat tests/agent/_h/h.js "$f" > scratch/train_bb.js
  train "bbreview/$(basename "$f")" 30 "$DYNA" scratch/train_bb.js
done

for f in tests/test_builtin.js tests/test_closure.js tests/test_language.js \
         tests/test_modern.js tests/test_array_ext.js tests/test_iterator_lazy.js \
         tests/test_string_ext.js tests/test_object_ext.js tests/test_object_ext.js \
         tests/test_number_ext.js tests/test_loop.js tests/test_optimizer.js \
         tests/test_bigint.js tests/test_date_ext.js tests/test_function_ext.js \
         tests/test_disposable.js; do
  train "core/$(basename "$f")" 60 "$DYNA" --std "$f"
done

for i in $(seq 10); do
  train "modules/pass$i" 60 "$DYNA" -m tests/agent/measure/pgo_train/train_modules.js
done

for f in bench/bench_markdown.js bench/bench_strscan.js bench/bench_xmlparse.js \
         bench/bench_stdlib_all.js bench/http_parse.js; do
  [ -f "$f" ] && train "bench/$(basename "$f")" 60 "$DYNA" "$f"
done

replay=tests/agent/measure/pgo_train/repl_session.txt
train "repl/pass1" 60 sh -c "cat $replay | script -q /dev/null $DYNA -i >/dev/null 2>&1"
train "repl/pass2" 60 sh -c "cat $replay | script -q /dev/null $DYNA -i >/dev/null 2>&1"

n=$(ls "$PROFDATA_DIR" | wc -l | tr -d ' ')
echo "== collected $n profraw files" | tee -a "$LOG"
if [ "$n" = "0" ]; then echo "FAIL: no profiles collected" | tee -a "$LOG"; exit 1; fi

if command -v llvm-profdata >/dev/null 2>&1; then
  llvm-profdata merge -o pgo.profdata "$PROFDATA_DIR"/*.profraw || exit 1
else
  xcrun llvm-profdata merge -o pgo.profdata "$PROFDATA_DIR"/*.profraw || exit 1
fi
ls -la pgo.profdata | tee -a "$LOG"
echo "== training complete $(date)" | tee -a "$LOG"
