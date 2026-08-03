/* BLAKE3, BLAKE2b/2s and Murmur3-128 against INDEPENDENT implementations
   (the blake3 and mmh3 packages, and hashlib), not against a round trip.
   The sizes cross this code's own cliffs: the 64-byte block, the 1024-byte
   chunk, and the merges above it -- a merge-order bug was invisible below
   2049 bytes and wrong at every size above it. */
import { BLAKE3, BLAKE3Hex, BLAKE2bHex, BLAKE2sHex, Murmur3_128, Murmur3_128Hex } from "dyna:hash";
let n = 0, fails = 0;
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL " + m + "\n  got  " + a + "\n  want " + b); } }
function pat(k) { const a = new Uint8Array(k); for (let i = 0; i < k; i++) a[i] = i % 251; return a; }
eq(BLAKE3Hex(pat(0)), "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262", "BLAKE3 0");
eq(BLAKE2bHex(pat(0)), "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce", "BLAKE2b 0");
eq(BLAKE2sHex(pat(0)), "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", "BLAKE2s 0");
eq(Murmur3_128Hex(pat(0)), "00000000000000000000000000000000", "Murmur3 0");
eq(BLAKE3Hex(pat(1)), "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213", "BLAKE3 1");
eq(BLAKE2bHex(pat(1)), "2fa3f686df876995167e7c2e5d74c4c7b6e48f8068fe0e44208344d480f7904c36963e44115fe3eb2a3ac8694c28bcb4f5a0f3276f2e79487d8219057a506e4b", "BLAKE2b 1");
eq(BLAKE2sHex(pat(1)), "e34d74dbaf4ff4c6abd871cc220451d2ea2648846c7757fbaac82fe51ad64bea", "BLAKE2s 1");
eq(Murmur3_128Hex(pat(1)), "b55cff6ee5ab10468335f878aa2d6251", "Murmur3 1");
eq(BLAKE3Hex(pat(2)), "7b7015bb92cf0b318037702a6cdd81dee41224f734684c2c122cd6359cb1ee63", "BLAKE3 2");
eq(BLAKE2bHex(pat(2)), "1c08798dc641aba9dee435e22519a4729a09b2bfe0ff00ef2dcd8ed6f8a07d15eaf4aee52bbf18ab5608a6190f70b90486c8a7d4873710b1115d3debbb4327b5", "BLAKE2b 2");
eq(BLAKE2sHex(pat(2)), "ddad9ab15dac4549ba42f49d262496bef6c0bae1dd342a8808f8ea267c6e210c", "BLAKE2s 2");
eq(Murmur3_128Hex(pat(2)), "4c26ab8dc5f5b37c44e0c26e32013cf0", "Murmur3 2");
eq(BLAKE3Hex(pat(63)), "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b", "BLAKE3 63");
eq(BLAKE2bHex(pat(63)), "d10bf9a15b1c9fc8d41f89bb140bf0be08d2f3666176d13baac4d381358ad074c9d4748c300520eb026daeaea7c5b158892fde4e8ec17dc998dcd507df26eb63", "BLAKE2b 63");
eq(BLAKE2sHex(pat(63)), "e57cb79487dd57902432b250733813bd96a84efce59f650fac26e6696aefafc3", "BLAKE2s 63");
eq(Murmur3_128Hex(pat(63)), "94600734460da3996743c2d4a1c6a4ed", "Murmur3 63");
eq(BLAKE3Hex(pat(64)), "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98", "BLAKE3 64");
eq(BLAKE2bHex(pat(64)), "2fc6e69fa26a89a5ed269092cb9b2a449a4409a7a44011eecad13d7c4b0456602d402fa5844f1a7a758136ce3d5d8d0e8b86921ffff4f692dd95bdc8e5ff0052", "BLAKE2b 64");
eq(BLAKE2sHex(pat(64)), "56f34e8b96557e90c1f24b52d0c89d51086acf1b00f634cf1dde9233b8eaaa3e", "BLAKE2s 64");
eq(Murmur3_128Hex(pat(64)), "0123818d2d52d5ffa18e3356eb3822a2", "Murmur3 64");
eq(BLAKE3Hex(pat(65)), "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee", "BLAKE3 65");
eq(BLAKE2bHex(pat(65)), "fcbe8be7dcb49a32dbdf239459e26308b84dff1ea480df8d104eeff34b46fae98627b450c2267d48c0946a697c5b59531452ac0484f1c84e3a33d0c339bb2e28", "BLAKE2b 65");
eq(BLAKE2sHex(pat(65)), "1b53ee94aaf34e4b159d48de352c7f0661d0a40edff95a0b1639b4090e974472", "BLAKE2s 65");
eq(Murmur3_128Hex(pat(65)), "e21e984d019136d08efc5b38b08258bd", "Murmur3 65");
eq(BLAKE3Hex(pat(127)), "d81293fda863f008c09e92fc382a81f5a0b4a1251cba1634016a0f86a6bd640d", "BLAKE3 127");
eq(BLAKE2bHex(pat(127)), "b6292669ccd38d5f01caae96ba272c76a879a45743afa0725d83b9ebb26665b731f1848c52f11972b6644f554c064fa90780dbbbf3a89d4fc31f67df3e5857ef", "BLAKE2b 127");
eq(BLAKE2sHex(pat(127)), "f18417b39d617ab1c18fdf91ebd0fc6d5516bb34cf39364037bce81fa04cecb1", "BLAKE2s 127");
eq(Murmur3_128Hex(pat(127)), "4b43af66bdf09fba5e41dbb64227f511", "Murmur3 127");
eq(BLAKE3Hex(pat(128)), "f17e570564b26578c33bb7f44643f539624b05df1a76c81f30acd548c44b45ef", "BLAKE3 128");
eq(BLAKE2bHex(pat(128)), "2319e3789c47e2daa5fe807f61bec2a1a6537fa03f19ff32e87eecbfd64b7e0e8ccff439ac333b040f19b0c4ddd11a61e24ac1fe0f10a039806c5dcc0da3d115", "BLAKE2b 128");
eq(BLAKE2sHex(pat(128)), "1fa877de67259d19863a2a34bcc6962a2b25fcbf5cbecd7ede8f1fa36688a796", "BLAKE2s 128");
eq(Murmur3_128Hex(pat(128)), "537ef1a53b4dd7952e808db6afa7a0ab", "Murmur3 128");
eq(BLAKE3Hex(pat(1023)), "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11", "BLAKE3 1023");
eq(BLAKE2bHex(pat(1023)), "e55fd611a16696f8295ea5120a151e312e5dfb1488ac74be64118ffe1bc1d539e725ad0440e5213de297ba435d381c66edf88eebf28b8d640e31103842d3be29", "BLAKE2b 1023");
eq(BLAKE2sHex(pat(1023)), "e73d4cf80407e185a3f0d14c0472d35e2ca15218ce76004e8a596065d33af5c3", "BLAKE2s 1023");
eq(Murmur3_128Hex(pat(1023)), "dae047d038ce46ed070e9770a6957c9b", "Murmur3 1023");
eq(BLAKE3Hex(pat(1024)), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7", "BLAKE3 1024");
eq(BLAKE2bHex(pat(1024)), "8d1090909017add40e749df2d0ebac43273d6fc816bc4ffaf2a6dfabe4206dea13677d2002399e4a38e700d8083db4af8341ee9b3a5147110b6a963a3894e4e2", "BLAKE2b 1024");
eq(BLAKE2sHex(pat(1024)), "eefe540b091c081f91a31b4db99926352f05cc012a7a1402268923dd00a278d7", "BLAKE2s 1024");
eq(Murmur3_128Hex(pat(1024)), "47db36fd76caf30a593eedf2ed69b8e4", "Murmur3 1024");
eq(BLAKE3Hex(pat(1025)), "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444", "BLAKE3 1025");
eq(BLAKE2bHex(pat(1025)), "7a9e5283a15d13b995755360fde4c65c2ae1bc0cf33e8db2ce8416e5d10697c73fc4b2622a29b938a1faec43d931b02e71ad8635e071265633643a9d9396ec28", "BLAKE2b 1025");
eq(BLAKE2sHex(pat(1025)), "9b4b1bfb89177545cc59b321be5403774c58f061db927f04d206116b8278d2b4", "BLAKE2s 1025");
eq(Murmur3_128Hex(pat(1025)), "876e9ec63ae6ac83372d834685552068", "Murmur3 1025");
eq(BLAKE3Hex(pat(2047)), "58830fbf51a4423c573b164471690570e544cfe793bead46225664796b4b1467", "BLAKE3 2047");
eq(BLAKE2bHex(pat(2047)), "a37d79197d9b3e68df05910f3a8e14b5411f0a4fbd03f00dabe8d3ca5814dfae1bc2fc34deb1eb69c3f2187954330ae234ca0697534645634e9a457996f20703", "BLAKE2b 2047");
eq(BLAKE2sHex(pat(2047)), "7219492142b914fa82db53e9c4acb140ece69f5d012ef54fa91c076982478f46", "BLAKE2s 2047");
eq(Murmur3_128Hex(pat(2047)), "d2dc36e69e0d006212420d8805a48752", "Murmur3 2047");
eq(BLAKE3Hex(pat(2048)), "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a", "BLAKE3 2048");
eq(BLAKE2bHex(pat(2048)), "84ef376f8080d5d19a6914c9b8e8eaf71b3f716f5b4f0da4fdf81b6c465a5656e01b52807011e1fce05e77729aae5422c6424fe241f7ba93da39456e5c5448d9", "BLAKE2b 2048");
eq(BLAKE2sHex(pat(2048)), "e0edc36d40bfa488e118fb944ad9361e1ec72fe8f24570e4ef64876b7e3d1a49", "BLAKE2s 2048");
eq(Murmur3_128Hex(pat(2048)), "708ef5c68d58aa269299901d25deba7a", "Murmur3 2048");
eq(BLAKE3Hex(pat(2049)), "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030", "BLAKE3 2049");
eq(BLAKE2bHex(pat(2049)), "146560fd774a01704fcce96f5f9b4b042ae43c928ad6546fb070b0ec18d2a4ac592578af038a1f6c5b79144fb16a0c6428999d518384d8349a3ec3707aa50ac2", "BLAKE2b 2049");
eq(BLAKE2sHex(pat(2049)), "4ec12e09806c18225b107785a2031e61ee1227412b9c18c5611bc6e54cd8c415", "BLAKE2s 2049");
eq(Murmur3_128Hex(pat(2049)), "be22ed7d28f7a7d65b1cf084c02b2ac7", "Murmur3 2049");
eq(BLAKE3Hex(pat(3072)), "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2", "BLAKE3 3072");
eq(BLAKE2bHex(pat(3072)), "6a4fd5fd8cc0a8e717b28757c896096b0452750684cf7c6c3636f51a98beb32c88f32c9ed7140f90a2cdff2fc4ff49bcaa257f14a6bf6f926530cb47cc7aa340", "BLAKE2b 3072");
eq(BLAKE2sHex(pat(3072)), "13a2b5802ff592e3600d91aa339ce2afe02206510aab762e3142ba844a83ce6c", "BLAKE2s 3072");
eq(Murmur3_128Hex(pat(3072)), "1345738c31c7b5ad14e10832821bdda4", "Murmur3 3072");
eq(BLAKE3Hex(pat(4095)), "0cdbdde4d038f0509412a5d1c3b1fb767d5e8c8a0eb2aae963fd36d1f544791a", "BLAKE3 4095");
eq(BLAKE2bHex(pat(4095)), "d745504d13996a7960709a8061b8be4010e32c1294a8e6372e55069587e967e1766d95c75099ac4742f881ef02820d8fecc6bdbfa0c4340370f290a7dd2989aa", "BLAKE2b 4095");
eq(BLAKE2sHex(pat(4095)), "1db35136887b3fd2c2cbeb7dc4d217da0e23a0a30ae596e660b602b6f5875a46", "BLAKE2s 4095");
eq(Murmur3_128Hex(pat(4095)), "4e71a91890f4316023010aca1fd48038", "Murmur3 4095");
eq(BLAKE3Hex(pat(4096)), "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969", "BLAKE3 4096");
eq(BLAKE2bHex(pat(4096)), "c7a3d6a53bd11772ecf077c1dc9633a39c6fe691ec07a530e0e765c0a9d5a01a16f00995536578b83e54c2821766ac7ac6ae86e22269a5d14208ccac954cc95f", "BLAKE2b 4096");
eq(BLAKE2sHex(pat(4096)), "753200579e43772518340d84db0958f343329f84493e7c69fcf195d4060cb9c5", "BLAKE2s 4096");
eq(Murmur3_128Hex(pat(4096)), "bf418a7d7fa327f75fe46a0a61c18de6", "Murmur3 4096");
eq(BLAKE3Hex(pat(6144)), "3e2e5b74e048f3add6d21faab3f83aa44d3b2278afb83b80b3c35164ebeca205", "BLAKE3 6144");
eq(BLAKE2bHex(pat(6144)), "71b0c49f685263009535c8d90d3cb3983eb39840f5b6e32072ac239f2a5f7fc72d1ef13a5a765a82f1485dfe63b0f5145726940848ca2390a57dc5e719f17b4e", "BLAKE2b 6144");
eq(BLAKE2sHex(pat(6144)), "766b838dbc94e6498b0980d74cb9d2416247da70257594ad2669ee9884775d0d", "BLAKE2s 6144");
eq(Murmur3_128Hex(pat(6144)), "35a8d3d5dcfdf3740b80c5fe3aff2c44", "Murmur3 6144");
eq(BLAKE3Hex(pat(8192)), "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63", "BLAKE3 8192");
eq(BLAKE2bHex(pat(8192)), "6e02a28235a5fea5bb41fe376b384a8f83376b633ae67572d73b4152c94b07a5fadb1478a2debefb3ac30cb5594e0352b108b73163f9e09f260e4f483900a039", "BLAKE2b 8192");
eq(BLAKE2sHex(pat(8192)), "f39291e392e6af194e52755f12a2eb8b1d0671bfe163c4f1b8efb2acb69a9c50", "BLAKE2s 8192");
eq(Murmur3_128Hex(pat(8192)), "7ebe5fd0b6b1c6f1b9a9fd5b7e1a06c3", "Murmur3 8192");
eq(BLAKE3Hex(pat(10000)), "5f81f9e4ab67627b6b036d5d4e3bc40d9d3daa6fcc2b6dd07ab2bbf0a877da54", "BLAKE3 10000");
eq(BLAKE2bHex(pat(10000)), "9e9616f8ed00cd5b3fccbb8e629258f50daa3c05f01cd66f8b0073dd67e615faeec101e16fe991e18979ff45cfb0eaa3b88f834de1ec73f833bb5c4b369c1fe4", "BLAKE2b 10000");
eq(BLAKE2sHex(pat(10000)), "ccc0841dd7c39f8fe87956a69f975c51c7fc2e021e0efc9f566e83060c4df5ce", "BLAKE2s 10000");
eq(Murmur3_128Hex(pat(10000)), "1b80761a4028e80eaf9d7777c0959026", "Murmur3 10000");
eq(BLAKE3Hex(pat(1024), 1), "42", "BLAKE3 xof 1");
eq(BLAKE3Hex(pat(1024), 16), "42214739f095a406f3fc83deb889744a", "BLAKE3 xof 16");
eq(BLAKE3Hex(pat(1024), 31), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855a", "BLAKE3 xof 31");
eq(BLAKE3Hex(pat(1024), 32), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7", "BLAKE3 xof 32");
eq(BLAKE3Hex(pat(1024), 33), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71c", "BLAKE3 xof 33");
eq(BLAKE3Hex(pat(1024), 63), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb4", "BLAKE3 xof 63");
eq(BLAKE3Hex(pat(1024), 64), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb404", "BLAKE3 xof 64");
eq(BLAKE3Hex(pat(1024), 65), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb40475", "BLAKE3 xof 65");
eq(BLAKE3Hex(pat(1024), 127), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb404756f6eeae7883b446b70ebb144527c2075ab8ab204c0086bb22b7c93d465efc57f8d917f0b385c6df265e77003b85102967486ed57db5c5ca170ba441427ed", "BLAKE3 xof 127");
eq(BLAKE3Hex(pat(1024), 128), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb404756f6eeae7883b446b70ebb144527c2075ab8ab204c0086bb22b7c93d465efc57f8d917f0b385c6df265e77003b85102967486ed57db5c5ca170ba441427ed9a", "BLAKE3 xof 128");
eq(BLAKE3Hex(pat(1024), 131), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb404756f6eeae7883b446b70ebb144527c2075ab8ab204c0086bb22b7c93d465efc57f8d917f0b385c6df265e77003b85102967486ed57db5c5ca170ba441427ed9afa684e", "BLAKE3 xof 131");
eq(BLAKE3Hex(pat(1024), 256), "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af71cf8107265ecdaf8505b95d8fcec83a98a6a96ea5109d2c179c47a387ffbb404756f6eeae7883b446b70ebb144527c2075ab8ab204c0086bb22b7c93d465efc57f8d917f0b385c6df265e77003b85102967486ed57db5c5ca170ba441427ed9afa684eb1241e955fcca7c31328676a8175ec3e32956530421123c5d59012992eab705964fd017d6199e8aa520215d12566e89f106b70970d688643a42dcbdba781ae7bccdeaaccf9d8d005c9d5499dcd1e34c25df0e1e621631cb8346451501d71596736a3cb501abfcef176621f22a2f230c8b8c3ef246a114235f221d24012", "BLAKE3 xof 256");
eq(BLAKE2bHex(pat(100), 1), "8c", "BLAKE2b len 1");
eq(BLAKE2bHex(pat(100), 16), "a1af4534b70cd9add253056a6d2d7b6c", "BLAKE2b len 16");
eq(BLAKE2bHex(pat(100), 32), "5ac86383dec1db602fdbc2c978c3fe1bf4328fea1e1b495b68be2c3b67ba033b", "BLAKE2b len 32");
eq(BLAKE2bHex(pat(100), 48), "313b0350c45772ff6787f1e2831cafc6c8d77162be100819e5142f1f398bb0fb3a3123fbff2c94927f7196386952067c", "BLAKE2b len 48");
eq(BLAKE2bHex(pat(100), 64), "6f793eb4374a48b0775acaf9adcf8e45e54270c9475f004ad8d5973e2aca52747ff4ed04ae967275b9f9eb0e1ff75fb4f794fa8be9add7a41304868d103fab10", "BLAKE2b len 64");
eq(BLAKE2sHex(pat(100), 1), "a5", "BLAKE2s len 1");
eq(BLAKE2sHex(pat(100), 16), "666a861d60a4c86bf684fe089cfba8cc", "BLAKE2s len 16");
eq(BLAKE2sHex(pat(100), 20), "680d1a3bda75f684618cbdc2863a76719155ee53", "BLAKE2s len 20");
eq(BLAKE2sHex(pat(100), 32), "81dcc3a505eace3f879d8f702776770f9df50e521d1428a85daf04f9ad2150e0", "BLAKE2s len 32");
eq(Murmur3_128Hex(pat(37), 0), "20d802dd5ead7451b0cd03c799538480", "Murmur3 seed 0");
eq(Murmur3_128Hex(pat(37), 1), "017a0e0503ebe35d11bfd67c08ddb624", "Murmur3 seed 1");
eq(Murmur3_128Hex(pat(37), 42), "57db6b62d866447a576229c6c36b1e19", "Murmur3 seed 42");
eq(Murmur3_128Hex(pat(37), 3735928559), "08dac4a7fa22d325039c769864a02c86", "Murmur3 seed 3735928559");

/* The byte form and the hex form must agree, and the digest must be a real
   Uint8Array rather than an ArrayBuffer (which has no .length and indexes to
   undefined, so a caller's loop would run zero times and report success). */
const b = BLAKE3(pat(100));
eq(b.length, 32, "BLAKE3 returns 32 bytes");
eq(Array.from(b, (x) => x.toString(16).padStart(2, "0")).join(""), BLAKE3Hex(pat(100)),
   "the byte form and the hex form agree");
eq(Murmur3_128(pat(9)).length, 16, "Murmur3-128 is 16 bytes");

function threw(fn, m) { n++; try { fn(); fails++; print("FAIL " + m + ": did not throw"); }
                        catch (e) { } }
threw(() => BLAKE3(), "BLAKE3 with no data");
threw(() => BLAKE3(pat(4), 0), "a zero output length");
threw(() => BLAKE2bHex(pat(4), 65), "BLAKE2b past its 64-byte maximum");
threw(() => BLAKE2sHex(pat(4), 33), "BLAKE2s past its 32-byte maximum");

if (fails) { print("test_blake: " + fails + " FAILED of " + n); throw new Error("test_blake failed"); }
print("test_blake: " + n + " assertions, 0 failures");
