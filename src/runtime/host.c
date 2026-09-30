/*
 * host.c - the SimCity 2000 recomp host: maps SIMCITY.EXE, runs its lifted
 * code, and connects it to the real Win32 API it was written against.
 *
 * The design rests on one fact: this is a 32-bit Windows process running a
 * 32-bit Windows program. So the original image is mapped at its own base
 * (0x00400000) by the Windows loader, every guest pointer is a host pointer,
 * and almost nothing needs translating. Three mechanisms do the rest:
 *
 *   guest -> native  The IAT holds real API addresses, as the Windows loader
 *                    would leave it. A dispatch to any VA outside the guest
 *                    image goes through native_call32, which copies the
 *                    guest's argument words to the host stack and measures
 *                    how many the callee popped. No per-API argument tables.
 *   native -> guest  Guest .text is mapped non-executable. When Win32 calls a
 *                    guest function pointer (window procedure, MFC's CBT hook,
 *                    a thread start, a timer) the CPU faults on the execute;
 *                    the vectored handler redirects to callback_entry, which
 *                    runs the lifted function on the guest stack and returns
 *                    to Win32 popping what the lifted `ret N` popped.
 *   threads          The lifted code keeps its registers in globals. A guest
 *                    lock serialises guest execution; each thread snapshots
 *                    the register file around every native call, and gets its
 *                    own guest stack and TIB the first time it enters.
 *
 * docs/architecture.md has the reasoning at more length.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "recomp_types.h"

#define GUEST_BASE      0x00400000u
#define MAIN_STACK      (4u << 20)
#define THREAD_STACK    (1u << 20)
#define ARG_WORDS       32          /* words copied across a native call */

/* ---- the register file recomp_types.h declares ---- */
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double   g_st[8];
int      g_fp_top;
uint16_t g_fpu_cw = 0x027F;
uint64_t g_mm[8];
uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
ptrdiff_t g_mem_base = 0;
uint32_t g_fs_base, g_gs_base;
uint32_t g_cur_func;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;

extern const uint32_t recomp_entry_va;

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, esi, edi, ebp;
    uint32_t fk, fa, fb, fcf, fs, cur;
    double   st[8];
    int      top;
    uint16_t cw;
    uint64_t mm[8];
} regs_t;

static void regs_save(regs_t *r) {
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esp = g_esp; r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp;
    r->fk = g_flag_k; r->fa = g_flag_a; r->fb = g_flag_b; r->fcf = g_flag_cf;
    r->fs = g_fs_base; r->cur = g_cur_func;
    memcpy(r->st, g_st, sizeof g_st); r->top = g_fp_top; r->cw = g_fpu_cw;
    memcpy(r->mm, g_mm, sizeof g_mm);
}

static void regs_load(const regs_t *r) {
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esp = r->esp; g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp;
    g_flag_k = r->fk; g_flag_a = r->fa; g_flag_b = r->fb; g_flag_cf = r->fcf;
    g_fs_base = r->fs; g_cur_func = r->cur;
    memcpy(g_st, r->st, sizeof g_st); g_fp_top = r->top; g_fpu_cw = r->cw;
    memcpy(g_mm, r->mm, sizeof g_mm);
}

/* ---- guest lock and per-thread guest state ---- */
static CRITICAL_SECTION g_gil;
static _Thread_local uint32_t t_guest_esp;     /* guest esp of the innermost native call, 0 if none */
static _Thread_local uint32_t t_stack_top;     /* this thread's guest stack */
static _Thread_local uint32_t t_tib;           /* this thread's simulated TIB (fs:) */
static _Thread_local uint32_t t_native_target; /* set by recomp_lookup_import */

static uint32_t g_text_lo, g_text_hi, g_image_hi;

