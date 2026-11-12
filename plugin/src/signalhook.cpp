#include "signalhook.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.h"
#include "config.h"
#include "signalheads.h"

// ---------------------------------------------------------------------------
// Where and why
//
// CTrafficLights::DisplayActualLight(CEntity*) is at 0x49DAB0 in the GTA SA 1.0
// US exe. Its loop over the model's 2DFX entries ends, for the head that is
// currently lit, with a 21-argument call to CCoronas::RegisterCorona (0x6FC580,
// a thunk that maps the corona type through a table and tail-calls 0x6FC180):
//
//   0x49DD4C  E8 2F E8 25 00      call 0x6FC580
//   0x49DD51  ...                 (add $0x54,%esp afterwards: 21 args, 84 bytes)
//
// Argument layout at that call site, established by walking esp on both sides of
// the call and cross-checked against the callee's stores into the corona array
// at 0xC3E064 (position at store+0x58, r/g/b at +0x84/0x85/0x86) - full evidence
// in .strata/notes-s01-signal-heads.md:
//
//   [0]  entity + head index      [7]  strength (float)
//   [1]  CEntity* (0 here)        [8]  range   (float, 50.0)
//   [2]  r                        [9]  corona type (0)
//   [3]  g                        [18] corona radius (float, 15.0)
//   [4]  b
//   [5]  alpha (255)
//   [6]  CVector* world position  <-- the lens, in world coordinates
//
// Patching that one call is better than hooking 0x6FC580 itself: 0x6FC580 is
// shared by 28 call sites (vehicle lights, searchlights, explosions) and its
// arguments do not identify the entity, while this site is traffic heads only
// and still has esi = the entity and ebx = the head index in registers.
//
// Two traps, and we have now fallen into both of them once.
//
// 1. A `call` pushes a return address; the `jmp` that replaces it does not. So
//    on entry to the stub esp points at argument 0, not at a return address: the
//    argument block is at esp+36 (not esp+40) and the stub has to push a return
//    address itself before jumping to the callee. Getting that wrong is not a
//    missing feature: the callee reads every argument one dword late and its
//    `ret` pops argument 0 (entity + head index) as the return address, which
//    derails the game's whole call chain (crash of 2026-11-11 12:04:33).
// 2. The call site's esp is not 16-byte aligned - it is whatever the game's
//    renderer left it at. Realigning with `and $-16,%esp` *after* pushing the
//    recorder's arguments moves esp below those arguments, and the recorder then
//    reads the game's own stack as its arguments. That is the crash of
//    2026-11-11 13:04:44: esp was 0x010BF54C, the shift was 12 bytes, `args`
//    came out as 0xC48F9A0F (the float -575.2) and reading args[6] at
//    0xC48F9A27 faulted inside the render pass. The realignment now happens
//    before any argument is pushed, and the argument block is addressed off ebp,
//    which the realignment cannot move. Post-mortem at the end of
//    .strata/notes-s01-signal-heads.md.
//
// s19 re-read the whole detour against the disassembly (stack depth per
// instruction on both sides of it in .strata/notes-s19-signal-hook-hardening.md)
// and added four things:
//
//   * a kill switch in rhySignalHookInstall(): no patch unless
//     [trafficlights] source=game AND the bytes at the call site, at the callee
//     and - new - at the caller's `add esp,0x54` all say what the stub assumes.
//     Any mismatch logs the reason and leaves the module on source=list.
//   * a re-entrancy guard: the recorder writes a shared array and a shared page
//     cache with no lock, so a second entry while it is running is refused and
//     counted, never interleaved.
//   * page-cache invalidation once a frame: a page cached as readable in one
//     frame may be decommitted in the next, and a stale "yes" is a fault.
//   * the decision logic moved to signalheads.cpp (rhySignalPatchCheck,
//     rhyCollectHead, the page cache), where tests/signals_check.cpp can run
//     every path against fake memory instead of hoping the game shows a bug.
//
// What the recorder must never do, and why: it is called from the naked stub,
// whose .eh_frame FDE is empty (objdump -W shows pc=stub..stub+0x29 with only
// DW_CFA_nop, so the unwinder would resume with the CIE's "eip = [esp]", which
// is argument 0). An exception that escaped the recorder, or a SEH unwind that
// had to cross the stub, would therefore land on a data value. So the recorder
// allocates nothing, logs nothing, calls no game or Remix function, and touches
// no container: rhyCollectHead is straight-line code over a fixed array, and
// every pointer is validated before it is read.
// ---------------------------------------------------------------------------

