// Guest threads. Translated code runs on the host stack, so each guest context
// gets a host stack of its own. Saving and loading a context switches stacks.
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <ucontext.h>
#endif

#include "cpu.h"

// OSContext layout in guest memory.
enum {
  CONTEXT_CR = 0x80, CONTEXT_LR = 0x84, CONTEXT_CTR = 0x88, CONTEXT_XER = 0x8c, CONTEXT_FPR = 0x90,
  CONTEXT_SRR0 = 0x198, CONTEXT_SRR1 = 0x19c, CONTEXT_STATE = 0x1a2, CONTEXT_GQR = 0x1a4, CONTEXT_PS = 0x1c8,
};
// Tells the scheduler not to save over a context an exception is using.
#define STATE_IN_EXCEPTION 2u

#define MAX_CONTEXTS 256
#define HOST_STACK_BYTES (32u * 1024u * 1024u)
#define GQR_BASE 912
#define INTERRUPTS_ENABLED 0x8000u

// save_context returns before anything resumes it, so a ucontext taken inside it would
// come back to a frame already overwritten: its saved registers and return address are
// on the host stack, and later calls reuse that space. So a stub records the caller's
// own registers instead, and load_context returns straight to the caller with them. On
// the Intel Mac the old way only worked while no later call happened to overwrite those
// slots; the flat guest memory changed the code's frames, and rbx came back as a guest
// value.
#if defined(_WIN32) && defined(__x86_64__)
// Windows has no ucontext, and its calling convention keeps more registers. One record
// serves both ways of saving: what a function must keep for its caller, where it returns
// to, and the stack's bounds, which Windows keeps per thread (the TEB) and uses to grow a
// stack and to deliver a fault.
#define RECORDS_CALLER 1
typedef struct {
  uint64_t rbx, rbp, rdi, rsi, r12, r13, r14, r15;
  uint64_t return_address, stack;
  uint64_t stack_base, stack_limit, stack_reserved;
  uint64_t padding;
  uint8_t xmm6_to_xmm15[160];
} CallerRegisters;
typedef CallerRegisters ucontext_t;
__attribute__((returns_twice)) int context_capture(CallerRegisters *context);  // 0, then 1 once resumed.
__attribute__((noreturn)) void context_resume(const CallerRegisters *context);
#define getcontext(context) context_capture(context)
#define setcontext(context) context_resume(context)
#elif defined(__aarch64__)
#define RECORDS_CALLER 1
typedef struct {
  uint64_t x19_to_x30[12], stack, d8_to_d15[8], padding;
} CallerRegisters;
#elif defined(__x86_64__)
#define RECORDS_CALLER 1
typedef struct {
  uint64_t rbx, rbp, r12, r13, r14, r15, return_address, stack;
} CallerRegisters;
#endif

typedef struct {
  uint32_t guest;
  uint32_t resume_pc, resume_stack;
  volatile int resumed;
  ucontext_t host;
#ifdef RECORDS_CALLER
  CallerRegisters caller;
  int use_caller;
#endif
} Slot;

static Slot slots[MAX_CONTEXTS];

// The last few switches, printed when something goes wrong.
#define HISTORY 24
typedef struct { const char *what; uint32_t context, pc, stack, link; } Event;
static Event history[HISTORY];
static unsigned history_count;

static void record(const char *what, uint32_t context, uint32_t pc) {
  history[history_count++ % HISTORY] = (Event){what, context, pc, cpu.r[1], cpu.lr};
}

static void print_history(void) {
  unsigned first = history_count > HISTORY ? history_count - HISTORY : 0;
  for (unsigned i = first; i < history_count; i++) {
    Event *e = &history[i % HISTORY];
    fprintf(stderr, "  %-9s context %08x pc %08x stack %08x link %08x\n", e->what, e->context, e->pc, e->stack, e->link);
  }
}

static Slot *slot_for(uint32_t guest) {
  for (int i = 0; i < MAX_CONTEXTS; i++) {
    if (slots[i].guest == guest) return &slots[i];
    if (!slots[i].guest) {
      slots[i].guest = guest;
      return &slots[i];
    }
  }
  fprintf(stderr, "stop: too many thread contexts\n");
  exit(5);
}

#ifdef RECORDS_CALLER
#define NAME_TEXT(text) #text
#define NAME_WITH(prefix, name) NAME_TEXT(prefix) #name
#define SYMBOL(prefix, name) NAME_WITH(prefix, name)

