#include "signalhook.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.h"
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
// ---------------------------------------------------------------------------

static const uint32_t kCallSite = 0x49DD4C;
// Where the patched call returns: 0x49DD51 (`mov eax,[esi+0x14]`), three bytes
// before the caller's `add $0x54,%esp` at 0x49DD54. Pushing this is what makes
// the tail `jmp 0x6FC580` indistinguishable from the call that was deleted.
static const uint32_t kResumeAt = kCallSite + 5;
static const uint32_t kCoronaFn = 0x6FC580;
static const uint8_t kExpectedBytes[5] = {0xE8, 0x2F, 0xE8, 0x25, 0x00};
// 0x6FC580 has to be the corona-type thunk the argument layout was read from:
// `movzbl 0x28(%esp),%edx; mov 0xc3e000(,%edx,4),%eax; mov %eax,0x28(%esp)`.
static const uint8_t kCalleeBytes[5] = {0x0F, 0xB6, 0x54, 0x24, 0x28};
// Bytes the stub puts between the entry esp and argument 0: pusha (8 x 4) plus
// pushfl (1 x 4). No return address is in that count, because a jmp pushed none.
static const unsigned kSavedBytes = 36;
// The call site's esp is whatever the game happened to leave it at (0x010BF54C
// in the crash of 2026-11-11 13:04:44), so the stub cannot assume 16-byte
// alignment: it has to realign *before* pushing the recorder's arguments.
static const unsigned kStackAlign = 16;

// Argument indices into the 21-dword block, as recorded above.
enum {
    kArgR = 2,
    kArgG = 3,
    kArgB = 4,
    kArgAlpha = 5,
    kArgPos = 6,
    kArgStrength = 7
};

// CEntity::m_matrix, and CMatrix::at (the third 4-float column).
static const uint32_t kEntityMatrix = 0x14;
static const uint32_t kMatrixAt = 0x20;

// ---------------------------------------------------------------- collection

static RhySignalHead g_heads[kRhySignalHeadCap];
static volatile unsigned g_n = 0;
static volatile unsigned g_calls = 0;
static volatile unsigned g_records = 0;
static volatile unsigned g_dropped = 0;
static volatile unsigned g_bad = 0;
static bool g_installed = false;

// Is [p, p+len) safe to read? The hook runs inside the game's render pass, where
// a single bad pointer is the end of the process, and the arguments it is handed
// come from a call site we patched - so "the game would not have passed a bad
// pointer" is not a guarantee we can rely on.
//
// VirtualQuery per corona would be wasteful, so page results are kept in a small
// MRU: the game's renderer touches the same handful of pages every frame, so a
// miss is rare and a hit is two compares.
static const unsigned kPageBits = 12;
static const uintptr_t kPageSize = 1u << kPageBits;
static const unsigned kPageCache = 8;
static uintptr_t g_pageKey[kPageCache];
static bool g_pageOk[kPageCache];
static unsigned g_pageSlot = 0;

static bool rhyPageReadable(uintptr_t page) {
    for (unsigned i = 0; i < kPageCache; ++i) {
        if (g_pageKey[i] == page) {
            return g_pageOk[i];
        }
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
    g_pageKey[g_pageSlot] = page;
    g_pageOk[g_pageSlot] = ok;
    g_pageSlot = (g_pageSlot + 1) % kPageCache;
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

// Called from the stub below with the arguments the game was about to pass to
// RegisterCorona, plus the two registers the call site still holds. Runs on the
// game's render thread, inside the render pass: it must not log, allocate, call
// into the game, or call Remix. It is a straight copy into a fixed array.
//
// Every pointer here is game memory reached through a call site we patched, and
// a fault on the render thread is the end of the process: the crash of
// 2026-11-11 13:04:44 was this function reading a float (-575.2) as a pointer
// and touching 0xC48F9A27. MinGW's DWARF unwinder gives us no __try/__except,
// and the handlers that are installed in this process (modloader's SEH, the
// Remix bridge's vectored handler) make a fault here unrecoverable anyway.
// So each pointer is checked against the page table before it is dereferenced,
// and a bad one becomes a dropped record.
extern "C" void rhySignalRecordCorona(const uint32_t* args, uint32_t entity,
                                      uint32_t index) {
    // Counted before anything is dereferenced: "calls=0" means the patched call
    // site is never reached, "calls>0 records=0" means it is reached and we are
    // rejecting what it hands us. Those two need very different fixes.
    ++g_calls;
    if (g_n >= kRhySignalHeadCap) {
        ++g_dropped;
        return;
    }
    const uint32_t argsAddr =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(args));
    if (!rhyReadable(argsAddr, 21 * sizeof(uint32_t))) {
        ++g_bad;
        return;
    }
    const uint32_t posBits = args[kArgPos];
    if (!rhyReadable(posBits, 3 * sizeof(float))) {
        ++g_bad;  // not a usable CVector* in this process's address space
        return;
    }
    const float* pos =
        reinterpret_cast<const float*>(static_cast<uintptr_t>(posBits));
    RhySignalHead& h = g_heads[g_n];
    h.entity = entity;
    h.index = index;
    h.pos[0] = pos[0];
    h.pos[1] = pos[1];
    h.pos[2] = pos[2];
    // The entity matrix is guaranteed valid here: DisplayActualLight has just
    // resolved it (it calls CPlaceable::SetupMatrix2 when it is null) and used
    // it to place the very corona we are recording. Still checked, because a
    // modded or partially-built entity would otherwise fault in the render pass.
    h.normal[0] = h.normal[1] = h.normal[2] = 0.0f;
    const uint32_t mBits =
        (rhyReadable(entity + kEntityMatrix, sizeof(uint32_t)))
            ? *reinterpret_cast<const uint32_t*>(
                  reinterpret_cast<uintptr_t>(entity + kEntityMatrix))
            : 0u;
    if (rhyReadable(mBits + kMatrixAt, 3 * sizeof(float))) {
        const float* at = reinterpret_cast<const float*>(
            static_cast<uintptr_t>(mBits) + kMatrixAt);
        h.normal[0] = at[0];
        h.normal[1] = at[1];
        h.normal[2] = at[2];
    }
    h.rgb[0] = static_cast<uint8_t>(args[kArgR] & 0xFFu);
    h.rgb[1] = static_cast<uint8_t>(args[kArgG] & 0xFFu);
    h.rgb[2] = static_cast<uint8_t>(args[kArgB] & 0xFFu);
    h.alpha = static_cast<uint8_t>(args[kArgAlpha] & 0xFFu);
    const uint32_t sBits = args[kArgStrength];
    std::memcpy(&h.strength, &sBits, sizeof h.strength);
    h.tick = GetTickCount();
    ++g_n;
    ++g_records;
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
    static_assert(kArgPos == 6, "kArgPos indexes the CVector* argument");
    static_assert(kSavedBytes == 36, "pusha + pushfl = 36 bytes");
    static_assert(kStackAlign == 16, "stack realignment target");
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
        "i"(kCoronaFn), "i"(kResumeAt)
        : "memory");
}

