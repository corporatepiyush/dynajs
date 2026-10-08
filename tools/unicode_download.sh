#!/bin/sh
# unicode_download.sh -- fetch the Unicode UCD data files that feed
# tools/unicode_gen.c (built as `unicode_gen`, which generates
# libunicode-table.h for the engine when unicode/UnicodeData.txt exists).
# Downloads the pinned version below into ./unicode/ with wget, overwriting.
# Run from the repo root; needs network + wget; exits on the first failed
# download (set -e).
set -e

version="17.0.0"
url="ftp://ftp.unicode.org/Public"


files="CaseFolding.txt DerivedNormalizationProps.txt PropList.txt \
SpecialCasing.txt CompositionExclusions.txt ScriptExtensions.txt \
UnicodeData.txt DerivedCoreProperties.txt NormalizationTest.txt Scripts.txt \
PropertyValueAliases.txt"

mkdir -p unicode

for f in $files; do
    g="${url}/${version}/ucd/${f}"
    wget $g -O unicode/$f
done

wget "${url}/${version}/ucd/emoji/emoji-data.txt" -O unicode/emoji-data.txt

wget "${url}/${version}/emoji/emoji-sequences.txt" -O unicode/emoji-sequences.txt
wget "${url}/${version}/emoji/emoji-zwj-sequences.txt" -O unicode/emoji-zwj-sequences.txt