// Puts the registers back and returns from the save_context that recorded them.
#if defined(_WIN32) && defined(__x86_64__)
#define XMM_LOAD(base, reg) \
  "movups " #base "+0(" reg "), %xmm6\n" "movups " #base "+16(" reg "), %xmm7\n" "movups " #base "+32(" reg "), %xmm8\n" \
  "movups " #base "+48(" reg "), %xmm9\n" "movups " #base "+64(" reg "), %xmm10\n" "movups " #base "+80(" reg "), %xmm11\n" \
  "movups " #base "+96(" reg "), %xmm12\n" "movups " #base "+112(" reg "), %xmm13\n" "movups " #base "+128(" reg "), %xmm14\n" \
  "movups " #base "+144(" reg "), %xmm15\n"
#define XMM_SAVE(base, reg) \
  "movups %xmm6, " #base "+0(" reg ")\n" "movups %xmm7, " #base "+16(" reg ")\n" "movups %xmm8, " #base "+32(" reg ")\n" \
  "movups %xmm9, " #base "+48(" reg ")\n" "movups %xmm10, " #base "+64(" reg ")\n" "movups %xmm11, " #base "+80(" reg ")\n" \
  "movups %xmm12, " #base "+96(" reg ")\n" "movups %xmm13, " #base "+112(" reg ")\n" "movups %xmm14, " #base "+128(" reg ")\n" \
  "movups %xmm15, " #base "+144(" reg ")\n"
__attribute__((naked, returns_twice)) int context_capture(CallerRegisters *context) {
  __asm__(
      "movq %rbx, 0(%rcx)\n"
      "movq %rbp, 8(%rcx)\n"
      "movq %rdi, 16(%rcx)\n"
      "movq %rsi, 24(%rcx)\n"
      "movq %r12, 32(%rcx)\n"
      "movq %r13, 40(%rcx)\n"
      "movq %r14, 48(%rcx)\n"
      "movq %r15, 56(%rcx)\n"
      "movq (%rsp), %rax\n"
      "movq %rax, 64(%rcx)\n"
      "leaq 8(%rsp), %rax\n"
      "movq %rax, 72(%rcx)\n"
      "movq %gs:0x8, %rax\n"
      "movq %rax, 80(%rcx)\n"
      "movq %gs:0x10, %rax\n"
      "movq %rax, 88(%rcx)\n"
      "movq %gs:0x1478, %rax\n"
      "movq %rax, 96(%rcx)\n"
      XMM_SAVE(112, "%rcx")
      "xorl %eax, %eax\n"
      "ret\n");
}
__attribute__((naked, noreturn)) void context_resume(const CallerRegisters *context) {
  __asm__(
      XMM_LOAD(112, "%rcx")
      "movq 80(%rcx), %rax\n"
      "movq %rax, %gs:0x8\n"
      "movq 88(%rcx), %rax\n"
      "movq %rax, %gs:0x10\n"
      "movq 96(%rcx), %rax\n"
      "movq %rax, %gs:0x1478\n"
      "movq 0(%rcx), %rbx\n"
      "movq 8(%rcx), %rbp\n"
      "movq 16(%rcx), %rdi\n"
      "movq 24(%rcx), %rsi\n"
      "movq 32(%rcx), %r12\n"
      "movq 40(%rcx), %r13\n"
      "movq 48(%rcx), %r14\n"
      "movq 56(%rcx), %r15\n"
      "movq 64(%rcx), %rdx\n"
      "movq 72(%rcx), %rsp\n"
      "movl $1, %eax\n"
      "jmpq *%rdx\n");
}
#define return_to_caller(caller) context_resume(caller)
#elif defined(__x86_64__)
__attribute__((naked, noreturn)) static void return_to_caller(const CallerRegisters *caller) {
  __asm__(
      "movq 0(%rdi), %rbx\n"
      "movq 8(%rdi), %rbp\n"
      "movq 16(%rdi), %r12\n"
      "movq 24(%rdi), %r13\n"
      "movq 32(%rdi), %r14\n"
      "movq 40(%rdi), %r15\n"
      "movq 48(%rdi), %rax\n"
      "movq 56(%rdi), %rsp\n"
      "jmpq *%rax\n");
}
#else
__attribute__((naked, noreturn)) static void return_to_caller(const CallerRegisters *caller) {
  __asm__(
      "ldp x19, x20, [x0, #0]\n"
      "ldp x21, x22, [x0, #16]\n"
      "ldp x23, x24, [x0, #32]\n"
      "ldp x25, x26, [x0, #48]\n"
      "ldp x27, x28, [x0, #64]\n"
      "ldp x29, x30, [x0, #80]\n"
      "ldr x9, [x0, #96]\n"
      "mov sp, x9\n"
      "ldp d8, d9, [x0, #104]\n"
      "ldp d10, d11, [x0, #120]\n"
      "ldp d12, d13, [x0, #136]\n"
      "ldp d14, d15, [x0, #152]\n"
      "ret\n");
}
#endif
#endif

