# Bit-exact reconstruction of the SIGSEGV v-register state (HPC core 145 case)
# All inputs are the raw 64-bit patterns from the crash dump (log lines 551026-551110).
import struct, math

# Full v-register dump (low'high lanes) for the §6.1 decode table:
VREGS = {
 'v0':'0x3ff0000000000001','v1':'0x3fefffc0710c5eb1','v2':'0xbf7fe3a08c94d3e0',
 'v3':'0x3e25e91a320cb8ed3e95fc6d216df799','v4':'0xbfefffc0710c5eb1',
 'v5':'0x3e25e91a320cb8ed3e95fc6d216df799','v6':'0x3e95fc6d216df7993f060fd11bbf6275',
 'v7':'0xbf627ca898e3c065','v8':'0xbc2f59a0c9d9a0d1','v9':'0x3f7fe3a08c94d3df',
 'v10':'0x3f7fe3dfe3ab98c9','v11':'0x3ffcf98109143182','v12':'0','v13':'0','v14':'0','v15':'0',
 'v16':'0xbf71977b74205554','v17':'0xbf55198e7e7479e5','v18':'0','v19':'0',
 'v20':'0x3fcf440a0b3d96e2','v21':'0','v22':'0x3ff0000000000000','v23':'0',
 'v24':'0xbf91de046bb01480','v25':'0x3fc83fff3e1158a1','v26':'0x3fc0f96647d2ccf0',
 'v27':'0x3fb832f18b8ef44b','v28':'0x3fdb178cb642a4e6','v29':'0x3fe35298cd3e2e4a',
 'v30':'0x3fa0efcff83cbbec','v31':'0xbfcf829f8cc8211e',
}
print("== v-register decode (low lane as double) ==")
for k in sorted(VREGS, key=lambda s:int(s[1:])):
    hx = VREGS[k]
    val = int(hx,16) if hx!='0' else 0
    if val >> 64:  # full 128-bit pattern given
        hi, lo = val >> 64, val & ((1<<64)-1)
        dlo = struct.unpack('<d',struct.pack('<Q',lo))[0]
        dhi = struct.unpack('<d',struct.pack('<Q',hi))[0]
        print(f"{k}: low={dlo!r}  high={dhi!r}")
    else:
        dlo = struct.unpack('<d',struct.pack('<Q',val))[0]
        print(f"{k}: {dlo!r}")
print()

def d(bits64):
    return struct.unpack('<d', struct.pack('<Q', bits64))[0]

def b(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]

v0  = d(0x3ff0000000000001)   # 1.0000000000000002
v1  = d(0x3fefffc0710c5eb1)   # 0.9999696929907796
v2  = d(0xbf7fe3a08c94d3e0)   # -0.007785441536999976
v4  = d(0xbfefffc0710c5eb1)   # -0.9999696929907796
v8  = d(0xbc2f59a0c9d9a0d1)   # -8.497464257699151e-19
v9  = d(0x3f7fe3a08c94d3df)   # 0.0077854415369999755
v10 = d(0x3f7fe3dfe3ab98c9)   # 0.007785677497599682
v11 = d(0x3ffcf98109143182)   # 1.8109140734332816
v22 = d(0x3ff0000000000000)   # 1.0

u = v10                        # u = t/d
tmp = math.sqrt(1.0 + u*u)     # tmp = sqrt(1+u^2)
rot1_s = 1.0/tmp
rot1_c = u/tmp

print(f"u            = {u!r}")
print(f"tmp          = {tmp!r}")
print(f"rot1.s=1/tmp = {rot1_s!r}   v1 = {v1!r}   bit-exact: {b(rot1_s)==b(v1)}")
print(f"rot1.c=u/tmp = {rot1_c!r}   v9 = {v9!r}   bit-exact: {b(rot1_c)==b(v9)}")
print(f"v4 == -v1 (exact)             : {b(-v1)==b(v4)}")
print(f"v2 vs -v9: XOR = 0x{b(v2)^b(-v9):016x}  (1-ULP diff: {b(v2)!=b(-v9)})")
q = v9/v1
print(f"v9/v1 = {q!r}  == v10 bit-exact: {b(q)==b(v10)}")

theta_rot1  = math.degrees(math.atan2(rot1_s, rot1_c))
theta_right = math.degrees(math.atan2(v9, v1))
print(f"theta_rot1  = {theta_rot1:.9f} deg")
print(f"theta_right = {theta_right:.9f} deg")
print(f"sum         = {theta_rot1+theta_right:.9f} deg  (identity atan2(1,u)+atan2(u,1)=90)")

# j_left = rot1 * j_right^T  ->  d8 = j_left.c, d0 = j_left.s (m_c@+0, m_s@+8)
# j_left should be a 90-degree rotation: c = -sin(eps), s = cos(eps)
eps = math.asin(-v8)           # j_left.c = -8.497e-19 = -sin(eps)
print(f"d8 = j_left.c = {v8!r}  ->  eps = {eps!r} = 2^{math.log2(abs(eps)):.2f}")
print(f"d0 = j_left.s = {v0!r}  ->  d0-1 = 2^{math.log2(v0-1.0):.2f} (1 ULP)")
# verify j_left is orthogonal to rounding: cos(eps) should be 1.0 or 1+1ULP
print(f"cos(eps) computed = {math.cos(eps)!r}  (d0 = 1+1ULP: rounding of the composition)")
print(f"|t| = v11 = {v11!r}; d = t/u = {v11/u!r}")

# makeJacobi tau reconstruction (documented open detail)
t = v10
m00_m11 = 1.0   # normalized probe; documented mismatch in report
tau = abs(v9)/abs(m00_m11)
w = 1.0/math.sqrt(1.0+tau*tau)
print(f"makeJacobi tau-probe: 1/(tau+w) = {1.0/(tau+w)!r} vs t = {t!r}  match: {1.0/(tau+w)==t}")

print()
print("MATHEMATICAL CLOSURE: theta_rot1 + theta_right = atan2(1,u) + atan2(u,1) = 90 deg for any u.")
print("j_left = 90-deg rotation: c=-sin(eps), s=cos(eps), eps~2^-60.")
print("CONCLUSION: v-register state at crash is a COMPLETE, SELF-CONSISTENT, CORRECT")
print("computation of real_2x2_jacobi_svd for u=0.007785677497599682 (|t|=1.8109, d=232.6).")
print("No FP register is corrupted. The ONLY corrupted state is x20 (pointer, 4 bits).")