// ---------------------------------------------------------------- install

const char* rhySignalHookReasonText(int reason) {
    switch (reason) {
        case kSignalHookOk: return "ok";
        case kSignalHookBytes: return "the bytes at 0x49DD4C are not 'call 0x6FC580'";
        case kSignalHookProtect: return "VirtualProtect on the code page failed";
        case kSignalHookRange: return "the stub is more than 2 GB from the call site";
        case kSignalHookCallee:
            return "0x6FC580 is not the corona-type thunk the stub assumes";
        default: return "unknown";
    }
}

bool rhySignalHookInstall(int* reason) {
    if (g_installed) {
        if (reason) {
            *reason = kSignalHookOk;
        }
        return true;
    }
    const uint8_t* at = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(kCallSite));
    // Never patch a call site we have not identified: on any other GTA SA build
    // 0x49DD4C is some other instruction, and overwriting five bytes of it is a
    // crash, not a missing feature. The callee gets the same treatment, because
    // the stub hands it a stack that was rebuilt by hand.
    if (std::memcmp(at, kExpectedBytes, sizeof kExpectedBytes) != 0) {
        if (reason) {
            *reason = kSignalHookBytes;
        }
        return false;
    }
    const uint8_t* callee =
        reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(kCoronaFn));
    if (std::memcmp(callee, kCalleeBytes, sizeof kCalleeBytes) != 0) {
        if (reason) {
            *reason = kSignalHookCallee;
        }
        return false;
    }
    const long long rel = static_cast<long long>(
        reinterpret_cast<uintptr_t>(&rhySignalCoronaStub)) -
        static_cast<long long>(kCallSite + 5);
    if (rel < -0x80000000ll || rel > 0x7FFFFFFFll) {
        if (reason) {
            *reason = kSignalHookRange;
        }
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(static_cast<uintptr_t>(kCallSite)),
                        sizeof kExpectedBytes, PAGE_EXECUTE_READWRITE, &old)) {
        if (reason) {
            *reason = kSignalHookProtect;
        }
        return false;
    }
    uint8_t* slot = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kCallSite));
    const int32_t rel32 = static_cast<int32_t>(rel);
    slot[0] = 0xE9;  // jmp rel32
    std::memcpy(slot + 1, &rel32, sizeof rel32);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof kExpectedBytes);
    DWORD back = 0;
    VirtualProtect(slot, sizeof kExpectedBytes, old, &back);
    g_installed = true;
    if (reason) {
        *reason = kSignalHookOk;
    }
    rhyLog("signalhook: patched 0x%08X (was 'call 0x%08X') -> stub at 0x%08X, "
           "cap %d heads per frame",
           kCallSite, kCoronaFn,
           static_cast<unsigned>(
               reinterpret_cast<uintptr_t>(&rhySignalCoronaStub)),
           kRhySignalHeadCap);
    return true;
}

bool rhySignalHookInstalled() { return g_installed; }
unsigned rhySignalHookCalls() { return g_calls; }
unsigned rhySignalHookRecords() { return g_records; }
unsigned rhySignalHookDropped() { return g_dropped; }
unsigned rhySignalHookBad() { return g_bad; }

unsigned rhySignalHeadsTake(RhySignalHead* out, unsigned outCap) {
    // Same thread as the hook (the game's render thread), so no lock: the hook
    // has already run for this frame by the time hkPresent gets here, and it
    // cannot run again until this frame's rendering restarts.
    const unsigned n = (g_n < outCap) ? g_n : outCap;
    if (n > 0) {
        std::memcpy(out, g_heads, n * sizeof(RhySignalHead));
    }
    g_n = 0;
    return n;
}