static uint32_t condition_bits(void) {
  uint32_t value = 0;
  for (int i = 0; i < 8; i++) value = (value << 4) | cpu.cr[i];
  return value;
}

static void write_registers(uint32_t c, uint32_t pc) {
  for (int i = 0; i < 32; i++) {
    wr32(c + i * 4, cpu.r[i]);
    wr64(c + CONTEXT_FPR + i * 8, double_bits(cpu.f[i]));
    wr64(c + CONTEXT_PS + i * 8, double_bits(cpu.p[i]));
  }
  wr32(c + CONTEXT_CR, condition_bits());
  wr32(c + CONTEXT_LR, cpu.lr);
  wr32(c + CONTEXT_CTR, cpu.ctr);
  wr32(c + CONTEXT_XER, xer_get());
  wr32(c + CONTEXT_SRR0, pc);
  wr32(c + CONTEXT_SRR1, cpu.msr);
  for (int i = 0; i < 8; i++) wr32(c + CONTEXT_GQR + i * 4, cpu.spr[GQR_BASE + i]);
}

static void read_registers(uint32_t c) {
  for (int i = 0; i < 32; i++) {
    cpu.r[i] = rd32(c + i * 4);
    cpu.f[i] = as_double(rd64(c + CONTEXT_FPR + i * 8));
    cpu.p[i] = as_double(rd64(c + CONTEXT_PS + i * 8));
  }
  uint32_t cr = rd32(c + CONTEXT_CR);
  for (int i = 0; i < 8; i++) cpu.cr[i] = (cr >> (28 - i * 4)) & 15;
  cpu.lr = rd32(c + CONTEXT_LR);
  cpu.ctr = rd32(c + CONTEXT_CTR);
  xer_set(rd32(c + CONTEXT_XER));
  cpu.msr = rd32(c + CONTEXT_SRR1);
  for (int i = 0; i < 8; i++) cpu.spr[GQR_BASE + i] = rd32(c + CONTEXT_GQR + i * 4);
  if (cpu.msr & 0x8000u) {  // As rfi does: one waiting is taken next.
    void interrupts_turned_on_soon(void);
    interrupts_turned_on_soon();
  }
}

// A context that has never run starts its code on a new stack. If that code
// returns, it continues at the link register, which holds the thread's exit.
static uint32_t start_pc;

__attribute__((used)) void thread_start(void) {
  if (getenv("TRACE_THREADS")) fprintf(stderr, "thread starts at %08x, stack %08x, link %08x\n", start_pc, cpu.r[1], cpu.lr);
  dispatch(start_pc);
  if (getenv("TRACE_THREADS")) fprintf(stderr, "thread from %08x returned, link %08x\n", start_pc, cpu.lr);
  while (1) dispatch(cpu.lr);
}

#ifdef _WIN32
#include <windows.h>
#define FIRST_COMMIT (256u * 1024u)
// Moves to a new stack and runs thread_start there. Windows grows a stack through the
// guard page below what's in use, by the bounds in the TEB, so those are set first.
__attribute__((naked, noreturn)) static void start_on_stack(void *top, void *limit, void *reserved) {
  __asm__(
      "movq %rcx, %gs:0x8\n"
      "movq %rdx, %gs:0x10\n"
      "movq %r8, %gs:0x1478\n"
      "leaq -72(%rcx), %rsp\n"
      "movq $0, (%rsp)\n"
      "jmp thread_start\n");
}