// The addresses themselves live in signalheads.h (kRhySignalCallSite,
// kRhySignalResumeAt, kRhySignalCleanupAt, kRhySignalCoronaFn) so the tests can
// name the same constants the stub bakes into its immediates.
// Bytes the stub puts between the entry esp and argument 0: pusha (8 x 4) plus
// pushfl (1 x 4). No return address is in that count, because a jmp pushed none.
static const unsigned kSavedBytes = 36;
// The call site's esp is whatever the game happened to leave it at (0x010BF54C
// in the crash of 2026-11-11 13:04:44), so the stub cannot assume 16-byte
// alignment: it has to realign *before* pushing the recorder's arguments.
static const unsigned kStackAlign = 16;

// ---------------------------------------------------------------- collection

static RhySignalCollector g_col;
static bool g_installed = false;

// Is [p, p+len) safe to read? The hook runs inside the game's render pass, where
// a single bad pointer is the end of the process, and the arguments it is handed
// come from a call site we patched - so "the game would not have passed a bad
// pointer" is not a guarantee we can rely on.
//
// VirtualQuery per corona would be wasteful, so page results are kept in the
// small MRU from signalheads.cpp: the game's renderer touches the same handful
// of pages every frame, so a miss is rare and a hit is two compares. The cache
// is cleared once a frame (in rhySignalHeadsTake), because a page cached as
// readable in one frame can be freed and decommitted in the next, and a stale
// "yes" is exactly the fault the checks exist to prevent.
static const unsigned kPageBits = 12;
static const uintptr_t kPageSize = 1u << kPageBits;
static RhyPageCache g_pages;

static bool rhyPageReadable(uintptr_t page) {
    int hit = rhyPageCacheFind(&g_pages, static_cast<uint32_t>(page));
    if (hit >= 0) {
        return hit != 0;
    }
    MEMORY_BASIC_INFORMATION mbi;
    bool ok = false;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) ==
            sizeof mbi &&
        mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0) {
        switch (mbi.Protect & 0xFFu) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                ok = true;
                break;
            default:
                break;  // NOACCESS, execute-only, write-combine
        }
    }
    rhyPageCachePut(&g_pages, static_cast<uint32_t>(page), ok ? 1 : 0);
    return ok;
}

static bool rhyReadable(uint32_t addr, unsigned len) {
    if (!rhyRangePlausible(addr, len)) {
        return false;
    }
    const uintptr_t first = addr & ~(kPageSize - 1);
    const uintptr_t last = (addr + len - 1) & ~(kPageSize - 1);
    for (uintptr_t pg = first; pg <= last; pg += kPageSize) {
        if (!rhyPageReadable(pg)) {
            return false;
        }
    }
    return true;
}

// The reader rhyCollectHead() runs every pointer through: validate, then copy.
// `ctx` is unused (the collector is a process-wide singleton and the addresses
// are process addresses).
static int rhyGameRead(void* ctx, uint32_t addr, void* dst, unsigned len) {
    (void)ctx;
    if (!rhyReadable(addr, len)) {
        return 0;
    }
    std::memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)),
                len);
    return 1;
}

// Called from the stub below with the arguments the game was about to pass to
// RegisterCorona, plus the two registers the call site still holds (esi = the
// CEntity*, ebx = the 2DFX head index - both confirmed against the disassembly:
// esi comes from [esp+0xac] in the prologue, ebx is the loop counter compared
// against the model's 2DFX count at 0x49DE21). Runs on the game's render thread,
// inside the render pass.
//
// This is only the machine-code-facing shim. The decision path - re-entrancy,
// the array cap, which pointer to validate, what to copy, which counter to bump
// - is rhyCollectHead() in signalheads.cpp, where tests/signals_check.cpp runs
// every one of those paths against a fake memory image. The crash of
// 2026-11-11 13:04:44 was this function reading the float -575.2 as a pointer
// and touching 0xC48F9A27; that path is now a test case, not a post-mortem.
extern "C" int rhySignalRecordCorona(const uint32_t* args, uint32_t entity,
                                     uint32_t index) {
    return rhyCollectHead(&g_col,
                          static_cast<uint32_t>(
                              reinterpret_cast<uintptr_t>(args)),
                          entity, index, GetTickCount(), &rhyGameRead, nullptr);
}