static void thread_guest_init(uint32_t stack_size) {
    if (t_stack_top) return;
    uint8_t *stk = VirtualAlloc(NULL, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t *tib = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stk || !tib) { fprintf(stderr, "[host] out of memory for a guest thread\n"); ExitProcess(3); }
    /* Headroom above the top: native_call32 copies ARG_WORDS words whatever
     * the callee takes, and must not read past the allocation. */
    t_stack_top = (uint32_t)(uintptr_t)stk + stack_size - 0x400;
    tib[0x00 / 4] = 0xFFFFFFFFu;                         /* SEH chain end */
    tib[0x04 / 4] = t_stack_top;                         /* stack base (high) */
    tib[0x08 / 4] = (uint32_t)(uintptr_t)stk;            /* stack limit */
    tib[0x18 / 4] = (uint32_t)(uintptr_t)tib;            /* self */
    tib[0x20 / 4] = GetCurrentProcessId();
    tib[0x24 / 4] = GetCurrentThreadId();
    t_tib = (uint32_t)(uintptr_t)tib;
}

/* ---- guest -> native ---- */

/* uint64_t native_call32(fn, args, &popped): call fn with ARG_WORDS words of
 * args on the host stack; report how many bytes it popped (stdcall pops its
 * own, cdecl pops none). Returns edx:eax. */
uint64_t native_call32(uint32_t fn, const uint32_t *args, uint32_t *popped);
__asm__(
    ".att_syntax\n.text\n.globl _native_call32\n_native_call32:\n"
    "  pushl %ebp\n  movl %esp, %ebp\n  pushl %esi\n  pushl %edi\n  pushl %ebx\n"
    "  subl $128, %esp\n"                    /* 4 * ARG_WORDS */
    "  movl 12(%ebp), %esi\n  movl %esp, %edi\n  movl $32, %ecx\n  cld\n  rep movsl\n"
    "  movl %esp, %ebx\n"
    "  call *8(%ebp)\n"
    "  movl %esp, %ecx\n  subl %ebx, %ecx\n"
    "  movl 16(%ebp), %esi\n  movl %ecx, (%esi)\n"
    "  leal -12(%ebp), %esp\n  popl %ebx\n  popl %edi\n  popl %esi\n  popl %ebp\n  ret\n");

/* SC2K_APITRACE=1: every native call, by import name, with its first four
 * argument words and its result. A run of the same call collapses to a count,
 * so an idle loop polling PeekMessageA costs one line per burst. */
static int g_apitrace;
static struct { uint32_t addr; char name[48]; } g_names[512];
static int g_nnames;

static const char *native_name(uint32_t va) {
    for (int i = 0; i < g_nnames; i++) if (g_names[i].addr == va) return g_names[i].name;
    return "?";
}

static void apitrace(uint32_t fn, const uint32_t *a, uint32_t rv) {
    static uint32_t last_fn, repeats;
    if (fn == last_fn) { repeats++; return; }
    if (repeats) fprintf(stderr, "[api]   (x%u more)\n", repeats);
    last_fn = fn;
    repeats = 0;
    fprintf(stderr, "[api] %08X %-24s(%08X %08X %08X %08X) = %08X\n", g_cur_func, native_name(fn),
            a[0], a[1], a[2], a[3], rv);
}

static void native_bridge(void) {
    uint32_t fn = t_native_target, popped = 0;
    regs_t r;
    regs_save(&r);
    uint32_t outer = t_guest_esp;
    t_guest_esp = r.esp;                     /* callbacks push below the args */
    LeaveCriticalSection(&g_gil);
    uint64_t rv = native_call32(fn, (const uint32_t *)(uintptr_t)(r.esp + 4), &popped);
    EnterCriticalSection(&g_gil);
    t_guest_esp = outer;
    regs_load(&r);
    g_eax = (uint32_t)rv;
    g_edx = (uint32_t)(rv >> 32);
    g_esp = r.esp + 4 + popped;              /* dummy return address + args */
    if (g_apitrace) apitrace(fn, (const uint32_t *)(uintptr_t)(r.esp + 4), g_eax);
}

