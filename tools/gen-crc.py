IEEE=0xEDB88320; CAST=0x82F63B78
def tables(poly):
    t=[[0]*256 for _ in range(8)]
    for n in range(256):
        c=n
        for _ in range(8):
            c = (c>>1) ^ (poly & -(c & 1))
            c &= 0xFFFFFFFF
        t[0][n]=c
    for n in range(256):
        c=t[0][n]
        for k in range(1,8):
            c = t[0][c & 0xFF] ^ (c >> 8)
            c &= 0xFFFFFFFF
            t[k][n]=c
    return t
def emit(name,t,out):
    out.append(f"static const uint32_t {name}[8][256] = {{")
    for row in t:
        out.append("{")
        for i in range(0,256,6):
            out.append("    "+",".join(f"0x{v:08X}u" for v in row[i:i+6])+",")
        out.append("},")
    out.append("};\n")
ieee=tables(IEEE); cast=tables(CAST)
out=["""/* Slice-by-8 CRC tables, generated from the polynomials (see tools/gen-crc.py).
 * Row j holds the contribution of a byte j positions back in the window, so
 * eight bytes are consumed per iteration instead of one. static const: no
 * initialiser, no lazily-built static, nothing to race. */
#ifndef DYN_CRC_TABLES_H
#define DYN_CRC_TABLES_H
"""]
emit("crc32_ieee_s8",ieee,out); emit("crc32c_s8",cast,out)
out.append("#endif")
import sys; (open(sys.argv[1],'w') if len(sys.argv)>1 else sys.stdout).write("\n".join(out)+"\n")
# proof: row 0 must equal the table currently in dyn-hash.c
print("gen row0[1] ieee = 0x%08X (expect 0x77073096)"%ieee[0][1])
print("gen row0[1] cast = 0x%08X (expect 0xF26B8303)"%cast[0][1])
print("gen row1[1] ieee = 0x%08X (expect 0x191B3141)"%ieee[1][1])
print("gen row7[1] cast = 0x%08X (expect 0x493C7D27)"%cast[7][1])