// The detour. The call site is patched with `jmp stub`, so on entry esp points
// at argument 0: the return address the deleted call would have pushed is
// missing, and the stub has to put it back itself.
//
//   pusha / pushfl              save every register and the flags (36 bytes)
//   mov ebp,esp / and $-16,esp  realign NOW, before any argument exists
//   lea 36(%ebp),%eax           the argument block (36 saved, no return address)
//   push 0 / ebx / esi / eax    padding, head index, entity, arguments
//   call recorder               C++ code, sees nothing but a copy job
//   mov esp,ebp / popfl / popa  back to exactly the registers the call expected
//   push 0x49DD51               the return address the call would have pushed
//   jmp 0x6FC580                the original callee, on a stack it cannot tell apart
//
// The callee's ret then lands on 0x49DD51 and the game runs its own
// `add $0x54,%esp` at 0x49DD54, so the caller's stack is balanced again.
//
// The order of the realign and the pushes is not cosmetic. The previous version
// pushed the three arguments first and only then did `and $-16,%esp`; on a call
// site whose esp is not already 16-aligned that moves esp *below* the arguments
// that were just pushed, so the recorder read the game's own stack as its
// arguments (crash of 2026-11-11 13:04:44: esp 0x010BF54C, 12 bytes of shift,
// args became 0xC48F9A0F and `args[6]` faulted). Realignment therefore happens
// while esp still points only at saved registers, and the argument block is
// addressed off ebp, which is immune to the realignment.
__attribute__((naked)) static void rhySignalCoronaStub(void) {
    static_assert(kRhyArgPos == 6, "kRhyArgPos indexes the CVector* argument");
    static_assert(kSavedBytes == 36, "pusha + pushfl = 36 bytes");
    static_assert(kStackAlign == 16, "stack realignment target");
    static_assert(kRhySignalResumeAt == kRhySignalCallSite + 5u,
                  "the resume point is the instruction after the patched call");
    static_assert(kRhySignalCleanupAt == kRhySignalResumeAt + 3u,
                  "the caller's add esp,0x54 is three bytes after it");
    __asm__ volatile(
        "pusha\n\t"
        "pushfl\n\t"
        "movl %%esp, %%ebp\n\t"      // ebp = the 36-byte saved block
        "andl $-16, %%esp\n\t"       // realign before anything is pushed
        "pushl $0\n\t"               // padding: esp is 16-aligned at the call
        "leal 36(%%ebp), %%eax\n\t"  // the deleted call's argument block
        "pushl %%ebx\n\t"            // arg3: head index
        "pushl %%esi\n\t"            // arg2: CEntity*
        "pushl %%eax\n\t"            // arg1: the argument block itself
        "movl %0, %%eax\n\t"
        "call *%%eax\n\t"
        "movl %%ebp, %%esp\n\t"      // drop the arguments and the padding
        "popfl\n\t"
        "popa\n\t"
        "movl %2, %%eax\n\t"
        "pushl %%eax\n\t"
        "movl %1, %%eax\n\t"
        "jmp *%%eax\n\t" ::"i"(&rhySignalRecordCorona),
        "i"(kRhySignalCoronaFn), "i"(kRhySignalResumeAt)
        : "memory");
}

// ---------------------------------------------------------------- install

const char* rhySignalHookReasonText(int reason) {
    switch (reason) {
        case kSignalHookOk: return "ok";
        case kSignalHookBytes:
            return "the bytes at 0x49DD4C are not 'call 0x6FC580'";
        case kSignalHookProtect: return "VirtualProtect on the code page failed";
        case kSignalHookRange: return "the stub is more than 2 GB from the call site";
        case kSignalHookCallee:
            return "0x6FC580 is not the corona-type thunk the stub assumes";
        case kSignalHookSource:
            return "[trafficlights] source is not game (the hook is switched off)";
        case kSignalHookCleanup:
            return "the caller's 'add esp,0x54' stack cleanup is missing";
        case kSignalHookUnreadable:
            return "the patch site or the callee could not be read";
        default: return "unknown";
    }
}