/* Is va real, executable host code? Cached: this sits on every API call. */
static int native_code(uint32_t va) {
    static uint32_t cache[1024];
    uint32_t *slot = &cache[(va >> 2) & 1023];
    if (*slot == va && va) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    *slot = va;
    return 1;
}

recomp_func_t recomp_lookup(uint32_t va) {
    int lo = 0, hi = (int)recomp_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t m = recomp_dispatch_table[mid].address;
        if (m == va) return recomp_dispatch_table[mid].func;
        if (m < va) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

recomp_func_t recomp_lookup_import(uint32_t va) {
    if (va >= GUEST_BASE && va < g_image_hi) return NULL;   /* a guest VA nobody lifted */
    if (!native_code(va)) return NULL;
    t_native_target = va;
    return native_bridge;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "[host] not lifted: 0x%08X from 0x%08X\n", va, g_cur_func);
}

/* ---- native -> guest ---- */

uint32_t guest_callback(uint32_t target, const uint32_t *args, uint32_t *popped) {
    EnterCriticalSection(&g_gil);
    regs_t saved;
    regs_save(&saved);
    uint32_t outer = t_guest_esp;
    if (!t_stack_top)
        fprintf(stderr, "[host] thread %lu enters the game at 0x%08X\n", GetCurrentThreadId(), target);
    thread_guest_init(THREAD_STACK);
    uint32_t esp = (outer ? outer : t_stack_top) - 64;
    for (int i = ARG_WORDS - 1; i >= 0; i--) { esp -= 4; MEM32(esp) = args[i]; }
    uint32_t base = esp;
    esp -= 4; MEM32(esp) = RECOMP_RETADDR;
    g_esp = esp;
    g_fs_base = t_tib;
    uint32_t result = 0;
    recomp_func_t fn = recomp_lookup(target);
    static int cbtrace = -1;
    if (cbtrace < 0) cbtrace = GetEnvironmentVariableA("SC2K_CBTRACE", NULL, 0) != 0;
    if (cbtrace) {
        char c0[48] = "", c1[48] = "";
        if (IsWindow((HWND)(uintptr_t)args[0])) GetClassNameA((HWND)(uintptr_t)args[0], c0, sizeof c0);
        if (IsWindow((HWND)(uintptr_t)args[1])) GetClassNameA((HWND)(uintptr_t)args[1], c1, sizeof c1);
        fprintf(stderr, "[cb] %08X(%08X %08X %08X %08X) %s %s\n", target, args[0], args[1], args[2], args[3], c0, c1);
    }
    if (fn) {
        fn();
        result = g_eax;
        *popped = g_esp - base;
        if (*popped > 4 * ARG_WORDS || (*popped & 3)) {
            fprintf(stderr, "[host] callback 0x%08X popped %u bytes; assuming 0\n", target, *popped);
            *popped = 0;
        }
    } else {
        fprintf(stderr, "[host] callback into unlifted 0x%08X\n", target);
        *popped = 0;
    }
    t_guest_esp = outer;
    regs_load(&saved);
    LeaveCriticalSection(&g_gil);
    return result;
}

/* Entered with the faulting guest VA in eax and the native caller's frame
 * untouched: [esp] = return address, [esp+4..] = arguments. */
void callback_entry(void);
__asm__(
    ".att_syntax\n.text\n.globl _callback_entry\n_callback_entry:\n"
    "  pushl %ebp\n  movl %esp, %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
    "  subl $4, %esp\n"                      /* popped, at -16(%ebp) */
    "  leal -16(%ebp), %edx\n  pushl %edx\n"
    "  leal 8(%ebp), %ecx\n  pushl %ecx\n"
    "  pushl %eax\n"
    "  call _guest_callback\n"
    "  addl $12, %esp\n"
    "  movl -16(%ebp), %ecx\n"
    "  leal -12(%ebp), %esp\n  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n"
    "  popl %edx\n  addl %ecx, %esp\n  jmp *%edx\n");