static void start_fresh(Slot *slot, uint32_t pc) {
  uint8_t *stack = VirtualAlloc(0, HOST_STACK_BYTES, MEM_RESERVE, PAGE_READWRITE);
  uint8_t *top = stack + HOST_STACK_BYTES;
  if (!stack || !VirtualAlloc(top - FIRST_COMMIT, FIRST_COMMIT, MEM_COMMIT, PAGE_READWRITE)
      || !VirtualAlloc(top - FIRST_COMMIT - 4096, 4096, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
    fprintf(stderr, "stop: no memory for a thread's stack (error %lu)\n", GetLastError());
    exit(5);
  }
  start_pc = pc;
  slot->resume_pc = 0;
  start_on_stack(top, top - FIRST_COMMIT, stack);
}
#else
static void start_fresh(Slot *slot, uint32_t pc) {
  static ucontext_t fresh;
  getcontext(&fresh);
  fresh.uc_stack.ss_sp = malloc(HOST_STACK_BYTES);
  fresh.uc_stack.ss_size = HOST_STACK_BYTES;
  fresh.uc_link = 0;
  start_pc = pc;
  makecontext(&fresh, thread_start, 0);
  slot->resume_pc = 0;
  setcontext(&fresh);
}
#endif

void load_context(void) {
  uint32_t c = cpu.r[3];
  uint32_t pc = rd32(c + CONTEXT_SRR0);
  if (!pc) {
    fprintf(stderr, "stop: context %08x has no start address (called from %08x)\n", c, cpu.lr);
    print_history();
    exit(5);
  }
  record("load", c, pc);
  read_registers(c);
  wr16(c + CONTEXT_STATE, rd16(c + CONTEXT_STATE) & ~STATE_IN_EXCEPTION);
  Slot *slot = slot_for(c);
  if (slot->resume_pc == pc && slot->resume_stack == cpu.r[1]) {
    slot->resumed = 1;
#ifdef RECORDS_CALLER
    if (slot->use_caller) return_to_caller(&slot->caller);
#endif
    setcontext(&slot->host);
  }
  start_fresh(slot, pc);
}

#ifdef RECORDS_CALLER
void save_context_recorded(const CallerRegisters *caller);

// A stub that records its caller's registers (CallerRegisters) and hands them to
// `recorded`. Returns 0 the first time, and 1 when something loads this context again.
#if defined(_WIN32) && defined(__x86_64__)
// On entry the return address is at the stack pointer. 312 bytes hold the 32 the callee
// may use, the record, and 8 to keep the stack 16-aligned for the call.
#define RECORDING_STUB(name, recorded) \
__attribute__((naked)) void name(void) { \
  __asm__( \
      "subq $312, %rsp\n" \
      "movq %rbx, 32(%rsp)\n" \
      "movq %rbp, 40(%rsp)\n" \
      "movq %rdi, 48(%rsp)\n" \
      "movq %rsi, 56(%rsp)\n" \
      "movq %r12, 64(%rsp)\n" \
      "movq %r13, 72(%rsp)\n" \
      "movq %r14, 80(%rsp)\n" \
      "movq %r15, 88(%rsp)\n" \
      "movq 312(%rsp), %rax\n" \
      "movq %rax, 96(%rsp)\n" \
      "leaq 320(%rsp), %rax\n" \
      "movq %rax, 104(%rsp)\n" \
      "movq %gs:0x8, %rax\n" \
      "movq %rax, 112(%rsp)\n" \
      "movq %gs:0x10, %rax\n" \
      "movq %rax, 120(%rsp)\n" \
      "movq %gs:0x1478, %rax\n" \
      "movq %rax, 128(%rsp)\n" \
      XMM_SAVE(144, "%rsp") \
      "leaq 32(%rsp), %rcx\n" \
      "call " #recorded "\n" \
      "addq $312, %rsp\n" \
      "ret\n"); \
}
#elif defined(__x86_64__)
// On entry the return address is at the stack pointer, and the caller's stack is 8 above.
// 72 bytes keep the stack 16-aligned for the call.
#define RECORDING_STUB(name, recorded) \
__attribute__((naked)) void name(void) { \
  __asm__( \
      "subq $72, %rsp\n" \
      "movq %rbx, 0(%rsp)\n" \
      "movq %rbp, 8(%rsp)\n" \
      "movq %r12, 16(%rsp)\n" \
      "movq %r13, 24(%rsp)\n" \
      "movq %r14, 32(%rsp)\n" \
      "movq %r15, 40(%rsp)\n" \
      "movq 72(%rsp), %rax\n" \
      "movq %rax, 48(%rsp)\n" \
      "leaq 80(%rsp), %rax\n" \
      "movq %rax, 56(%rsp)\n" \
      "movq %rsp, %rdi\n" \
      "call " SYMBOL(__USER_LABEL_PREFIX__, recorded) "\n" \
      "addq $72, %rsp\n" \
      "ret\n"); \
}
#else
#define RECORDING_STUB(name, recorded) \
__attribute__((naked)) void name(void) { \
  __asm__( \
      "sub sp, sp, #176\n" \
      "stp x19, x20, [sp, #0]\n" \
      "stp x21, x22, [sp, #16]\n" \
      "stp x23, x24, [sp, #32]\n" \
      "stp x25, x26, [sp, #48]\n" \
      "stp x27, x28, [sp, #64]\n" \
      "stp x29, x30, [sp, #80]\n" \
      "add x9, sp, #176\n" \
      "str x9, [sp, #96]\n" \
      "stp d8, d9, [sp, #104]\n" \
      "stp d10, d11, [sp, #120]\n" \
      "stp d12, d13, [sp, #136]\n" \
      "stp d14, d15, [sp, #152]\n" \
      "mov x0, sp\n" \
      "bl " SYMBOL(__USER_LABEL_PREFIX__, recorded) "\n" \
      "ldp x29, x30, [sp, #80]\n" \
      "add sp, sp, #176\n" \
      "ret\n"); \
}
#endif
RECORDING_STUB(save_context, save_context_recorded)

void save_context_recorded(const CallerRegisters *caller) {
  uint32_t c = cpu.r[3], return_pc = cpu.lr;
  uint32_t saved_three = cpu.r[3];
  cpu.r[3] = 1;
  write_registers(c, return_pc);
  cpu.r[3] = saved_three;
  record("save", c, return_pc);
  Slot *slot = slot_for(c);
  slot->resume_pc = return_pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
  slot->caller = *caller;
  slot->use_caller = 1;
  cpu.r[3] = 0;
}
#else
// Returns 0 the first time, and 1 when something loads this context again.
void save_context(void) {
  uint32_t c = cpu.r[3], return_pc = cpu.lr;
  uint32_t saved_three = cpu.r[3];
  cpu.r[3] = 1;
  write_registers(c, return_pc);
  cpu.r[3] = saved_three;
  record("save", c, return_pc);
  Slot *slot = slot_for(c);
  slot->resume_pc = return_pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
  getcontext(&slot->host);
  if (slot->resumed) return;
  cpu.r[3] = 0;
}
#endif

// The game's own setjmp and longjmp: the C library's, saving the link register, the
// condition register, r1, r2, r13 to r31, f14 to f31 with their second paired-single
// value, and the FPSCR (offsets below, as the game's code lays them out). Games use them
// as coroutines, as Mario Party's process system does: a process's buffer is filled in by
// hand with a new stack and its function, and longjmp starts it. Translated code returns
// rather than jumps, so a longjmp into another stack must become a host context switch,
// exactly as OSLoadContext's is: setjmp records its caller's host registers too, and
// longjmp either returns to them, or starts the function on a host stack of its own.
// Two layouts are known: the C library's with paired singles (each of f14 to f31 in 16
// bytes, the FPSCR at 384), and the plain one Hudson's engine keeps in Mario Party 8 (8 bytes
// each, the FPSCR at 240). Both keep the rest where the other does.
enum { JUMP_LR = 0, JUMP_CR = 4, JUMP_SP = 8, JUMP_R2 = 12, JUMP_R13 = 20, JUMP_FPR = 96, MAX_JUMPS = 1024 };
typedef struct { int fpr_stride, fpscr_at, paired; } JumpLayout;
static const JumpLayout JUMP_PAIRED = {16, 384, 1}, JUMP_PLAIN = {8, 240, 0};
static Slot jump_slots[MAX_JUMPS];

static Slot *jump_slot_for(uint32_t buffer) {
  for (int i = 0; i < MAX_JUMPS; i++) {
    if (jump_slots[i].guest == buffer) return &jump_slots[i];
    if (!jump_slots[i].guest) {
      jump_slots[i].guest = buffer;
      return &jump_slots[i];
    }
  }
  fprintf(stderr, "stop: too many setjmp buffers\n");
  exit(5);
}

static void write_jump_buffer(uint32_t b, const JumpLayout *layout) {
  wr32(b + JUMP_LR, cpu.lr);
  wr32(b + JUMP_CR, condition_bits());
  wr32(b + JUMP_SP, cpu.r[1]);
  wr32(b + JUMP_R2, cpu.r[2]);
  for (int i = 13; i < 32; i++) wr32(b + JUMP_R13 + (i - 13) * 4, cpu.r[i]);
  for (int i = 14; i < 32; i++) {
    uint32_t at = b + JUMP_FPR + (uint32_t)((i - 14) * layout->fpr_stride);
    wr64(at, double_bits(cpu.f[i]));
    if (layout->paired) ps_store(at + 8, 0, 0, i);  // psq_st with GQR 0: both values as singles.
  }
  wr64(b + (uint32_t)layout->fpscr_at, double_bits(fpscr_as_double()));
}

static void read_jump_buffer(uint32_t b, const JumpLayout *layout) {
  cpu.lr = rd32(b + JUMP_LR);
  uint32_t cr = rd32(b + JUMP_CR);
  for (int i = 0; i < 8; i++) cpu.cr[i] = (cr >> (28 - i * 4)) & 15;
  cpu.r[1] = rd32(b + JUMP_SP);
  cpu.r[2] = rd32(b + JUMP_R2);
  for (int i = 13; i < 32; i++) cpu.r[i] = rd32(b + JUMP_R13 + (i - 13) * 4);
  for (int i = 14; i < 32; i++) {
    uint32_t at = b + JUMP_FPR + (uint32_t)((i - 14) * layout->fpr_stride);
    cpu.f[i] = as_double(rd64(at));
    if (layout->paired) ps_load(at + 8, 0, 0, i);
    else cpu.p[i] = cpu.f[i];  // lfd sets both halves of a paired single.
  }
  fpscr_write_fields(255, (uint32_t)rd64(b + (uint32_t)layout->fpscr_at));
}

#ifdef RECORDS_CALLER
static void setjmp_recorded(const CallerRegisters *caller, const JumpLayout *layout) {
  uint32_t b = cpu.r[3];
  write_jump_buffer(b, layout);
  record("setjmp", b, cpu.lr);
  Slot *slot = jump_slot_for(b);
  slot->resume_pc = cpu.lr;
  slot->resume_stack = cpu.r[1];
  slot->caller = *caller;
  slot->use_caller = 1;
  cpu.r[3] = 0;
}

static void longjmp_with(const JumpLayout *layout) {
  uint32_t b = cpu.r[3], value = cpu.r[4];
  read_jump_buffer(b, layout);
  cpu.r[3] = value ? value : 1;
  record("longjmp", b, cpu.lr);
  Slot *slot = jump_slot_for(b);
  if (slot->use_caller && slot->resume_pc == cpu.lr && slot->resume_stack == cpu.r[1]) return_to_caller(&slot->caller);
  // Filled in by hand: the function in the link register starts on a stack of its own.
  slot->use_caller = 0;
  start_fresh(slot, cpu.lr);
}

void guest_setjmp_recorded(const CallerRegisters *caller) { setjmp_recorded(caller, &JUMP_PAIRED); }
void guest_setjmp_plain_recorded(const CallerRegisters *caller) { setjmp_recorded(caller, &JUMP_PLAIN); }
RECORDING_STUB(guest_setjmp, guest_setjmp_recorded)
RECORDING_STUB(guest_setjmp_plain, guest_setjmp_plain_recorded)
void guest_longjmp(void) { longjmp_with(&JUMP_PAIRED); }
void guest_longjmp_plain(void) { longjmp_with(&JUMP_PLAIN); }
#endif

// An exception saves where the code was, runs the game's handler with
// interrupts off, and comes back here when that handler loads the context.
void enter_exception(uint32_t kind, uint32_t handler) {
  uint32_t c = rd32(0x800000d4);
  static uint32_t marker = 0xe0000000u;
  uint32_t pc = marker += 4;
  record(kind == 8 ? "timer" : "external", c, pc);
  write_registers(c, pc);
  wr16(c + CONTEXT_STATE, rd16(c + CONTEXT_STATE) | STATE_IN_EXCEPTION);
  Slot *slot = slot_for(c);
  slot->resume_pc = pc;
  slot->resume_stack = cpu.r[1];
  slot->resumed = 0;
#ifdef RECORDS_CALLER
  slot->use_caller = 0;
#endif
  getcontext(&slot->host);
  if (slot->resumed) return;
  cpu.r[3] = kind;
  cpu.r[4] = c;
  cpu.msr &= ~INTERRUPTS_ENABLED;
  dispatch(handler);
  fprintf(stderr, "stop: exception handler %08x returned\n", handler);
  exit(6);
}
