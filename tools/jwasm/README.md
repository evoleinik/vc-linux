# JWasm

`jwasm` is JWasm built from https://github.com/Baron-von-Riedesel/JWasm at commit
`a7c6e70a63cf364b05e44decafc338b4915a8bb0`, the same commit ddanila/vc vendors:

    make -f GccUnix.mak "extra_c_flags=-DNDEBUG -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"

It is built without `_FORTIFY_SOURCE` because the fortified binary aborts with "buffer overflow
detected" while writing the listing for `VC.ASM`. The assembled `VC.COM` is identical either way.
JWasm is under the Sybase Open Watcom Public License 1.0.