static void crash_report(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "\n[host] exception 0x%08lX at %p", er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " (%s 0x%08lX)", er->ExceptionInformation[0] == 1 ? "write" :
                er->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long)er->ExceptionInformation[1]);
    fprintf(stderr, "\n  in lifted 0x%08X  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
            "  esp=%08X ebp=%08X esi=%08X edi=%08X\n  recent indirect calls (target <- caller):\n",
            g_cur_func, g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    for (int i = 16; i > 0; i--) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        if (g_icall_trace[k]) fprintf(stderr, "    %08X <- %08X\n", g_icall_trace[k], g_icall_from[k]);
    }
    fflush(stderr);
}

static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 8) {
        uint32_t a = (uint32_t)er->ExceptionInformation[1];
        if (a >= g_text_lo && a < g_text_hi && ep->ContextRecord->Eip == a) {
            ep->ContextRecord->Eax = a;
            ep->ContextRecord->Eip = (DWORD)(uintptr_t)callback_entry;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        er->ExceptionCode == EXCEPTION_STACK_OVERFLOW ||
        er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)
        crash_report(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- loading ---- */

void *override_for(const char *dll, const char *name);   /* overrides.c */

static int bind_imports(uint8_t *base) {
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int missing = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        HMODULE h = LoadLibraryA(dll);
        uint32_t *iat = (uint32_t *)(base + d->FirstThunk);
        uint32_t *ilt = (uint32_t *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        DWORD old;
        VirtualProtect(iat, 0x1000, PAGE_READWRITE, &old);
        for (int i = 0; ilt[i]; i++) {
            char ord[16];
            const char *name;
            if (ilt[i] & 0x80000000u) {
                snprintf(ord, sizeof ord, "#%u", ilt[i] & 0xFFFF);
                name = ord;
            } else {
                name = (const char *)(base + ilt[i] + 2);
            }
            void *p = override_for(dll, name);
            if (!p && h) p = (void *)GetProcAddress(h, name[0] == '#' ? (LPCSTR)(uintptr_t)(ilt[i] & 0xFFFF) : name);
            if (!p) { fprintf(stderr, "[host] unresolved import %s!%s\n", dll, name); missing++; }
            iat[i] = (uint32_t)(uintptr_t)p;
            if (g_nnames < 512) {
                g_names[g_nnames].addr = iat[i];
                snprintf(g_names[g_nnames++].name, sizeof g_names[0].name, "%s", name);
            }
        }
    }
    return missing;
}

char g_game_exe[MAX_PATH];     /* full path of SIMCITY.EXE */
char g_guest_cmdline[4096];    /* what the game sees from GetCommandLineA */

int  headless_init(void);                                   /* capture.c */
int  record_start(const char *path, int fps, double seconds);
void exit_after(double seconds);
void input_start(const char *script);

/* SC2K_WATCH=1: once a second, where the guest is. Reads the globals without
 * the lock on purpose -- a stuck guest holds it. */
static DWORD WINAPI watch_thread(LPVOID unused) {
    (void)unused;
    for (unsigned s = 1;; s++) {
        Sleep(1000);
        int32_t days = *(volatile int32_t *)0x004CAE04;          /* see fx.c */
        fprintf(stderr, "[watch] %us  in 0x%08X  icalls %u  last %08X <- %08X  day %d month %d weather %u funds %d\n",
                s, g_cur_func, g_icall_count, g_icall_trace[(g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1)],
                g_icall_from[(g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1)], days, (days / 25) % 12,
                *(volatile uint8_t *)0x004CB40C, *(volatile int32_t *)0x004CA444);
    }
    return 0;
}

void headless_attach(void);                                 /* capture.c */
void registry_defaults(void);                               /* overrides.c */

static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    headless_attach();
    EnterCriticalSection(&g_gil);
    thread_guest_init(MAIN_STACK);
    g_esp = t_stack_top;
    g_fs_base = t_tib;
    PUSH32(g_esp, RECOMP_RETADDR);
    recomp_func_t entry = recomp_lookup(recomp_entry_va);
    if (!entry) { fprintf(stderr, "[host] entry 0x%08X not lifted\n", recomp_entry_va); return 2; }
    entry();
    fprintf(stderr, "[host] guest entry returned, eax=%u\n", g_eax);
    return g_eax;
}

/* Getting 0x00400000 for the game image (the approach is gta's premap.c).
 * Nothing in-process can claim it: by the time main runs, process init has
 * mapped NLS tables and heap segments there. So the host starts itself again
 * suspended -- when only ntdll, the host image and the main stack exist --
 * reserves the range in that child, and resumes it. The child's loader places
 * everything else around the reservation. Returns the child's exit code. */
#define GUEST_SPAN 0x00200000u   /* SIMCITY.EXE's SizeOfImage is 0x163000 */
static int relaunch_reserved(void) {
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    /* The game shows its frame with ShowWindow(m_nCmdShow | 3): "maximized",
     * given the SW_SHOWNORMAL Explorer passed in 1996. Started from a console,
     * the CRT reports SW_SHOWDEFAULT (10), and 10|3 is SW_FORCEMINIMIZE today,
     * so the frame never appeared. Launch the child the way Explorer would. */
    GetStartupInfoA(&si);
    if (!(si.dwFlags & STARTF_USESHOWWINDOW)) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
    }
    SetEnvironmentVariableA("SC2K_CHILD", "1");
    if (!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[host] cannot relaunch (error %lu)\n", GetLastError());
        return 2;
    }
    if (!VirtualAllocEx(pi.hProcess, (void *)(uintptr_t)GUEST_BASE, GUEST_SPAN,
                        MEM_RESERVE, PAGE_NOACCESS)) {
        fprintf(stderr, "[host] cannot reserve 0x%08X in the child (error %lu)\n",
                GUEST_BASE, GetLastError());
        TerminateProcess(pi.hProcess, 2);
        return 2;
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 2;
    GetExitCodeProcess(pi.hProcess, &rc);
    return (int)rc;
}

/* --native: run the ORIGINAL SIMCITY.EXE instead of the recompiled one, on the
 * same (headless) desktop with the same recorder and input script. It is the
 * oracle: when the recompiled game behaves oddly, the same scenario run
 * natively says what it should have done. A job object ends the game with us. */
void capture_target(DWORD pid);                            /* capture.c */
void input_set_game_thread(DWORD tid);                     /* input.c */

void input_start(const char *script);
static int run_native(int headless, const char *input) {
    char dir[MAX_PATH], cmd[sizeof g_guest_cmdline];
    strcpy(dir, g_game_exe);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    strcpy(cmd, g_guest_cmdline);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    if (headless) si.lpDesktop = "sc2k-headless";
    PROCESS_INFORMATION pi;
    HANDLE job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    if (!CreateProcessA(g_game_exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        fprintf(stderr, "[host] cannot start %s natively (error %lu)\n", g_game_exe, GetLastError());
        return 2;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    capture_target(pi.dwProcessId);
    input_set_game_thread(pi.dwThreadId);
    ResumeThread(pi.hThread);
    input_start(input);
    fprintf(stderr, "[host] running the original %s natively, pid %lu\n", g_game_exe, pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    return (int)rc;
}

static void usage(void) {
    fprintf(stderr, "usage: sc2k [--headless] [--record out.mp4] [--fps N] [--seconds N] [--input SCRIPT] [--native]\n"
                    "            [path/to/SIMCITY.EXE] [game arguments...]\n");
}

int fx_selftest(void);                                     /* fx.c */
void fx_init(void);

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return fx_selftest();
    if (!GetEnvironmentVariableA("SC2K_CHILD", NULL, 0)) return relaunch_reserved();
    const char *exe = "game/SIMCITY.EXE", *record = NULL, *input = NULL;
    int headless = 0, native = 0, fps = 10, i = 1;
    double seconds = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--native")) native = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else { usage(); return 2; }
    }
    if (i < argc) exe = argv[i++];
    if (!GetFullPathNameA(exe, sizeof g_game_exe, g_game_exe, NULL)) return 2;
    int n = snprintf(g_guest_cmdline, sizeof g_guest_cmdline, "\"%s\"", g_game_exe);
    for (; i < argc && n < (int)sizeof g_guest_cmdline; i++)
        n += snprintf(g_guest_cmdline + n, sizeof g_guest_cmdline - n, " %s", argv[i]);

    if (native) {
        registry_defaults();
        if (headless && !headless_init()) return 2;
        if (record && !record_start(record, fps, seconds)) return 2;
        exit_after(seconds);
        int rc = run_native(headless, input);
        return rc;
    }

    /* Guest .text is non-executable; the callback trap needs DEP on. */
    SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
    AddVectoredExceptionHandler(1, veh);

    /* SMACKW32.DLL and the data files live beside the game. */
    char dir[MAX_PATH];
    strcpy(dir, g_game_exe);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    SetDllDirectoryA(dir);
    SetCurrentDirectoryA(dir);

    /* Hand the parent's reservation (see relaunch_reserved) to the loader, and
     * map the game into it before anything else can allocate there. */
    VirtualFree((void *)(uintptr_t)GUEST_BASE, 0, MEM_RELEASE);
    HMODULE img = LoadLibraryExA(g_game_exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if ((uint32_t)(uintptr_t)img != GUEST_BASE) {
        MEMORY_BASIC_INFORMATION m;
        VirtualQuery((void *)(uintptr_t)GUEST_BASE, &m, sizeof m);
        fprintf(stderr, "[host] could not map %s at 0x%08X (got %p, error %lu); "
                "that range holds a region at %p+0x%lX, state 0x%lX type 0x%lX\n",
                g_game_exe, GUEST_BASE, (void *)img, GetLastError(),
                m.AllocationBase, (unsigned long)m.RegionSize, m.State, m.Type);
        return 2;
    }
    uint8_t *base = (uint8_t *)img;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    g_image_hi = GUEST_BASE + nt->OptionalHeader.SizeOfImage;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) {
            g_text_lo = GUEST_BASE + sec[i].VirtualAddress;
            g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
        }
    }
    int missing = bind_imports(base);
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)g_text_lo, g_text_hi - g_text_lo, PAGE_READONLY, &old)) {
        fprintf(stderr, "[host] cannot protect guest .text\n");
        return 2;
    }
    fprintf(stderr, "[host] %s mapped at 0x%08X, %u lifted functions, %d unresolved imports\n",
            g_game_exe, GUEST_BASE, recomp_dispatch_count, missing);

    /* Only now: anything that allocates must wait until the image owns its range. */
    registry_defaults();
    fx_init();
    if (headless && !headless_init()) return 2;
    if (record && !record_start(record, fps, seconds)) return 2;
    exit_after(seconds);
    input_start(input);
    g_apitrace = GetEnvironmentVariableA("SC2K_APITRACE", NULL, 0) != 0;
    if (GetEnvironmentVariableA("SC2K_WATCH", NULL, 0)) CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);

    /* The game runs on its own thread. Lifted code nests deeply on the host
     * stack, and the host's main stack cannot simply be made big: the linker
     * reserves it before main runs, low in the address space -- a 16 MB one
     * sat exactly on 0x00400000 and the game image had nowhere to go. */
    InitializeCriticalSection(&g_gil);
    HANDLE t = CreateThread(NULL, 64u << 20, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { fprintf(stderr, "[host] cannot start the guest thread\n"); return 2; }
    WaitForSingleObject(t, INFINITE);
    DWORD rc = 0;
    GetExitCodeThread(t, &rc);
    return (int)rc;
}
