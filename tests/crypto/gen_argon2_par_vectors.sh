#!/bin/sh
# Generates the external Argon2id vectors for tests/crypto/test_crypto_argon2_par.js with OpenSSL 3.5's
# ARGON2ID KDF (an independent implementation; it reproduces RFC 9106 section 5.3's tag 0d640df5...).
# Usage: tests/crypto/gen_argon2_par_vectors.sh [openssl binary]
O=${1:-/opt/homebrew/opt/openssl@3.5/bin/openssl}
i=0
for spec in 1:128:16 3:32:4 2:64:1 1:1024:4 3:4096:4 2:4096:3 1:2048:8 2:16384:16 1:65536:4 3:8192:5 1:512:2 4:1000:7 1:4096:16 2:2048:1 1:3000:6; do
  t=${spec%%:*}; r=${spec#*:}; m=${r%%:*}; p=${r#*:}
  pw="pw-$i-par"; salt="saltsalt-$i-xyz"
  tag=$("$O" kdf -keylen 32 -kdfopt pass:"$pw" -kdfopt salt:"$salt" -kdfopt iter:"$t" -kdfopt memcost:"$m" -kdfopt lanes:"$p" -kdfopt threads:1 ARGON2ID | tr -d ':\n' | tr 'A-F' 'a-f')
  printf '    [%s, %s, %s, "%s", "%s", "%s"],\n' "$t" "$m" "$p" "$pw" "$salt" "$tag"
  i=$((i+1))
done
