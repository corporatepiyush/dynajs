cd "$(dirname "$0")/../../.." || exit 1
echo "--- patched:"; ./dynajs -I tests/agent/constprop_review/s1_const.js tests/agent/constprop_review/s2_mutate.js
echo "--- pristine:"; ./dynajs.pristine -I tests/agent/constprop_review/s1_const.js tests/agent/constprop_review/s2_mutate.js