// s30: resize the game's own signal-head corona. The game registers every lit
// head with radius 15.0 game units, i.e. a 30 m wide additive pure-green sprite;
// the Remix bridge renders that as emissive geometry, and a street full of them
// is the "ambient signal glow" of the 2026-11-12 report (measured: 10-39% of the
// signal-stop frames are saturated green, and not one green pixel in any of them
// reaches luma 200 - a huge area at moderate radiance is ambient, not a bulb).
// The radius is an immediate in the caller, so this is a 4-byte code patch next
// to the call-site patch: verified against the exe before it is written, and
// idempotent, so it can be re-applied when rhyremix.ini is edited - which is how
// rhY A/Bs corona_radius without restarting the game. iniRadius <= 0 restores
// the game's own 15.0.
bool rhySignalCoronaApply(float iniRadius) {
    uint8_t pushBytes[5] = {0};
    if (!rhyGameRead(nullptr, kRhySignalCoronaPush, pushBytes, 5)) {
        rhyLog("signalhook: cannot read 0x%08X to check the corona radius push",
               kRhySignalCoronaPush);
        return false;
    }
    float gameRadius = 0.0f;
    if (rhySignalCoronaPatchCheck(pushBytes, &gameRadius) != kRhyPatchOk) {
        // Either we already resized it (the imm32 is no longer 15.0) or this is
        // a build where the radius is not a push immediate. Read the value that
        // is actually there so the wanted radius is computed from the game's own
        // number, and only touch it if it is one we recognises as ours.
        if (!rhyGameRead(nullptr, kRhySignalCoronaRadiusImm,
                         reinterpret_cast<uint8_t*>(&gameRadius),
                         sizeof gameRadius)) {
            return false;
        }
        if (!(gameRadius > 0.0f) || gameRadius > 15.0f) {
            rhyLog("signalhook: 0x%08X is not the 'push 15.0f' the corona radius "
                   "lives in (bytes %02X %02X %02X %02X %02X): leaving the game's "
                   "corona size alone", kRhySignalCoronaPush, pushBytes[0],
                   pushBytes[1], pushBytes[2], pushBytes[3], pushBytes[4]);
            return false;
        }
    }
    const float want = rhySignalCoronaRadius(15.0f, iniRadius);
    if (want == gameRadius) {
        return false;  // already what the ini asks for
    }
    DWORD old = 0;
    const uintptr_t page = kRhySignalCoronaPush & ~0xFFFu;
    if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000u,
                        PAGE_EXECUTE_READWRITE, &old)) {
        rhyLog("signalhook: VirtualProtect failed for the corona radius page");
        return false;
    }
    uint32_t bits = 0;
    std::memcpy(&bits, &want, sizeof bits);
    uint8_t* slot = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(kRhySignalCoronaRadiusImm));
    std::memcpy(slot, &bits, sizeof bits);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(page), 0x1000u);
    DWORD back = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000u, old, &back);
    rhyLog("signalhook: signal-head corona radius %.2f -> %.2f game units at "
           "0x%08X (the game's own %.0f m green flare becomes a lens-sized bulb; "
           "corona_radius=0 puts it back)", gameRadius, want,
           kRhySignalCoronaRadiusImm, want * 2.0f);
    return true;
}

