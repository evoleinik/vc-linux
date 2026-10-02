/* tools/build_hack.py links once with the HSTP placeholder, hashes the linked
 * image, then recompiles only this module with that deterministic identity.
 * The final link must differ in exactly these four initialized data bytes.
 * Keeping both links compiler-produced preserves the independent OMF proof.
 */
#ifndef HACK_BUILD_STAMP
#define HACK_BUILD_STAMP 0x50545348UL
#endif
unsigned long hack_build_stamp = HACK_BUILD_STAMP;
