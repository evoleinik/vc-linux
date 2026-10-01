/* Read-only browser inspection. Nothing in this interface exists in the
 * native build, and neither entry point may run guest code or suspend.
 *
 * vc_source_snapshot() calls Module.vcSourceSnapshot with owned JS values:
 *   {version:1, raw:{cs,ip,ss,sp},
 *    current:{image,offset,cs,ip,kind,returnCS,returnIP},
 *    stack:[{sp,word,near:Location|null,far:Location|null}],
 *    recent:[Location], stackTruncated, historyCapacity, historyFull}
 * A Location is {image,offset,cs,ip}; image is Image.name, and offset is a
 * canonical load-module address, including for code copied by the guest.
 * current.image is null when unresolvable. "interrupt" identifies a checked
 * INT instruction before the real stacked return address; "continuation"
 * identifies the stacked continuation when no such instruction is proved.
 * "instruction" is the unmodified current CS:IP outside a host stub.
 *
 * Stack candidates are only address resolutions, NEVER proved callers. JS
 * must require that the listing has a CALL ending at the candidate offset.
 * near uses current.cs; far uses the following stack word as CS. After a
 * verified far return, JS can resolve later near words in its caller's CS.
 *
 * vc_source_resolve(cs,ip,preceding) calls Module.vcSourceResolved with a
 * Location or null. preceding=1 checks the bytes ending at the address (a
 * return address); 0 checks bytes starting there (execution). These helpers
 * use the dispatcher's approved relocated bytes and already observed copies,
 * without registering a copy or executing an Image.run probe.
 */
#ifndef VC_WEB_SOURCE_H
#define VC_WEB_SOURCE_H

#ifdef __EMSCRIPTEN__
void vc_source_snapshot(void);
void vc_source_resolve(unsigned cs, unsigned ip, int preceding);
#endif

#endif
