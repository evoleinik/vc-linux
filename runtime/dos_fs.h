/* Additional filesystem entry points; the HLE interface lives in hle.h. */
#ifndef VC_DOS_FS_H
#define VC_DOS_FS_H

/* Body of the runtime's far-callable F000:0100 country case-map stub.
 * Only AL changes, and only extended CP866 letters are uppercased. */
void dos_casemap_upper(void);

#endif