bool rhySignalHookInstall(int* reason) {
    if (g_installed) {
        if (reason) {
            *reason = kSignalHookOk;
        }
        return true;
    }
    // The kill switch, in the one place every install attempt has to come
    // through: the hook exists only if the user asked for it AND the code we
    // are about to overwrite is the code the stub was written against. On any
    // other GTA SA build - or after another mod moved the code - 0x49DD4C is
    // some other instruction, and overwriting five bytes of it is a crash, not
    // a missing feature.
    const RhyConfig* cfg = rhyConfigGet();
    // rhySignalPatchCheck() answers the source question before it looks at any
    // byte, so calling it with no byte windows at all is the source gate on its
    // own: with source=list the exe is never read and never written. It answers
    // kRhyPatchNoBytes when the source is right and the bytes have not been
    // fetched yet - that is the signal to go and read them, and since s30 it is
    // no longer mistaken for "the bytes are wrong" (which is how the hook ended
    // up uninstallable on every build from s19 to here).
    int why = rhySignalPatchCheck(cfg->traffic_source == 1, nullptr, nullptr,
                                  nullptr, nullptr);
    uint8_t callSite[5] = {0};
    uint8_t callee[5] = {0};
    uint8_t cleanup[3] = {0};
    rhyPageCacheInit(&g_pages);
    if (why == kRhyPatchNoBytes) {
        // Read the three windows through the same pointer checks the recorder
        // uses: an unmapped patch site would take the process down inside
        // memcmp, which is a strange way to discover that the exe is odd.
        if (!rhyGameRead(nullptr, kRhySignalCallSite, callSite, 5) ||
            !rhyGameRead(nullptr, kRhySignalCoronaFn, callee, 5) ||
            !rhyGameRead(nullptr, kRhySignalCleanupAt, cleanup, 3)) {
            if (reason) {
                *reason = kSignalHookUnreadable;
            }
            rhyLog("signalhook: cannot read 0x%08X / 0x%08X / 0x%08X to check "
                   "them: not patching", kRhySignalCallSite, kRhySignalCoronaFn,
                   kRhySignalCleanupAt);
            return false;
        }
        uint32_t target = 0;
        why = rhySignalPatchCheck(1, callSite, callee, cleanup, &target);
        if (why == kRhyPatchCleanup) {
            rhyLog("signalhook: 0x%08X does not hold 'add esp,0x54' "
                   "(found %02X %02X %02X): the 21-argument cleanup the stub "
                   "relies on is not there", kRhySignalCleanupAt,
                   cleanup[0], cleanup[1], cleanup[2]);
        } else if (why == kRhyPatchCallSite) {
            rhyLog("signalhook: 0x%08X does not call 0x%08X (bytes %02X %02X "
                   "%02X %02X %02X, target 0x%08X)", kRhySignalCallSite,
                   kRhySignalCoronaFn, callSite[0], callSite[1], callSite[2],
                   callSite[3], callSite[4], target);
        }
    }
    if (why != kRhyPatchOk) {
        if (reason) {
            *reason = (why == kRhyPatchSource)      ? kSignalHookSource
                    : (why == kRhyPatchCallee)      ? kSignalHookCallee
                    : (why == kRhyPatchCleanup)     ? kSignalHookCleanup
                                                    : kSignalHookBytes;
        }
        return false;
    }
    const long long rel = static_cast<long long>(
        reinterpret_cast<uintptr_t>(&rhySignalCoronaStub)) -
        static_cast<long long>(kRhySignalCallSite + 5u);
    if (rel < -0x80000000ll || rel > 0x7FFFFFFFll) {
        if (reason) {
            *reason = kSignalHookRange;
        }
        return false;
    }
    DWORD old = 0;
    // Both writes (the 5-byte jmp at 0x49DD4C and the 4-byte radius immediate at
    // 0x49DCDA) sit in the same 4 KiB page, so open that page once. VirtualProtect
    // works at page granularity anyway; naming the page here keeps "did we make
    // the byte we write writable?" answerable from the code.
    const uintptr_t patchPage = kRhySignalCallSite & ~0xFFFu;
    static_assert((kRhySignalCallSite & ~0xFFFu) ==
                      (kRhySignalCoronaPush & ~0xFFFu),
                  "the two patch sites must share a code page");
    if (!VirtualProtect(reinterpret_cast<void*>(patchPage), 0x1000u,
                        PAGE_EXECUTE_READWRITE, &old)) {
        if (reason) {
            *reason = kSignalHookProtect;
        }
        return false;
    }
    uint8_t* slot =
        reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kRhySignalCallSite));
    const int32_t rel32 = static_cast<int32_t>(rel);
    slot[0] = 0xE9;  // jmp rel32
    std::memcpy(slot + 1, &rel32, sizeof rel32);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(patchPage), 0x1000u);
    DWORD back = 0;
    VirtualProtect(reinterpret_cast<void*>(patchPage), 0x1000u, old, &back);
    // The corona radius is a separate write with its own protection, applied
    // only once the hook itself is agreed to: with source=list or a mismatched
    // exe nothing at all is written to the game's code.
    const bool corona = rhySignalCoronaApply(cfg->traffic_corona_radius);
    g_installed = true;
    if (reason) {
        *reason = kSignalHookOk;
    }
    rhyLog("signalhook: patched 0x%08X (was 'call 0x%08X') -> stub at 0x%08X, "
           "cap %d heads per frame, corona %s", kRhySignalCallSite,
           kRhySignalCoronaFn,
           static_cast<unsigned>(
               reinterpret_cast<uintptr_t>(&rhySignalCoronaStub)),
           kRhySignalHeadCap, corona ? "resized to a bulb" : "left as the game drew it");
    return true;
}

bool rhySignalHookInstalled() { return g_installed; }
unsigned rhySignalHookCalls() { return g_col.calls; }
unsigned rhySignalHookRecords() { return g_col.records; }
unsigned rhySignalHookDropped() { return g_col.dropped; }
unsigned rhySignalHookBad() { return g_col.bad; }
unsigned rhySignalHookReentrant() { return g_col.reentrant; }

unsigned rhySignalHeadsTake(RhySignalHead* out, unsigned outCap) {
    // Same thread as the hook (the game's render thread), so no lock: the hook
    // has already run for this frame by the time hkPresent gets here, and it
    // cannot run again until this frame's rendering restarts.
    const unsigned n = rhyCollectorTake(&g_col, out, outCap);
    // One frame of page-cache staleness, never more: the cache is what keeps
    // VirtualQuery out of the render pass, and a page the game freed between
    // frames must not stay "readable".
    rhyPageCacheClear(&g_pages);
    return n;
}
