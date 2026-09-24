#!/bin/sh
set -e

version="17.0.0"
url="ftp://ftp.unicode.org/Public"

# NOTE (audit M34-02): the four IDNA tables vendored into src/dyna-idna.inc.c
# are NOT fetched by this script -- they are pinned to 16.0.0
# (IdnaMappingTable, DerivedBidiClass, ArabicShaping, UnicodeData, plus the
# IdnaTestV2 conformance corpus in tests/test_idna.js) and may move on a
# different cadence than the engine's libunicode tables. To re-vendor them:
# fetch the same-named files plus IdnaTestV2.txt from
#   https://www.unicode.org/Public/<IDNA-VERSION>/ucd/  (IdnaTestV2 from ../)
# regenerate the header, and re-pin the version comment at its top.

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
