/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <strstream>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <string>
#include <queue>
#include <zlib.h>
#include <unwind.h>

#ifdef __FreeBSD__
# include <ucontext.h>
typedef __sighandler_t *sighandler_t;
# define RIP_REGISTER(v) (v).mc_rip
#else
# define RIP_REGISTER(v) (v).gregs[REG_RIP]
#endif

#include <boost/bind.hpp>
#include <boost/optional.hpp>
#include <boost/utility/typed_in_place_factory.hpp>
#include <boost/scoped_ptr.hpp>

#include <util/pathtrack.h>
#include <util/trace.h>
#include <util/bitops.h>
#include <util/debug.h>
#include <util/ringbuffer.h>
#include <util/rank.h>
#include <util/timer.h>

#include <runtime/base/tv_macros.h>
#include <runtime/vm/bytecode.h>
#include <runtime/vm/php_debug.h>
#include <runtime/vm/runtime.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/execution_context.h>
#include <runtime/base/strings.h>
#include <runtime/base/zend/zend_string.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/server/source_root_info.h>
#include <runtime/ext/ext_continuation.h>
#include <runtime/vm/debug/debug.h>
#include <runtime/vm/translator/targetcache.h>
#include <runtime/vm/translator/log.h>
#include <runtime/vm/translator/translator-deps.h>
#include <runtime/vm/translator/translator-inline.h>
#include <runtime/vm/translator/translator-x64.h>
#include <runtime/vm/translator/asm-x64.h>
#include <runtime/vm/translator/srcdb.h>
#include <runtime/vm/translator/x64-util.h>
#include <runtime/vm/translator/unwind-x64.h>
#include <runtime/vm/pendq.h>
#include <runtime/vm/treadmill.h>
#include <runtime/vm/stats.h>
#include <runtime/vm/pendq.h>
#include <runtime/vm/treadmill.h>
#include <runtime/vm/repo.h>
#include <runtime/vm/type-profile.h>
#include <runtime/eval/runtime/file_repository.h>

using namespace HPHP::x64;
using namespace HPHP::x64::reg;
using namespace HPHP::Util;
using namespace HPHP::Trace;
using std::max;

namespace HPHP {
namespace VM {
namespace Transl {

#define TRANS_PERF_COUNTERS \
  TPC(translate) \
  TPC(retranslate) \
  TPC(interp_bb) \
  TPC(interp_instr) \
  TPC(interp_one) \
  TPC(max_trans) \
  TPC(enter_tc) \
  TPC(service_req)

#define TPC(n) "trans_" #n,
static const char* const kPerfCounterNames[] = {
  TRANS_PERF_COUNTERS
};
#undef TPC

#define TPC(n) tpc_ ## n,
enum TransPerfCounter {
  TRANS_PERF_COUNTERS
  tpc_num_counters
};
#undef TPC
static __thread int64 s_perfCounters[tpc_num_counters];
#define INC_TPC(n) ++s_perfCounters[tpc_ ## n];

static const DataType BitwiseKindOfString = KindOfString;

#define KindOfString \
#error You probably do not mean to use KindOfString in this file.

#define NULLCASE() \
  case KindOfUninit: case KindOfNull

#define STRINGCASE() \
  case BitwiseKindOfString: case KindOfStaticString

static const Trace::Module TRACEMOD = Trace::tx64;
#ifdef DEBUG
static const bool debug = true;
#else
static const bool debug = false;
#endif

#define TVOFF(nm) offsetof(TypedValue, nm)
#define AROFF(nm) offsetof(ActRec, nm)

typedef const size_t COff; // Const offsets

// nextTx64: Global shared state. The tx64 that should be used for
// new requests going forward.
TranslatorX64* volatile nextTx64;
// tx64: Thread-local state. The tx64 we're using for the current request.
__thread TranslatorX64* tx64;

// Register dirtiness: thread-private.
__thread VMRegState tl_regState = REGSTATE_CLEAN;

// RAII logger for TC space consumption.
struct SpaceRecorder {
  const char *m_name;
  const X64Assembler& m_a;
  const uint8_t *m_start;
  SpaceRecorder(const char* name, const X64Assembler& a) :
      m_name(name), m_a(a), m_start(a.code.frontier)
    { }
  ~SpaceRecorder() {
    if (Trace::moduleEnabledRelease(Trace::tcspace, 1)) {
      ptrdiff_t diff = m_a.code.frontier - m_start;
      if (diff) Trace::traceRelease("TCSpace %10s %3d\n", m_name, diff);
    }
    if (Trace::moduleEnabledRelease(Trace::tcdump, 1)) {
      Trace::traceRelease("TCDump %s", m_name);
      for (const uint8_t* p = m_start; p < m_a.code.frontier; p++) {
        Trace::traceRelease(" %x", *p);
      }
      Trace::traceRelease("\n");
    }
  }
};

// While running in the TC, rVmFp points to the ActRec of the function
// we are currently in, rVmSp point to the top of the eval stack at the
// beginning of the current tracelet, and rVmTl points to the target cache
// for the current request.
//
// Note: unwind-x64.cpp relies on the values used for these registers.
static const PhysReg rVmSp = rbx;
static const PhysReg rVmFp = rbp;
static const PhysReg rVmTl = r12;
static const PhysReg rStashedAR = r15;

static StaticString s___call(LITSTR_INIT("__call"));
static StaticString s___callStatic(LITSTR_INIT("__callStatic"));

// The x64 C ABI.
static const PhysReg argNumToRegName[] = {
  rdi, rsi, rdx, rcx, r8, r9
};
static const int kNumRegisterArgs = sizeof(argNumToRegName) /
  sizeof(PhysReg);

// Initialize at most this many locals inline in function body prologue; more
// than this, and emitting a loop is more compact. To be precise, the actual
// crossover point in terms of code size is 6; 9 was determined by experiment to
// be the optimal point in certain benchmarks. #microoptimization
static const int kLocalsToInitializeInline = 9;
static const int kMaxInlineContLocals = 10;

// An intentionally funny-looking-in-core-dumps constant for uninitialized
// instruction pointers.
static const uint64_t kUninitializedRIP = 0xba5eba11acc01ade;

static inline size_t
cellsToBytes(int nCells) {
  return nCells * sizeof(Cell);
}

static inline size_t
bytesToCells(int nBytes) {
  ASSERT(nBytes % sizeof(Cell) == 0);
  return nBytes / sizeof(Cell);
}

/*
 * tx64LocPhysicalOffset --
 *
 *   The translator uses the stack pointer slightly differently from
 *   VM::Stack. Consequently, the translated code accesses slightly
 *   different offsets from rVmSp than the C++ runtime.
 */
static inline int
tx64LocPhysicalOffset(const Location& l, const Func *f = NULL) {
  return Translator::locPhysicalOffset(l, f);
}

/*
 * Helper code for stack frames. The struct is a "template" in the
 * non-C++ sense: we don't build source-level stack frames in C++
 * for the most part, but its offsets tell us where to find fields
 * in assembly.
 *
 * If we were physically pushing stack frames, we would push them
 * in reverse order to what you see here.
 */
static void
locToRegDisp(const Location& l, PhysReg *outbase, int *outdisp,
             const Func* f = NULL) {
  assert_not_implemented((l.space == Location::Stack ||
                          l.space == Location::Local ||
                          l.space == Location::Iter));
  *outdisp = cellsToBytes(tx64LocPhysicalOffset(l, f));
  *outbase = l.space == Location::Stack ? rVmSp : rVmFp;
}

static int
localOffset(int loc) {
  PhysReg base;
  int offset;
  locToRegDisp(Location(Location::Local, loc), &base, &offset);
  ASSERT(base == rVmFp);
  return offset;
}

// Return the SrcKey for the operation that should follow the supplied
// NormalizedInstruction.  (This might not be the next SrcKey in the
// unit if we merged some instructions or otherwise modified them
// during analysis.)
static SrcKey nextSrcKey(const Tracelet& t, const NormalizedInstruction& i) {
  return i.next ? i.next->source : t.m_nextSk;
}

class RedirectSpillFill : boost::noncopyable {
  X64Assembler* const m_oldSpf;
public:
  explicit RedirectSpillFill(X64Assembler* newCode)
    : m_oldSpf(tx64->m_spillFillCode)
  {
    tx64->m_spillFillCode = newCode;
  }
  ~RedirectSpillFill() {
    tx64->m_spillFillCode = m_oldSpf;
  }
};

// DiamondGuard is a scoped way to protect register allocator state around
// control flow. When we enter some optional code that may affect the state
// of the register file, we copy the register file's state, and redirect any
// spills and fills to the branch's body.
//
// When we're ready to rejoin the main control flow, we bring the registers
// back into the state they were in, and restore the old spill/fill
// destinations.
class DiamondGuard : boost::noncopyable {
  RedirectSpillFill m_spfChanger;
public:
  explicit DiamondGuard(X64Assembler& a) : m_spfChanger(&a) {
    tx64->m_savedRegMaps.push(
      TranslatorX64::SavedRegState(this, tx64->m_regMap));
  }
  ~DiamondGuard() {
    ASSERT(!tx64->m_savedRegMaps.empty());
    ASSERT(tx64->m_savedRegMaps.top().saver == this);

    /*
     * Bring the register state back to its state in the main body.
     *
     * Note: it's important that tx64->m_regMap is the branch
     * RegAlloc during this.  See RegAlloc::reconcile.
     */
    tx64->m_savedRegMaps.top().savedState.reconcile(tx64->m_regMap);
    tx64->m_regMap = tx64->m_savedRegMaps.top().savedState;
    tx64->m_savedRegMaps.pop();
  }
};

/*
 * Helper for use with UnlikelyIfBlock when you have a complex else
 * branch that needs to make changes to the register file.
 *
 * For an example usage see UnlikelyIfBlock.
 */
class DiamondReturn : boost::noncopyable {
  X64Assembler* m_branchA;
  X64Assembler* m_mainA;
  TCA m_branchJmp;
  TCA m_finishBranchFrontier;

private:
  template<int> friend class UnlikelyIfBlock;

  void initBranch(X64Assembler* branchA, X64Assembler* mainA) {
    /*
     * DiamondReturn must be used with branches going to different
     * code regions.
     */
    ASSERT(branchA != mainA);

    m_branchA = branchA;
    m_mainA = mainA;
    tx64->m_savedRegMaps.push(
      TranslatorX64::SavedRegState(this, tx64->m_regMap));
  }

  void finishBranch(TCA jmp, TCA frontier) {
    m_branchJmp = jmp;
    m_finishBranchFrontier = frontier;

    // If there's some reason to do something other than this we have
    // to change the way this class works.
    const int UNUSED kJumpSize = 5;
    ASSERT(m_finishBranchFrontier == m_branchJmp + kJumpSize);

    // We're done with the branch, so save the branch's state and
    // switch back to the main line's state.
    swapRegMaps();
  }

private:
  bool finishedBranch() const { return m_branchJmp != 0; }

  void swapRegMaps() {
    ASSERT(!tx64->m_savedRegMaps.empty());
    ASSERT(tx64->m_savedRegMaps.top().saver == this);
    std::swap(tx64->m_savedRegMaps.top().savedState, tx64->m_regMap);
  }

  void emitReconciliation() {
    ASSERT(!tx64->m_savedRegMaps.empty());
    ASSERT(tx64->m_savedRegMaps.top().saver == this);

    RedirectSpillFill spfRedir(m_branchA);

    if (finishedBranch()) {
      // We need tx64->m_regMap to point at the branch during
      // reconciliation.  See RegAlloc::reconcile().
      swapRegMaps();
    }
    RegAlloc& branchState = tx64->m_regMap;
    RegAlloc& currentState = tx64->m_savedRegMaps.top().savedState;
    currentState.reconcile(branchState);
    tx64->m_regMap = currentState;
    tx64->m_savedRegMaps.pop();
  }

public:
  explicit DiamondReturn()
    : m_branchA(0)
    , m_branchJmp(0)
  {}

  ~DiamondReturn() {
    ASSERT(m_branchA &&
      "DiamondReturn was created without being passed to UnlikelyIfBlock");

    if (!finishedBranch()) {
      /*
       * In this case, we're reconciling the branch even though it
       * isn't really finished (no one ever called finishBranch()), so
       * we just need to emit spills/fills now and not be as clever as
       * below.  See UnlikelyIfBlock::reconcileEarly.
       */
      emitReconciliation();
      return;
    }

    const TCA currentBranchFrontier = m_branchA->code.frontier;
    const bool branchFrontierMoved =
      currentBranchFrontier != m_finishBranchFrontier;

    /*
     * If the branch frontier hasn't moved since the branch was
     * finished, we don't need the jmp that was already emitted
     * anymore, so rewind so we can potentially overwrite it with
     * spills/fills.
     */
    if (!branchFrontierMoved) {
      m_branchA->code.frontier = m_branchJmp;
    }

    // Send out reconciliation code to the branch area.  We want to
    // bring the state of the branch's register file into sync with
    // the main-line.
    const TCA spfStart = m_branchA->code.frontier;
    emitReconciliation();
    const bool hadAnySpf = spfStart != m_branchA->code.frontier;

    if (branchFrontierMoved) {
      /*
       * In this case, more than one DiamondReturn is being used and
       * there are multiple unlikely branches.
       *
       * If there was no reconciliation code it's not big deal, we'll
       * just patch the existing jmp to go to the return in m_mainA.
       * But if we needed reconciliation code, we'll instead want to
       * patch it to jump there.
       */
      if (hadAnySpf) {
        m_branchA->patchJmp(m_branchJmp, spfStart);
        m_branchA->jmp(m_mainA->code.frontier);
      } else {
        m_branchA->patchJmp(m_branchJmp, m_mainA->code.frontier);
      }
    } else {
      ASSERT(spfStart == m_branchJmp);
      m_branchA->jmp(m_mainA->code.frontier);
    }
  }
};

/*
 * Wrap this in a scope around any emit calls where the caller needs
 * to be sure the callee will not modify the state of the register
 * map.  (Arises in situations with conditional code often.)
 */
struct FreezeRegs : private boost::noncopyable {
  explicit FreezeRegs(RegAlloc& regs) : m_regs(regs) { m_regs.freeze(); }
  ~FreezeRegs() { m_regs.defrost(); }

private:
  RegAlloc& m_regs;
};

// A CondBlock is an RAII structure for emitting conditional code. It
// compares the source register at fieldOffset with fieldValue, and
// conditionally branches over the enclosing block of assembly on the
// passed-in condition-code.
//
//  E.g.:
//    {
//      RefCountedOnly ifRefCounted(a, rdi, 0);
//      emitIncRef(rdi);
//    }
//
// will only execute emitIncRef if we find at runtime that rdi points at
// a ref-counted cell.
//
// It's ok to do reconcilable register operations in the body.
template<int FieldOffset, int FieldValue, int Jcc>
struct CondBlock {
  X64Assembler& m_a;
  int m_off;
  TCA m_jcc8;
  DiamondGuard* m_dg;

  CondBlock(X64Assembler& a, PhysReg reg, int offset = 0)
      : m_a(a), m_off(offset), m_dg(new DiamondGuard(a)) {
    int typeDisp = m_off + FieldOffset;
    a.   cmp_imm32_disp_reg32(FieldValue, typeDisp, reg);
    m_jcc8 = a.code.frontier;
    a.   jcc8(Jcc, m_jcc8);
    // ...
  }

  ~CondBlock() {
    delete m_dg;
    m_a.patchJcc8(m_jcc8, m_a.code.frontier);
  }
};

// JccBlock --
//   A raw condition-code block; assumes whatever comparison or ALU op
//   that sets the Jcc has already executed.
template <int Jcc>
struct JccBlock {
  mutable X64Assembler* m_a;
  TCA m_jcc8;
  mutable DiamondGuard* m_dg;

  explicit JccBlock(X64Assembler& a)
    : m_a(&a),
      m_jcc8(a.code.frontier),
      m_dg(new DiamondGuard(a)) {
    a.    jcc8(Jcc, 0);
  }

  ~JccBlock() {
    if (m_a) {
      delete m_dg;
      m_a->patchJcc8(m_jcc8, m_a->code.frontier);
    }
  }

private:
  JccBlock(const JccBlock&);
  JccBlock& operator=(const JccBlock&);
};

/*
 * UnlikelyIfBlock:
 *
 *  Branch to distant code (that we presumably don't expect to
 *  take). This helps keep hot paths compact.
 *
 *  A common pattern using this involves patching the jump in astubs
 *  to jump past the normal control flow in a (as in the following
 *  example).  Do this using DiamondReturn so the register allocator
 *  state will be properly maintained.  (Spills/fills to keep the
 *  states in sync will be emitted on the unlikely path.)
 *
 * Example:
 *
 *  {
 *    PhysReg inputParam = i.getReg(i.inputs[0]->location);
 *    a.   test_reg_reg(inputParam, inputParam);
 *    DiamondReturn retFromStubs;
 *    {
 *      UnlikelyIfBlock<CC_Z> ifNotRax(a, astubs, &retFromStubs);
 *      EMIT_CALL(a, TCA(launch_nuclear_missiles));
 *    }
 *    // The inputParam was non-zero, here is the likely branch:
 *    m_regMap.allocOutputRegs(i);
 *    emitMovRegReg(inputParam, m_regMap.getReg(i.outLocal));
 *    // ~DiamondReturn patches the jump, and reconciles the branch
 *    // with the main line.  (In this case it will fill the outLocal
 *    // register since the main line thinks it is dirty.)
 *  }
 *  // The two cases are joined here.  We can do logic that was
 *  // independent of whether the branch was taken, if necessary.
 *  emitMovRegReg(i.outLocal, m_regMap.getReg(i.outStack));
 *
 * Note: it is ok to nest UnlikelyIfBlocks, as long as their
 * corresponding DiamondReturns are correctly destroyed in reverse
 * order.  But also note that this can lead to more jumps on the
 * unlikely branch (see ~DiamondReturn).
 */
template <int Jcc>
struct UnlikelyIfBlock {
  X64Assembler& m_likely;
  X64Assembler& m_unlikely;
  TCA m_likelyPostBranch;

  DiamondReturn* m_returnDiamond;
  bool m_externalDiamond;
  boost::optional<FreezeRegs> m_ice;

  explicit UnlikelyIfBlock(X64Assembler& likely,
                           X64Assembler& unlikely,
                           DiamondReturn* returnDiamond = 0)
    : m_likely(likely)
    , m_unlikely(unlikely)
    , m_returnDiamond(returnDiamond ? returnDiamond : new DiamondReturn())
    , m_externalDiamond(!!returnDiamond)
  {
    m_likely.jcc(Jcc, m_unlikely.code.frontier);
    m_likelyPostBranch = m_likely.code.frontier;
    m_returnDiamond->initBranch(&unlikely, &likely);
    tx64->m_spillFillCode = &unlikely;
  }

  ~UnlikelyIfBlock() {
    TCA jmpAddr = m_unlikely.code.frontier;
    m_unlikely.jmp(m_likelyPostBranch);
    if (m_returnDiamond) {
      m_returnDiamond->finishBranch(jmpAddr, m_unlikely.code.frontier);
      if (!m_externalDiamond) {
        delete m_returnDiamond;
      }
    }
    tx64->m_spillFillCode = &m_likely;
  }

  /*
   * Force early reconciliation between the branch and main line.
   * Using this has some tricky cases, part of which is that you can't
   * allocate registers anymore in the branch if you do this (so we'll
   * freeze until ~UnlikelyIfBlock).
   *
   * It's also almost certainly error if we have m_externalDiamond, so
   * for now that's an assert.
   */
  void reconcileEarly() {
    ASSERT(!m_externalDiamond);
    delete m_returnDiamond;
    m_returnDiamond = 0;
    m_ice = boost::in_place<FreezeRegs>(boost::ref(tx64->m_regMap));
  }
};

// IfElseBlock: like CondBlock, but with an else clause.
//    a.   test_reg_reg(rax, rax);
//    {  IfElseBlock<CC_Z> ifRax(a);
//       // Code executed for rax != 0
//       ifRax.Else();
//       // Code executed for rax == 0
//    }
//
template <int Jcc>
class IfElseBlock : boost::noncopyable {
  X64Assembler& m_a;
  TCA m_jcc8;
  TCA m_jmp8;
 public:
  explicit IfElseBlock(X64Assembler& a) :
    m_a(a), m_jcc8(a.code.frontier), m_jmp8(NULL) {
    tx64->m_regMap.freeze();
    m_a.jcc8(Jcc, m_jmp8);  // 1f
  }
  void Else() {
    ASSERT(m_jmp8 == NULL);
    m_jmp8 = m_a.code.frontier;
    m_a.jmp8(m_jmp8); // 2f
    // 1:
    m_a.patchJcc8(m_jcc8, m_a.code.frontier);
  }
  ~IfElseBlock() {
    ASSERT(m_jmp8 != NULL);
    // 2:
    m_a.patchJmp8(m_jmp8, m_a.code.frontier);
    tx64->m_regMap.defrost();
  }
};

static bool
typeCanBeStatic(DataType t) {
  return t != KindOfObject && t != KindOfRef;
}

// IfRefCounted --
//   Emits if (IS_REFCOUNTED_TYPE()) { ... }
typedef CondBlock <TVOFF(m_type),
                   KindOfRefCountThreshold,
                   CC_LE> IfRefCounted;

typedef CondBlock <TVOFF(m_type),
                   KindOfRef,
                   CC_NZ> IfVariant;

// IfCountNotStatic --
//   Emits if (%reg->_count != RefCountStaticValue) { ... }.
//   May short-circuit this check if the type is known to be
//   static already.
struct IfCountNotStatic {
  typedef CondBlock<TVOFF(_count),
                    RefCountStaticValue,
                    CC_Z> NonStaticCondBlock;
  NonStaticCondBlock *m_cb; // might be null
  IfCountNotStatic(X64Assembler& a,
                   PhysReg reg,
                   DataType t = KindOfInvalid,
                   RegAlloc* rm = NULL) {
    // Objects and variants cannot be static
    if (typeCanBeStatic(t)) {
      m_cb = new NonStaticCondBlock(a, reg);
    } else {
      m_cb = NULL;
    }
  }

  ~IfCountNotStatic() {
    delete m_cb;
  }
};

// Segfault handler: figure out if it's an intentional segfault
// (timeout exception) and if so, act appropriately. Otherwise, pass
// the signal on.
void TranslatorX64::SEGVHandler(int signum, siginfo_t *info, void *ctx) {
  TranslatorX64 *self = Get();
  void *surprisePage =
    ThreadInfo::s_threadInfo->m_reqInjectionData.surprisePage;
  if (info->si_addr == surprisePage) {
    ucontext_t *ucontext = (ucontext_t*)ctx;
    TCA rip = (TCA)RIP_REGISTER(ucontext->uc_mcontext);
    SignalStubMap::const_accessor a;
    if (!self->m_segvStubs.find(a, rip)) {
      NOT_REACHED();
    }
    TCA astubsCall = a->second;

    // When this handler returns, "call" the astubs code for this
    // surprise check.
    RIP_REGISTER(ucontext->uc_mcontext) = (uintptr_t)astubsCall;

    // We've processed this event; reset the page in case execution
    // continues normally.
    g_vmContext->m_stack.unprotect();
  } else {
    sighandler_t handler = (sighandler_t)self->m_segvChain;
    if (handler == SIG_DFL || handler == SIG_IGN) {
      signal(signum, handler);
      raise(signum);
    } else {
      self->m_segvChain(signum, info, ctx);
    }
  }
}

// emitDispDeref --
// emitDeref --
// emitTypedValueStore --
// emitStoreUninitNull --
// emitStoreNull --
//
//   Helpers for common cell operations.
//
//   Dereference the var in the cell whose address lives in src into
//   dest.
static void
emitDispDeref(X64Assembler &a, PhysReg src, int disp,
              PhysReg dest) {
  a.    load_reg64_disp_reg64(src, disp + TVOFF(m_data), dest);
}

static void
emitDeref(X64Assembler &a, PhysReg src, PhysReg dest) {
  emitDispDeref(a, src, 0, dest);
}

static void
emitDerefIfVariant(X64Assembler &a, PhysReg reg) {
  if (RuntimeOption::EvalJitCmovVarDeref) {
    a.cmp_imm32_disp_reg32(KindOfRef, TVOFF(m_type), reg);
    a.cload_reg64_disp_reg64(CC_Z, reg, 0, reg);
  } else {
    IfVariant ifVar(a, reg);
    emitDeref(a, reg, reg);
  }
}

// NB: leaves count field unmodified.
static void
emitTypedValueStore(X64Assembler& a, DataType type, PhysReg val,
                    int disp, PhysReg dest) {
  a.    store_imm32_disp_reg(type, disp + TVOFF(m_type), dest);
  a.    store_reg64_disp_reg64(val, disp + TVOFF(m_data), dest);
}

// Assumes caller has already zeroed out zeroedReg
static void
emitStoreUninitNull(X64Assembler& a,
                    int disp,
                    PhysReg dest) {
  // OK to leave garbage in m_data, _count.
  a.    store_imm32_disp_reg(KindOfUninit, disp + TVOFF(m_type), dest);
}

static void
emitStoreNull(X64Assembler& a,
              int disp,
              PhysReg dest) {
  a.    store_imm32_disp_reg(KindOfNull, disp + TVOFF(m_type), dest);
  // It's ok to leave garbage in m_data, _count for KindOfNull.
}

static void
emitStoreNull(X64Assembler& a, const Location& where) {
  PhysReg base;
  int disp;
  locToRegDisp(where, &base, &disp);
  emitStoreNull(a, disp, base);
}

/*
 * Emit code that does the same thing as tvSet().
 *
 * The `oldType' and `oldData' registers are used for temporary
 * storage and unconditionally destroyed; `toPtr' may be destroyed;
 * `from' will not be modified.
 */
void TranslatorX64::emitTvSetRegSafe(const NormalizedInstruction& i,
                                     PhysReg from,
                                     DataType fromType,
                                     PhysReg toPtr,
                                     PhysReg oldType,
                                     PhysReg oldData) {
  ASSERT(!i.isNative());
  ASSERT(!i.isSimple());
  ASSERT(fromType != KindOfRef);

  emitDerefIfVariant(a, toPtr);
  a.  load_reg64_disp_reg32(toPtr, TVOFF(m_type), oldType);
  a.  load_reg64_disp_reg64(toPtr, TVOFF(m_data), oldData);
  emitTypedValueStore(a, fromType, from, 0, toPtr);
  emitIncRef(from, fromType);
  a.  cmp_imm32_reg32(KindOfRefCountThreshold, oldType);
  {
    JccBlock<CC_LE> ifRefCounted(a);
    {
      IfCountNotStatic ins(a, oldData, KindOfInvalid);
      a.  sub_imm32_disp_reg32(1, TVOFF(_count), oldData);
      {
        JccBlock<CC_NZ> ifZero(a);
        callBinaryStub(a, i, m_typedDtorStub, oldData, oldType);
      }
    }
  }
}

void TranslatorX64::emitTvSet(const NormalizedInstruction& i,
                              PhysReg from,
                              DataType fromType,
                              PhysReg toPtr) {
  ScratchReg oldType(m_regMap);
  ScratchReg oldData(m_regMap);
  emitTvSetRegSafe(i, from, fromType, toPtr, *oldType, *oldData);
}

// Logical register move: ensures the value in src will be in dest
// after execution, but might do so in strange ways. Do not count on
// being able to smash dest to a different register in the future, e.g.
void
TranslatorX64::emitMovRegReg(X64Assembler& a, PhysReg src, PhysReg dest) {
  SpaceRecorder("_RegMove", a);
  if (src != dest) {
    a.  mov_reg64_reg64(src, dest);
  }
}

void
TranslatorX64::emitMovRegReg(PhysReg src, PhysReg dest) {
  emitMovRegReg(a, src, dest);
}

// vstackOffset --
// emitVStackStore --
//
//   Store to the virtual stack at a normalized instruction boundary.
//   Since rVmSp points to the stack at entry to the current BB, we need to
//   adjust stack references relative to it.
//
//   For all parameters, offsets and sizes are in bytes. Sizes are expected
//   to be a hardware size: 1, 2, 4, or 8 bytes.
static int
vstackOffset(const NormalizedInstruction& ni, COff off) {
  return off - cellsToBytes(ni.stackOff);
}

static void
emitStoreImm(X64Assembler& a, uint64_t imm, PhysReg r, int off,
             int size = sz::qword, RegAlloc* regAlloc = NULL) {
  if (size == sz::qword) {
    PhysReg immReg = regAlloc ? regAlloc->getImmReg(imm) : InvalidReg;
    if (immReg == InvalidReg) {
      emitImmReg(a, imm, rScratch);
      immReg = rScratch;
    }
    a.   store_reg64_disp_reg64(immReg, off, r);
  } else if (size == sz::dword) {
    a.   store_imm32_disp_reg(imm, off, r);
  } else {
    not_implemented();
  }
}

static void
emitVStackStoreImm(X64Assembler &a, const NormalizedInstruction &ni,
                   uint64_t imm, int off, int size = sz::qword,
                   RegAlloc *regAlloc = NULL) {
  int hwOff = vstackOffset(ni, off);
  emitStoreImm(a, imm, rVmSp, hwOff, size, regAlloc);
}

static void
emitVStackStore(X64Assembler &a, const NormalizedInstruction &ni,
                PhysReg src, int off, int size = sz::qword) {
  int hwOff = vstackOffset(ni, off);
  if (size == sz::qword) {
    a.    store_reg64_disp_reg64(src, hwOff, rVmSp);
  } else if (size == sz::dword) {
    a.    store_reg32_disp_reg64(src, hwOff, rVmSp);
  } else {
    not_implemented();
  }
}

/*
 * The 'zero' argument can be noreg, rFlag, or a normal register
 * name. If it's noreg, a 0 immediate will be stored to _count. If
 * it's rFlag, nothing will be stored to _count. If it's a normal
 * register name, the contents of that register (hopefully set to zero
 * by the caller) are stored to _count.
 */
static void
emitCopyTo(X64Assembler& a,
           PhysReg src,
           int srcOff,
           PhysReg dest,
           int destOff,
           PhysReg scratch,
           PhysReg zero = noreg) {
  ASSERT(src != scratch);
  // This is roughly how gcc compiles this.
  a.    load_reg64_disp_reg64(src, srcOff + TVOFF(m_data), scratch);
  if (zero == noreg) {
    a.    store_imm32_disp_reg(0, destOff + TVOFF(_count), dest);
  } else if (zero != rFlag) {
    a.    store_reg32_disp_reg64(zero, destOff + TVOFF(_count), dest);
  }
  a.    store_reg64_disp_reg64(scratch, destOff + TVOFF(m_data), dest);
  a.    load_reg64_disp_reg32(src, srcOff + TVOFF(m_type), scratch);
  a.    store_reg32_disp_reg64(scratch, destOff + TVOFF(m_type), dest);
}

/*
 * Copy a heap cell from memory to the stack. Takes care to honor
 * the system invariant that _count == 0.
 *
 * Use emitCopyToStack when you can safely change the state of the
 * register map.  When using emitCopyToStackRegSafe, you'll need to
 * invalidate the stack location manually at an appropriate time.
 */

void TranslatorX64::emitCopyToStackRegSafe(X64Assembler& a,
                                           const NormalizedInstruction& ni,
                                           PhysReg src,
                                           int off,
                                           PhysReg tmpReg) {
  emitCopyTo(a, src, 0, rVmSp, vstackOffset(ni, off), tmpReg);
}

void
TranslatorX64::emitCopyToStack(X64Assembler& a,
                               const NormalizedInstruction& ni,
                               PhysReg src,
                               int off) {
  ScratchReg scratch(m_regMap);
  {
    FreezeRegs freeze(m_regMap);
    emitCopyToStackRegSafe(a, ni, src, off, *scratch);
  }
  // Forget whatever we thought we knew about the stack.
  m_regMap.invalidate(ni.outStack->location);
}

/*
 * emitPushAR --
 *
 *   Push an activation record. Will return to the next instruction emitted by
 *   the invoker. Called on behalf of FPushFuncD and FPushFunc. If func is
 *   unknown, we will leave it to the caller to fill in m_func.
 */
void
TranslatorX64::emitPushAR(const NormalizedInstruction& i, const Func* func,
                          const int bytesPopped /* = 0 */,
                          bool isCtor /* = false */,
                          bool clearThis /* = true */,
                          uintptr_t varEnvInvName /* = 0 */) {
  if (func && phpBreakpointEnabled(func->name()->data())) {
    translator_debug_break(a);
  }
  ASSERT(sizeof(Cell) < sizeof(ActRec));
  // We are about to push an ActRec onto the stack. The stack grows down,
  // so the offset of the beginning of the ActRec from the top of stack
  // is -sizeof(ActRec).
  int numArgs = i.imm[0].u_IVA;
  int startOfActRec = bytesPopped - sizeof(ActRec);
  size_t funcOff     = startOfActRec + AROFF(m_func);
  size_t thisOff     = startOfActRec + AROFF(m_this);
  size_t nargsOff    = startOfActRec + AROFF(m_numArgsAndCtorFlag);
  size_t varenvOff   = startOfActRec + AROFF(m_varEnv);
  size_t savedRbpOff = startOfActRec + AROFF(m_savedRbp);

  BOOST_STATIC_ASSERT((
    sizeof(((ActRec*)NULL)->m_numArgsAndCtorFlag) == sizeof(int32_t)
  ));
  /*
   * rVmSp might not be up-to-date here, so we use emitVStackStore and
   * emitVStackStoreImm which know how to compute the where the top of
   * stack currently is.
   */
  if (func) {
    emitVStackStoreImm(a, i, (uintptr_t)func, funcOff);
    if (clearThis) {
      emitVStackStoreImm(a, i, 0,               thisOff, sz::qword, &m_regMap);
    }
  }
  emitVStackStoreImm(a, i,   ActRec::encodeNumArgs(numArgs, isCtor),
                                              nargsOff, sz::dword);
  emitVStackStoreImm(a, i,   varEnvInvName,   varenvOff, sz::qword, &m_regMap);
  emitVStackStore(a, i,      rVmFp,           savedRbpOff, sz::qword);
}

static inline bool
regIsInCallingSequence(PhysReg r) {
  return r == rax || r == rdi || r == rsi || r == rcx || r == rdx ||
         r == r8  || r == r9;
}

/*
 * Registers available for program locations.
 *
 * rsp is reserved by the runtime.
 * r10 is reserved by the assembler.
 * rbp, r12, and rbx are statically allocated.
 */
static const RegSet kCallerSaved =
  RegSet(rax) | RegSet(rcx) | RegSet(rdx) | RegSet(rsi) |
  RegSet(rdi) | RegSet(r8)  | RegSet(r9)  | RegSet(r11);

// Keep this list in sync with behavior in unwind-x64.cpp.
static const RegSet kCalleeSaved = RegSet(r13) | RegSet(r14) | RegSet(r15);

static const RegSet kAllRegs = kCalleeSaved | kCallerSaved;

template<int StackParity>
class PhysRegSaverParity {
protected:
  X64Assembler& a;
  RegSet s;
  int numElts;
public:
  PhysRegSaverParity(X64Assembler& a_, RegSet s_) : a(a_), s(s_) {
    RegSet sCopy = s;
    numElts = 0;
    PhysReg reg;
    while (sCopy.findFirst(reg)) {
      a.   pushr(reg);
      sCopy.remove(reg);
      numElts++;
    }
    if ((numElts & 1) == StackParity) {
      // Maintain stack evenness for SIMD compatibility.
      a.   sub_imm32_reg64(8, rsp);
    }
  }

  ~PhysRegSaverParity() {
    if ((numElts & 1) == StackParity) {
      // See above; stack parity.
      a.   add_imm32_reg64(8, rsp);
    }
    RegSet sCopy = s;
    PhysReg reg;
    while (sCopy.findLast(reg)) {
      a.   popr(reg);
      sCopy.remove(reg);
    }
  }
};

// In shared stubs, we've already made the stack odd by calling
// from a to astubs. Calls from a are on an even rsp.
typedef PhysRegSaverParity<0> PhysRegSaverStub;
typedef PhysRegSaverParity<1> PhysRegSaver;

void
TranslatorX64::emitCallSaveRegs() {
  ASSERT(!m_regMap.frozen());
  m_regMap.cleanRegs(kCallerSaved);
}

class ArgManager {
public:
  ArgManager(TranslatorX64 &tx64, X64Assembler& a) : m_tx64(tx64), m_a(a) { }

  void addImm(uint64_t imm) {
    TRACE(6, "ArgManager: push arg %zd imm:%lu\n",
          m_args.size(), imm);
    m_args.push_back(ArgContent(ArgContent::ArgImm, InvalidReg, imm));
  }

  void addLoc(const Location &loc) {
    TRACE(6, "ArgManager: push arg %zd loc:(%s, %lld)\n",
          m_args.size(), loc.spaceName(), loc.offset);
    m_args.push_back(ArgContent(ArgContent::ArgLoc, loc));
  }

  void addDeref(const Location &loc) {
    TRACE(6, "ArgManager: push arg %zd deref:(%s, %lld)\n",
          m_args.size(), loc.spaceName(), loc.offset);
    m_args.push_back(ArgContent(ArgContent::ArgDeref, loc));
  }

  void addReg(PhysReg reg) {
    TRACE(6, "ArgManager: push arg %zd reg:r%d\n",
          m_args.size(), reg);
    m_args.push_back(ArgContent(ArgContent::ArgReg, reg, 0));
  }

  void addRegPlus(PhysReg reg, int32_t off) {
    TRACE(6, "ArgManager: push arg %zd regplus:r%d+%d\n",
          m_args.size(), reg, off);
    m_args.push_back(ArgContent(ArgContent::ArgRegPlus, reg, off));
  }

  void addLocAddr(const Location &loc) {
    TRACE(6, "ArgManager: push arg %zd addr:(%s, %lld)\n",
          m_args.size(), loc.spaceName(), loc.offset);
    ASSERT(!loc.isLiteral());
    m_args.push_back(ArgContent(ArgContent::ArgLocAddr, loc));
  }

  void emitArguments() {
    size_t n = m_args.size();
    ASSERT((int)n <= kNumRegisterArgs);
    cleanLocs();
    std::map<PhysReg, size_t> used;
    std::vector<PhysReg> actual(n, InvalidReg);
    computeUsed(used, actual);
    shuffleRegisters(used, actual);
    emitValues(actual);
  }

private:
  struct ArgContent {
    enum ArgKind {
      ArgImm, ArgLoc, ArgDeref, ArgReg, ArgRegPlus, ArgLocAddr
    } m_kind;
    PhysReg m_reg;
    const Location *m_loc;
    uint64_t m_imm;

    ArgContent(ArgKind kind, PhysReg reg, uint64_t imm) :
      m_kind(kind), m_reg(reg), m_loc(NULL), m_imm(imm) { }
    ArgContent(ArgKind kind, const Location &loc) :
      m_kind(kind), m_reg(InvalidReg), m_loc(&loc), m_imm(0) { }
  };

  TranslatorX64& m_tx64;
  X64Assembler& m_a;
  std::vector<ArgContent> m_args;

  ArgManager(); // Don't build without reference to translator

  void cleanLocs();
  void computeUsed(std::map<PhysReg, size_t> &used,
                   std::vector<PhysReg> &actual);
  void shuffleRegisters(std::map<PhysReg, size_t> &used,
                     std::vector<PhysReg> &actual);
  void emitValues(std::vector<PhysReg> &actual);
};

void ArgManager::cleanLocs() {
  for (size_t i = 0; i < m_args.size(); ++i) {
    // We only need to clean locations we are passing the address of.
    // (ArgLoc passes the value in the register mapped for a given
    // location, not the address of the location itself, so it doesn't
    // need cleaning here.)
    if (m_args[i].m_kind != ArgContent::ArgLocAddr) continue;
    if (m_tx64.m_regMap.hasReg(*m_args[i].m_loc)) {
      m_tx64.m_regMap.cleanLoc(*m_args[i].m_loc);
    }
  }
}

void ArgManager::computeUsed(std::map<PhysReg, size_t> &used,
                             std::vector<PhysReg> &actual) {
  size_t n = m_args.size();
  for (size_t i = 0; i < n; i++) {
    PhysReg reg = InvalidReg;
    if (m_args[i].m_kind == ArgContent::ArgReg ||
        m_args[i].m_kind == ArgContent::ArgRegPlus) {
      reg = m_args[i].m_reg;
    } else if (m_args[i].m_kind == ArgContent::ArgLoc ||
               m_args[i].m_kind == ArgContent::ArgDeref) {
      reg = m_tx64.getReg(*m_args[i].m_loc);
    } else {
      continue;
    }
    TRACE(6, "ArgManager: arg %zd incoming reg r%d\n", i, reg);
    used[reg] = i;
    actual[i] = reg;
  }
}

void
TranslatorX64::emitRB(X64Assembler& a,
                      RingBufferType t,
                      SrcKey sk, RegSet toSave) {
  if (!Trace::moduleEnabledRelease(Trace::tx64, 5)) {
    return;
  }
  PhysRegSaver rs(a, toSave);
  int arg = 0;
  emitImmReg(a, t, argNumToRegName[arg++]);
  emitImmReg(a, sk.m_funcId, argNumToRegName[arg++]);
  emitImmReg(a, sk.m_offset, argNumToRegName[arg++]);
  a.    call((TCA)ringbufferEntry);
}

void
TranslatorX64::emitRB(X64Assembler& a,
                      RingBufferType t,
                      const char* msg,
                      RegSet toSave) {
  if (!Trace::moduleEnabledRelease(Trace::tx64, 5)) {
    return;
  }
  PhysRegSaver save(a, toSave);
  int arg = 0;
  emitImmReg(a, (uintptr_t)msg, argNumToRegName[arg++]);
  emitImmReg(a, strlen(msg), argNumToRegName[arg++]);
  emitImmReg(a, t, argNumToRegName[arg++]);
  a.    call((TCA)ringbufferMsg);
}

/*
 * allocate the input registers for i, trying to
 * match inputs to call arguments.
 * if args[j] == ArgDontAllocate, the arg is skipped
 * if args[j] == ArgAnyReg, it will be allocated as normal
 * otherwise, args[j] should be a positional call argument,
 * and allocInputsForCall will attempt to allocate it to
 * argNumToRegName[args[j]].
 */
void
TranslatorX64::allocInputsForCall(const NormalizedInstruction& i,
                                  const int* args) {
  RegSet blackList;
  int arg;
  /*
   * If any of the inputs is already in an argument
   * register, blacklist it. ArgManager already takes
   * care of shuffling registers efficiently
   */
  for (arg = i.inputs.size(); arg--; ) {
    if (args[arg] != ArgDontAllocate &&
        m_regMap.hasReg(i.inputs[arg]->location)) {
      blackList |= RegSet(getReg(i.inputs[arg]->location));
    }
  }
  bool hasAnyReg = false;
  for (arg = i.inputs.size(); arg--; ) {
    if (args[arg] != ArgAnyReg) {
      if (args[arg] != ArgDontAllocate &&
        !m_regMap.hasReg(i.inputs[arg]->location)) {
        PhysReg target = argNumToRegName[args[arg]];
        if (!blackList.contains(target)) {
          m_regMap.cleanRegs(RegSet(target));
          m_regMap.smashRegs(RegSet(target));
        } else {
          target = InvalidReg;
        }
        m_regMap.allocInputReg(i, arg, target);
      }
    } else {
      hasAnyReg = true;
    }
  }
  if (hasAnyReg) {
    for (arg = i.inputs.size(); arg--; ) {
      if (args[arg] == ArgAnyReg) {
        m_regMap.allocInputReg(i, arg);
      }
    }
  }
}

void ArgManager::shuffleRegisters(std::map<PhysReg, size_t> &used,
                                  std::vector<PhysReg> &actual) {
  size_t n = m_args.size();
  for (size_t i = 0; i < n; i++) {
    if (actual[i] == InvalidReg)
      continue;

    if (!mapContains(used, argNumToRegName[i])) {
      // There's no conflict, so just copy
      TRACE(6, "ArgManager: arg %zd reg available, copying from r%d to r%d\n",
            i, actual[i], argNumToRegName[i]);
      // Do copy and data structure update here, because this way
      // we can reuse the register in actual[i] later without problems.
      m_tx64.emitMovRegReg(m_a, actual[i], argNumToRegName[i]);
      used.erase(actual[i]);
      actual[i] = argNumToRegName[i];
    } else {
      size_t j = used[argNumToRegName[i]];
      if (actual[j] != actual[i]) {
        // The register is used by some other value, so we must swap the two
        // registers.
        ASSERT(j > i);
        ASSERT(actual[j] != InvalidReg);
        PhysReg ri = actual[i],
                rj = actual[j];
        TRACE(6, "ArgManager: arg %zd register used by arg %zd, "
                 "swapping r%d with r%d\n", i, j, ri, rj);

        // Clean the registers first
        RegSet regs = RegSet(ri) | RegSet(rj);
        m_tx64.m_regMap.cleanRegs(regs);

        // Emit the actual swap
        m_tx64.m_regMap.swapRegisters(ri, rj);
        m_a.  xchg_reg64_reg64(ri, rj);

        // Update the data structure for later steps
        for (size_t k = 0; k < n; k++) {
          if (actual[k] == ri) {
            actual[k] = rj;
          } else if (actual[k] == rj) {
            actual[k] = ri;
          }
        }
        used[ri] = j;
        used[rj] = i;
      }
    }
  }
}

void ArgManager::emitValues(std::vector<PhysReg> &actual) {
  for (size_t i = 0; i < m_args.size(); i++) {
    switch(m_args[i].m_kind) {
    case ArgContent::ArgLoc:
    case ArgContent::ArgDeref:
    case ArgContent::ArgReg:
      TRACE(6, "ArgManager: copying arg %zd from r%d to r%d\n",
            i, actual[i], argNumToRegName[i]);
      m_tx64.emitMovRegReg(m_a, actual[i], argNumToRegName[i]);
      // Emit dereference if needed
      if (m_args[i].m_kind == ArgContent::ArgDeref) {
        emitDeref(m_a, argNumToRegName[i], argNumToRegName[i]);
      }
      break;

    // For any of these cases, the register should already be available.
    // If it was used previously by an input value, shuffleRegisters
    // should have moved it to the proper register from argNumToRegName.
    case ArgContent::ArgImm:
      emitImmReg(m_a, m_args[i].m_imm, argNumToRegName[i]);
      break;

    case ArgContent::ArgRegPlus:
      if (m_args[i].m_imm) {
        m_a.  add_imm32_reg64(m_args[i].m_imm, argNumToRegName[i]);
      }
      break;

    case ArgContent::ArgLocAddr:
      {
        PhysReg base;
        int disp;
        locToRegDisp(*m_args[i].m_loc, &base, &disp);
        m_a.  lea_reg64_disp_reg64(base, disp, argNumToRegName[i]);
      }
      break;

    default:
      // Should never happen
      ASSERT(false);
    }
  }
}

void
TranslatorX64::emitCall(X64Assembler& a, TCA dest, bool killRegs) {
  if (a.jmpDeltaFits(dest)) {
    a.    call(dest);
  } else {
    a.    call(getNativeTrampoline(dest));
  }
  if (killRegs) {
    // All caller-saved regs are now suspect.
    m_regMap.smashRegs(kCallerSaved);
  }
}

void
TranslatorX64::recordSyncPoint(X64Assembler& a, Offset pcOff, Offset spOff) {
  m_pendingFixups.push_back(PendingFixup(a.code.frontier,
                                         Fixup(pcOff, spOff)));
}

template <bool reentrant>
void
TranslatorX64::recordCallImpl(X64Assembler& a,
                              const NormalizedInstruction& i,
                              const SrcKey* inputSk /* = 0 */) {
  SrcKey sk = inputSk ? *inputSk : i.source;
  ASSERT(i.checkedInputs ||
         (reentrant && !i.isSimple()) ||
         (!reentrant && !i.isNative()));
  Offset pcOff = sk.offset() - curFunc()->base();
  SKTRACE(2, sk, "record%sCall pcOff %d\n",
             reentrant ? "Reentrant" : "", int(pcOff));
  Offset stackOff = i.stackOff + (vmfp() - vmsp());
  recordSyncPoint(a, pcOff, stackOff);
  SKTRACE(2, sk, "record%sCall stackOff %d\n",
             reentrant ? "Reentrant" : "", int(stackOff));

  /*
   * Right now we assume call sites that need to record sync points
   * may also throw exceptions.  We record information about dirty
   * callee-saved registers so we can spill their contents during
   * unwinding.  See unwind-x64.cpp.
   */
  if (!m_pendingUnwindRegInfo.empty()) {
    if (Trace::moduleLevel(Trace::tunwind) >= 2) {
      sk.trace("recordCallImpl has dirty callee-saved regs\n");
      TRACE_MOD(Trace::tunwind, 2,
                   "CTCA: %p saving dirty callee regs:\n",
                   a.code.frontier);
      for (int i = 0; i < UnwindRegInfo::kMaxCalleeSaved; ++i) {
        if (m_pendingUnwindRegInfo.m_regs[i].dirty) {
          TRACE_MOD(Trace::tunwind, 2, "  %s\n",
                    m_pendingUnwindRegInfo.m_regs[i].pretty().c_str());
        }
      }
    }
    m_unwindRegMap.insert(a.code.frontier, m_pendingUnwindRegInfo);
    m_pendingUnwindRegInfo.clear();
  }
}

void TranslatorX64::prepareCallSaveRegs() {
  emitCallSaveRegs(); // Clean caller-saved regs.
  m_pendingUnwindRegInfo.clear();

  RegSet rset = kCalleeSaved;
  PhysReg reg;
  while (rset.findFirst(reg)) {
    rset.remove(reg);
    if (!m_regMap.regIsDirty(reg)) continue;
    const RegInfo* ri = m_regMap.getInfo(reg);
    ASSERT(ri->m_cont.m_kind == RegContent::Loc);

    // If the register is dirty, we'll record this so that we can
    // restore it during stack unwinding if an exception is thrown.
    m_pendingUnwindRegInfo.add(reg, ri->m_type, ri->m_cont.m_loc);
  }
}

// Some macros to make writing calls palatable. You have to "type" the
// arguments
#define EMIT_CALL_PROLOGUE(a) do { \
  SpaceRecorder sr("_HCallInclusive", a);  \
  ArgManager _am(*this, a);       \
  prepareCallSaveRegs();

#define EMIT_CALL_EPILOGUE(a, dest) \
  _am.emitArguments();           \
  { \
    SpaceRecorder sr("_HCallExclusive", a); \
    emitCall(a, (TCA)(dest), true);         \
  } \
} while(0)

#define IMM(i)       _am.addImm(i)
#define V(loc)       _am.addLoc(loc)
#define DEREF(loc)   _am.addDeref(loc)
#define R(r)         _am.addReg(r)
#define RPLUS(r,off) _am.addRegPlus(r, off)
#define A(loc)       _am.addLocAddr(loc)

#define EMIT_CALL(a, dest, ...) \
  EMIT_CALL_PROLOGUE(a)         \
  __VA_ARGS__ ;                 \
  EMIT_CALL_EPILOGUE(a, dest)

void
TranslatorX64::emitIncRef(PhysReg base, DataType dtype) {
  if (!IS_REFCOUNTED_TYPE(dtype) && dtype != KindOfInvalid) {
    return;
  }
  ASSERT(m_regMap.getInfo(base));
  SpaceRecorder sr("_IncRef", a);
  ASSERT(sizeof(((Cell*)NULL)->_count == sizeof(int32_t)));
  { // if !static then
    IfCountNotStatic ins(a, base, dtype);
    /*
     * The optimization guide cautions against using inc; while it is
     * compact, it only writes the low-order 8 bits of eflags, causing a
     * partial dependency for any downstream flags-dependent code.
     */
    a.    add_imm32_disp_reg32(1, TVOFF(_count), base);
  } // endif
}

void
TranslatorX64::emitIncRefGenericRegSafe(PhysReg base,
                                        int disp,
                                        PhysReg tmpReg) {
  ASSERT(m_regMap.getInfo(base));
  { // if RC
    IfRefCounted irc(a, base, disp);
    a.    load_reg64_disp_reg64(base, disp + TVOFF(m_data),
                                tmpReg);
    { // if !static
      IfCountNotStatic ins(a, tmpReg);
      a.  add_imm32_disp_reg32(1, TVOFF(_count), tmpReg);
    } // endif
  } // endif
}

void TranslatorX64::emitIncRefGeneric(PhysReg base, int disp) {
  ScratchReg tmpReg(m_regMap);
  emitIncRefGenericRegSafe(base, disp, *tmpReg);
}

static void emitGetGContext(X64Assembler& a, PhysReg dest) {
  emitTLSLoad<ExecutionContext>(a, g_context, dest);
}

// emitEagerVMRegSave --
//   Inline. Saves regs in-place in the TC. This is an unusual need;
//   you probably want to lazily save these regs via recordCall and
//   its ilk.
//
//   SaveFP uses rVmFp, as usual. SavePC requires the caller to have
//   placed the PC offset of the instruction about to be executed in
//   rdi.
enum RegSaveFlags {
  SaveFP = 1,
  SavePC = 2
};

static TCA
emitEagerVMRegSave(X64Assembler& a,
                   int flags /* :: RegSaveFlags */) {
  TCA start = a.code.frontier;
  bool saveFP = bool(flags & SaveFP);
  bool savePC = bool(flags & SavePC);
  ASSERT((flags & ~(SavePC | SaveFP)) == 0);

  PhysReg pcReg = rdi;
  PhysReg rEC = rScratch;

  emitGetGContext(a, rEC);

  static COff spOff = offsetof(VMExecutionContext, m_stack) +
    Stack::topOfStackOffset();
  static COff fpOff = offsetof(VMExecutionContext, m_fp) - spOff;
  static COff pcOff = offsetof(VMExecutionContext, m_pc) - spOff;

  ASSERT(spOff != 0);
  // Instruction selection note: this is an lea, but add is more
  // compact and we can afford the flags bash.
  a.    add_imm32_reg64(spOff, rEC);
  a.    store_reg64_disp_reg64 (rVmSp, 0, rEC);
  if (savePC) {
    // We're going to temporarily abuse rVmSp to hold the current unit.
    PhysReg rBC = rVmSp;
    a.  pushr(rBC);
    // m_fp -> m_func -> m_unit -> m_bc + pcReg
    a.  load_reg64_disp_reg64(rVmFp, AROFF(m_func), rBC);
    a.  load_reg64_disp_reg64(rBC, Func::unitOff(), rBC);
    a.  load_reg64_disp_reg64(rBC, Unit::bcOff(), rBC);
    a.  add_reg64_reg64(rBC, pcReg);
    a.  store_reg64_disp_reg64(pcReg, pcOff, rEC);
    a.  popr(rBC);
  }
  if (saveFP) {
    a.  store_reg64_disp_reg64 (rVmFp, fpOff, rEC);
  }
  return start;
}

// Returns whether the stub in m_dtorStubs for a given type can
// possibly reenter or not.  (I.e. could it run a user-declared
// destructor.)
static bool canDtorStubReenter(DataType type) {
  return IS_REFCOUNTED_TYPE(type) && type != BitwiseKindOfString;
}

/**
 * emitDecRef --
 *
 *   Decrement a value's refcount and call the release helper if
 *   appropriate. emitDecRef requires that the caller knows the
 *   type at translation time.
 */
void TranslatorX64::emitDecRef(Asm& a,
                               const NormalizedInstruction& i,
                               PhysReg rDatum,
                               DataType type) {
  ASSERT(type != KindOfInvalid);
  if (!IS_REFCOUNTED_TYPE(type)) {
    return;
  }

  ASSERT(!i.isNative());
  ASSERT(!i.isSimple() || !canDtorStubReenter(type));
  SpaceRecorder sr("_DecRef", a);
  { // if !static
    IfCountNotStatic ins(a, rDatum, type);
    a.    sub_imm32_disp_reg32(1, TVOFF(_count), rDatum);

    ASSERT(type >= 0 && type < MaxNumDataTypes);
    if (&a == &this->astubs) {
      JccBlock<CC_NZ> ifZero(a);
      callUnaryStub(a, i, m_dtorStubs[type], rDatum);
    } else {
      UnlikelyIfBlock<CC_Z> ifZero(this->a, astubs);
      callUnaryStub(astubs, i, m_dtorStubs[type], rDatum);
    }
  } // endif
}

void TranslatorX64::emitDecRef(const NormalizedInstruction& i,
                               PhysReg rDatum,
                               DataType type) {
  emitDecRef(a, i, rDatum, type);
}

void TranslatorX64::emitDecRefInput(Asm& a,
                                    const NormalizedInstruction& i,
                                    int input) {
  DynLocation* value = i.inputs[input];
  if (IS_REFCOUNTED_TYPE(value->outerType())) {
    m_regMap.allocInputReg(i, input);
    PhysReg rValue = getReg(value->location);
    emitDecRef(a, i, rValue, value->outerType());
  }
}

/**
 * emitDecRefGeneric --
 *
 *   Decrement a value's refcount and call the release helper if
 *   appropriate. emitDecRefGeneric should only be used when the type
 *   is not known at translation time.
 *
 *   emitDecRefGeneric operates on the memory location given by
 *   srcReg+disp, so the caller is responsible for ensuring that the
 *   memory location is up to date and not enregistered.
 */
void TranslatorX64::emitDecRefGeneric(const NormalizedInstruction& i,
                                      PhysReg srcReg, int disp /* = 0 */) {
  SpaceRecorder sr("_DecRefGeneric", a);
  /*
   * The full, inlined generic dec ref looks like:
   *
   *    TypedValue* d = srcReg + disp;
   *    if (IS_REFCOUNTED_TYPE(d->m_type) && // a)
   *        d->_count != kStaticCount     && // b)
   *        d->_count-- == 0)             && // c)
   *            GenericDestroy(d);           // d)
   *
   * We originally inlined *all* of a-d, and have experimented with sharing
   * them all, too. At this writing (05-12-2012), inlining a) and outlining
   * b-d seems to strike the right balance between compactness and not
   * doing too much work in the common case where it is not refcounted.
   */
  {
    IfRefCounted irc(a, srcReg, disp);
    callUnaryReentrantStub(a, i, m_dtorGenericStub, srcReg, disp);
  }
}

/**
 * genericRefCountStub --
 *
 *   Shared code to decRef the TypedValue* of unknown, but refcounted, type
 *   in rdi. Tightly coupled with emitDecRefGeneric.
 */
TCA TranslatorX64::genericRefCountStub(X64Assembler& a) {
  moveToAlign(a);
  FreezeRegs brr(m_regMap);
  TCA retval = a.code.frontier;
  a.    pushr(rbp); // {
  a.    mov_reg64_reg64(rsp, rbp);
  {
    PhysRegSaverStub prs(a, RegSet(rsi));
    // We already know the type was refcounted if we got here.
    a.    load_reg64_disp_reg64(rdi, TVOFF(m_data), rsi);
    { // if !static
      IfCountNotStatic ins(a, rsi, KindOfInvalid);
      a.  sub_imm32_disp_reg32(1, TVOFF(_count), rsi);
      { // if zero
        JccBlock<CC_NZ> ifZero(a);
        RegSet s = kCallerSaved - (RegSet(rdi) | RegSet(rsi));
        PhysRegSaver prs(a, s);
        a.call(TCA(tv_release_generic));
      } // endif
    } // endif
  }
  a.    popr(rbp); // }
  a.    ret();
  return retval;
}

/*
 * Translation call targets. It is a lot easier, and a bit more
 * portable, to use C linkage from assembly.
 */
TCA TranslatorX64::retranslate(SrcKey sk, bool align) {
  if (isDebuggerAttachedProcess() && isSrcKeyInBL(curUnit(), sk)) {
    // We are about to translate something known to be blacklisted by
    // debugger, exit early
    SKTRACE(1, sk, "retranslate abort due to debugger\n");
    return NULL;
  }
  LeaseHolder writer(s_writeLease);
  if (!writer) return NULL;
  SKTRACE(1, sk, "retranslate\n");
  return translate(&sk, align);
}

/*
 * Satisfy an alignment constraint. If we're in a reachable section
 * of code, bridge the gap with nops. Otherwise, int3's.
 */
void
TranslatorX64::moveToAlign(X64Assembler &aa,
                           const size_t align /* =kJmpTargetAlign */,
                           bool unreachable /* =true */) {
  using namespace HPHP::Util;
  SpaceRecorder sr("_Align", aa);
  ASSERT(isPowerOfTwo(align));
  size_t leftInBlock = align - ((align - 1) & uintptr_t(aa.code.frontier));
  if (leftInBlock == align) return;
  if (unreachable) {
    aa.emitInt3s(leftInBlock);
    return;
  }
  aa.emitNop(leftInBlock);
}

/*
 * Req machinery. We sometimes emit code that is unable to proceed
 * without translator assistance; e.g., a basic block whose successor is
 * unknown. We leave one of these request arg blobs in m_data, and point
 * to it at callout-time.
 */

// REQ_BIND_CALL
struct ReqBindCall {
  SrcKey m_sourceInstr;
  TCA m_toSmash;
  int m_nArgs;
  bool m_isImmutable; // call was to known func.
} m_bindCall;

// ID to name mapping for tracing.
static inline const char*
reqName(int req) {
  static const char* reqNames[] = {
#define REQ(nm) #nm,
    SERVICE_REQUESTS
#undef REQ
  };
  return reqNames[req];
}

/*
 * Find or create a translation for sk. Returns TCA of "best" current
 * translation. May return NULL if it is currently impossible to create
 * a translation.
 */
TCA
TranslatorX64::getTranslation(const SrcKey *sk, bool align) {
  curFunc()->validate();
  SKTRACE(2, *sk, "getTranslation: curUnit %s funcId %llx offset %d\n",
          curUnit()->filepath()->data(),
          sk->m_funcId,
          sk->offset());
  SKTRACE(2, *sk, "   funcId: %llx\n",
          curFunc()->getFuncId());
  {
    if (curFrame()->hasVarEnv() && curFrame()->getVarEnv()->isGlobalScope()) {
      SKTRACE(2, *sk, "punting on pseudoMain\n");
      return NULL;
    }
    if (const SrcRec* sr = m_srcDB.find(*sk)) {
      TCA tca = sr->getTopTranslation();
      if (tca) {
        SKTRACE(2, *sk, "getTranslation: found %p\n", tca);
        return tca;
      }
    }
  }

  /*
   * Try to become the writer. We delay this until we *know* we will have
   * a need to create new translations, instead of just trying to win the
   * lottery at the dawn of time. Hopefully lots of requests won't require
   * any new translation.
   */
  LeaseHolder writer(s_writeLease);
  if (!writer) return NULL;
  if (SrcRec* sr = m_srcDB.find(*sk)) {
    TCA tca = sr->getTopTranslation();
    if (tca) {
      // Handle extremely unlikely race; someone may have just already
      // added the first instance of this SrcRec while we did a
      // non-blocking wait on the write lease.
      return tca;
    } else {
      // Since we are holding the write lease, we know that sk is properly
      // initialized, except that it has no translations (due to
      // replaceOldTranslations)
      return retranslate(*sk, align);
    }
  }

  // We put retranslate requests at the end of our slab to more frequently
  //   allow conditional jump fall-throughs

  TCA start = emitServiceReq(false, REQ_RETRANSLATE, 1, uint64_t(sk->offset()));
  SKTRACE(1, *sk, "inserting anchor translation for (%p,%d) at %p\n",
          curUnit(), sk->offset(), start);
  SrcRec* sr = m_srcDB.insert(*sk);
  sr->setFuncInfo(curFunc());
  sr->setAnchorTranslation(start);

  addTranslation(TransRec(*sk, curUnit()->md5(), TransAnchor, 0, 0, start,
                          astubs.code.frontier - start));

  ASSERT(getTransRec(start)->kind == TransAnchor);

  return retranslate(*sk, align);
}

TCA
TranslatorX64::translate(const SrcKey *sk, bool align) {
  INC_TPC(translate);
  ASSERT(vmfp() >= vmsp());
  ASSERT(((uintptr_t)vmsp() & (sizeof(Cell) - 1)) == 0);
  ASSERT(((uintptr_t)vmfp() & (sizeof(Cell) - 1)) == 0);
  Tracelet tlet;
  analyze(sk, tlet);

  if (align) {
    moveToAlign(a);
  }

  TCA start = a.code.frontier;
  translateTracelet(tlet);
  SKTRACE(1, *sk, "translate moved head from %p to %p\n",
          getTopTranslation(*sk), start);
  if (Trace::moduleEnabledRelease(tcdump, 1)) {
    static __thread int n;
    if (++n % 10000 == 0) {
      std::ofstream f("cfg.dot", std::ios_base::trunc);
      drawCFG(f);
      f.close();
    }
  }
  return start;
}

/*
 * Returns true if a's current frontier can have an nBytes-long
 * instruction written without any risk of cache-tearing.
 */
bool
TranslatorX64::isSmashable(X64Assembler& a, int nBytes) {
  ASSERT(nBytes <= int(kX64CacheLineSize));
  static const uint64 kCacheMask = ~(uint64(kX64CacheLineSize) - 1);
  uintptr_t iFrontier = uintptr_t(a.code.frontier);
  uintptr_t lastByte = iFrontier + nBytes - 1;
  return (iFrontier & kCacheMask) == (lastByte & kCacheMask);
}

void
TranslatorX64::prepareForSmash(X64Assembler& a, int nBytes) {
  if (UNLIKELY(!isSmashable(a, nBytes))) {
    moveToAlign(a, kX64CacheLineSize, false);
  }
  ASSERT(isSmashable(a, nBytes));
}

void
TranslatorX64::prepareForSmash(int nBytes) {
  prepareForSmash(a, nBytes);
}

void
TranslatorX64::smash(X64Assembler &a, TCA src, TCA dest) {
  ASSERT(canWrite());
  TRACE(2, "smash: %p -> %p\n", src, dest);
  /*
   * !
   *
   * We are about to smash reachable code in the translation cache. A
   * hardware thread might be executing the very instruction we're
   * modifying. This is safe because:
   *
   *    1. We align smashable instructions so that they reside on a single
   *       cache line;
   *
   *    2. We modify the instruction with a single processor store; and
   *
   *    3. The smashed region contains only a single instruction in the
   *       orignal instruction stream (see jmp() -> emitJ32() -> bytes() in
   *       the assembler.
   */
  CodeCursor cg(a, src);
  ASSERT(isSmashable(a, kJmpLen));
  if (dest > src && dest - src <= 7) {
    a.    emitNop(dest - src);
  } else {
    a.    jmp(dest);
  }
}

void TranslatorX64::protectCode() {
  mprotect(tx64->a.code.base, tx64->a.code.size, PROT_READ | PROT_EXEC);

}

void TranslatorX64::unprotectCode() {
  mprotect(tx64->a.code.base, tx64->a.code.size,
           PROT_READ | PROT_WRITE | PROT_EXEC);
}

void
TranslatorX64::emitStackCheck(int funcDepth, Offset pc) {
  uint64_t stackMask = cellsToBytes(RuntimeOption::EvalVMStackElms) - 1;
  a.    mov_reg64_reg64(rVmSp, rScratch); // copy to destroy
  a.    and_imm64_reg64(stackMask, rScratch);
  a.    sub_imm64_reg64(funcDepth + Stack::sSurprisePageSize, rScratch);
  ASSERT(m_stackOverflowHelper);
  a.    jl(m_stackOverflowHelper); // Unlikely branch to failure.
  // Success.
}

// Tests the surprise flags for the current thread. Should be used
// before a jnz to surprise handling code.
void
TranslatorX64::emitTestSurpriseFlags() {
  CT_ASSERT(sizeof(((RequestInjectionData*)0)->conditionFlags) == 8);
  a.test_imm64_disp_reg64(-1, TargetCache::kConditionFlagsOff, rVmTl);
}

void
TranslatorX64::emitCheckSurpriseFlagsEnter(bool inTracelet, Offset pcOff,
                                           Offset stackOff) {
  emitTestSurpriseFlags();
  {
    UnlikelyIfBlock<CC_NZ> ifTracer(a, astubs);
    if (false) { // typecheck
      const ActRec* ar = NULL;
      EventHook::FunctionEnter(ar, 0);
    }
    astubs.mov_reg64_reg64(rVmFp, argNumToRegName[0]);
    CT_ASSERT(EventHook::NormalFunc == 0);
    astubs.xor_reg32_reg32(argNumToRegName[1], argNumToRegName[1]);
    emitCall(astubs, (TCA)&EventHook::FunctionEnter);
    if (inTracelet) {
      recordSyncPoint(astubs, pcOff, stackOff);
    } else {
      // If we're being called while generating a func prologue, we
      // have to record the fixup directly in the fixup map instead of
      // going through m_pendingFixups like normal.
      m_fixupMap.recordFixup(astubs.code.frontier, Fixup(pcOff, stackOff));
    }
  }
}

void
TranslatorX64::setArgInActRec(ActRec* ar, int argNum, uint64_t datum,
                              DataType t) {
  TypedValue* tv =
    (TypedValue*)(uintptr_t(ar) - (argNum+1) * sizeof(TypedValue));
  tv->m_data.num = datum;
  tv->_count = 0;
  tv->m_type = t;
}

int
TranslatorX64::shuffleArgsForMagicCall(ActRec* ar) {
  if (!ar->hasInvName()) {
    return 0;
  }
  const Func* f UNUSED = ar->m_func;
  f->validate();
  ASSERT(f->name()->isame(s___call.get())
         || f->name()->isame(s___callStatic.get()));
  ASSERT(f->numParams() == 2);
  TRACE(1, "shuffleArgsForMagicCall: ar %p\n", ar);
  ASSERT(ar->hasInvName());
  StringData* invName = ar->getInvName();
  ASSERT(invName);
  ar->setVarEnv(NULL);
  int nargs = ar->numArgs();
  // We need to make an array containing all the arguments passed by the
  // caller and put it where the second argument is
  HphpArray* argArray = NEW(HphpArray)(nargs);
  argArray->incRefCount();
  for (int i = 0; i < nargs; ++i) {
    TypedValue* tv =
      (TypedValue*)(uintptr_t(ar) - (i+1) * sizeof(TypedValue));
    argArray->nvAppend(tv, false);
    tvRefcountedDecRef(tv);
  }
  // Put invName in the slot for first argument
  setArgInActRec(ar, 0, uint64_t(invName), BitwiseKindOfString);
  // Put argArray in the slot for second argument
  setArgInActRec(ar, 1, uint64_t(argArray), KindOfArray);
  // Fix up ActRec's numArgs
  ar->initNumArgs(2);
  return 1;
}

/*
 * The standard VMRegAnchor treatment won't work for some cases called
 * during function preludes.
 *
 * The fp sync machinery is fundamentally based on the notion that
 * instruction pointers in the TC are uniquely associated with source
 * HHBC instructions, and that source HHBC instructions are in turn
 * uniquely associated with SP->FP deltas.
 *
 * run_intercept_helper/trimExtraArgs is called from the prologue of
 * the callee. The prologue is 1) still in the caller frame for now,
 * and 2) shared across multiple call sites. 1 means that we have the
 * fp from the caller's frame, and 2 means that this fp is not enough
 * to figure out sp.
 *
 * However, the prologue passes us the callee actRec, whose predecessor
 * has to be the caller. So we can sync sp and fp by ourselves here.
 * Geronimo!
 */
static void sync_regstate_to_caller(ActRec* preLive) {
  ASSERT(tl_regState == REGSTATE_DIRTY);
  vmfp() = (TypedValue*)preLive->m_savedRbp;
  vmsp() = (TypedValue*)preLive - preLive->numArgs();
  if (ActRec* fp = g_vmContext->m_fp) {
    if (fp->m_func && fp->m_func->unit()) {
      vmpc() = fp->m_func->unit()->at(fp->m_func->base() + preLive->m_soff);
    }
  }
  tl_regState = REGSTATE_CLEAN;
}

static uint64 run_intercept_helper(ActRec* ar, Variant* ihandler) {
  sync_regstate_to_caller(ar);
  bool ret = run_intercept_handler<true>(ar, ihandler);
  /*
   * Restore tl_regState manually in the no-exception case only.  (The
   * VM regs are clean here---we only need to set them dirty if we are
   * stopping to execute in the TC again, which we won't be doing if
   * an exception is propagating.)
   */
  tl_regState = REGSTATE_DIRTY;
  return ret;
}

void
TranslatorX64::trimExtraArgs(ActRec* ar) {
  ASSERT(!ar->hasInvName());

  sync_regstate_to_caller(ar);
  const Func* f = ar->m_func;
  int numParams = f->numParams();
  int numArgs = ar->numArgs();
  ASSERT(numArgs > numParams);
  int numExtra = numArgs - numParams;

  TRACE(1, "trimExtraArgs: %d args, function %s takes only %d, ar %p\n",
        numArgs, f->name()->data(), numParams, ar);

  if (f->attrs() & AttrMayUseVV) {
    if (!ar->hasExtraArgs()) {
      ar->setExtraArgs(new ExtraArgs());
    }
    // Stash the excess args in the VarEnv attached to the ActRec. They'll be
    // decref'ed, if needed, when the VarEnv gets destructed.
    ar->getExtraArgs()->copyExtraArgs(
      (TypedValue*)(uintptr_t(ar) - numArgs * sizeof(TypedValue)),
      numArgs - numParams);
  } else {
    // Function is not marked as "MayUseVV", so discard the extra arguments
    TypedValue* tv = (TypedValue*)(uintptr_t(ar) - numArgs*sizeof(TypedValue));
    for (int i = 0; i < numExtra; ++i) {
      tvRefcountedDecRef(tv);
      ++tv;
    }
    ar->setNumArgs(numParams);
  }

  // Only go back to dirty in a non-exception case.  (Same reason as
  // above.)
  tl_regState = REGSTATE_DIRTY;
}

TCA
TranslatorX64::getInterceptHelper() {
  if (false) {  // typecheck
    Variant *h = get_intercept_handler(CStrRef((StringData*)NULL),
                                       (char*)NULL);
    bool c UNUSED = run_intercept_helper((ActRec*)NULL, h);
  }
  if (!m_interceptHelper) {
    m_interceptHelper = TCA(astubs.code.frontier);
    astubs.    load_reg64_disp_reg64(rStashedAR, offsetof(ActRec, m_func),
                                     rax);
    astubs.    lea_reg64_disp_reg64(rax, Func::fullNameOff(),
                                    argNumToRegName[0]);

    astubs.    lea_reg64_disp_reg64(rax, Func::maybeInterceptedOff(),
                                    argNumToRegName[1]);

    astubs.    call(TCA(get_intercept_handler));
    astubs.    test_reg64_reg64(rax, rax);
    {
      JccBlock<CC_NZ> ifNotIntercepted(astubs);
      astubs.  ret();
    }

    // we might re-enter, so align the stack
    astubs.    sub_imm32_reg64(8, rsp);
    // Copy the old rbp into the savedRbp pointer.
    astubs.    store_reg64_disp_reg64(rbp, 0, rStashedAR);

    PhysReg rSavedRip = r13;
    // Fish out the saved rip. We may need to jump there, and the helper will
    // have wiped out the ActRec.
    astubs.    load_reg64_disp_reg64(rStashedAR, AROFF(m_savedRip),
                                     rSavedRip);
    astubs.    mov_reg64_reg64(rStashedAR, argNumToRegName[0]);
    astubs.    mov_reg64_reg64(rax, argNumToRegName[1]);
    astubs.    call(TCA(run_intercept_helper));

    // Normally we'd like to recordReentrantCall here, but the vmreg sync'ing
    // for run_intercept_handler is a special little snowflake. See
    // run_intercept_handler for details.
    astubs.    test_reg64_reg64(rax, rax);
    {
      // If the helper returned false, don't execute this function. The helper
      // will have cleaned up the interceptee's arguments and AR, and pushed
      // the handler's return value; we now need to get out.
      //
      // We don't need to touch rVmFp; it's still pointing to the caller of
      // the interceptee. We need to adjust rVmSp. Then we need to jump to the
      // saved rip from the interceptee's ActRec.
      JccBlock<CC_NZ> ifDontEnterFunction(astubs);
      astubs.  add_imm32_reg64(16, rsp);
      astubs.  lea_reg64_disp_reg64(rStashedAR, AROFF(m_r), rVmSp);
      astubs.  jmp_reg(rSavedRip);
    }
    astubs.    add_imm32_reg64(8, rsp);
    astubs.    ret();
  }
  return m_interceptHelper;
}

TCA
TranslatorX64::emitPrologueRedispatch(X64Assembler& a) {
  TCA retval;
  moveToAlign(a);
  retval = a.code.frontier;
  // We're in the wrong func prologue. By convention with emitFuncGuard,
  // rax contains the function we need to enter.

  // We don't know how many params we were invoked with. Infer it from
  // the stack and rStashedAR rather than reading it from the actrec.
  //
  //    mov %r15, %rdx
  //    ld  m_numParams(%rax), %ecx #ecx: targetFunc->numParams
  //    sub %rbx, %rdx #edx: n_args
  //    shr $4, rdx
  a.    mov_reg64_reg64(rStashedAR, rdx);
  a.    load_reg64_disp_reg32(rax, Func::numParamsOff(), rcx);
  a.    sub_reg64_reg64(rVmSp, rdx);
  BOOST_STATIC_ASSERT(sizeof(TypedValue) == 16);
  a.    shr_imm32_reg32(4, rdx); // rdx: numPassed

  // If we didn't pass too many args, directly dereference
  // func->m_prologues.
  a.    cmp_reg32_reg32(rdx, rcx);
  TCA bToFixedProloguesCheck = a.code.frontier;
  a.    jcc8(CC_L, bToFixedProloguesCheck);

  //   cmp $kNumFixedPrologues, %rdx
  //   jl numParamsCheck
  TCA actualDispatch = a.code.frontier;

  // rcx: prologueIdx
  // rax = func->prologues[numParams]
  // jmp rax
  a.    load_reg64_disp_index_reg64(rax,
                                    Func::prologueTableOff(),
                                    rdx,
                                    rax);
  a.    jmp_reg(rax);
  a.    ud2();

  // Hmm, more parameters passed than the function expected. Did we pass
  // kNumFixedPrologues or more? If not, %rdx is still a perfectly
  // legitimate index into the func prologue table.
  // numParamsCheck:
  //    cmp $kNumFixedPrologues, %rcx
  //    jl  dispatch
  a.patchJcc8(bToFixedProloguesCheck, a.code.frontier); // numParamsCheck:
  a.    cmp_imm32_reg32(kNumFixedPrologues, rdx);
  a.    jcc8(CC_L, actualDispatch);

  // Too many gosh-darned parameters passed. Go to numExpected + 1, which
  // is always a "too many params" entry point.
  //
  //    mov %rdx, %rcx
  //    add $1, %rcx
  //    jmp dispatch
  a.    load_reg64_disp_index_reg64(rax,
                                    // %rcx + 1
                                    Func::prologueTableOff() + sizeof(TCA),
                                    rcx,
                                    rax);
  a.    jmp_reg(rax);
  a.    ud2();
  return retval;
}

// The funcGuard gets skipped and patched by other code, so we have some
// magic offsets.
static const int kFuncMovImm = 6; // Offset to the immediate for expected func
static const int kFuncGuardLen = 23;

template<typename T>
static T*
funcGuardToFuncImm(TCA funcGuard) {
  T* retval = (T*)(funcGuard + kFuncMovImm + (2 - sizeof(T)/4));
  // We padded these so the immediate would fit inside an aligned 8 byte region
  // so the xor of the address of the first byte, with the address of the last
  // byte should only be non zero in the bottom 3 bits.
  ASSERT(((uintptr_t(retval) ^ (uintptr_t(retval + 1) - 1)) & ~7) == 0);
  return retval;
}

static inline bool
funcGuardIsForFunc(TCA funcGuard, const Func* func) {
  intptr_t iptr = uintptr_t(func);
  if (x64::deltaFits(iptr, sz::dword)) {
    return *funcGuardToFuncImm<int32_t>(funcGuard) == iptr;
  }
  return *funcGuardToFuncImm<int64_t>(funcGuard) == iptr;
}

static void
disableFuncGuard(TCA funcGuard, Func* func) {
  ASSERT(funcGuardIsForFunc(funcGuard, func));
  if (x64::deltaFits((intptr_t)func, sz::dword)) {
    *funcGuardToFuncImm<int32_t>(funcGuard) = 0;
  } else {
    *funcGuardToFuncImm<int64_t>(funcGuard) = 0;
  }
  ASSERT(!funcGuardIsForFunc(funcGuard, func));
}

TCA
TranslatorX64::emitFuncGuard(X64Assembler& a, const Func* func) {
  // Ensure the immediate is safely smashable; the immediate needs
  // to be at a qword boundary, so we need to start the movImm at
  // (kAlign - kFuncMovImm) % 8.
  static const int kAlign = 8;
  static const int kAlignMask = kAlign - 1;
  int loBits = uintptr_t(a.code.frontier) & kAlignMask;
  a.emitNop(((kAlign - kFuncMovImm) - loBits) & kAlignMask);
  ASSERT((uintptr_t(a.code.frontier) & kAlignMask) == kAlign - kFuncMovImm);
  TCA aStart = a.code.frontier;
  a.    load_reg64_disp_reg64(rStashedAR, AROFF(m_func), rax);
  ASSERT((a.code.frontier - aStart) ==
         (kFuncMovImm - 2 /* rex + movimmOpcode */));
  a.    mov_imm64_reg(uint64_t(func), rdx);
  a.    cmp_reg64_reg64(rax, rdx);

  if (!m_funcPrologueRedispatch) {
    m_funcPrologueRedispatch = emitPrologueRedispatch(astubs);
  }
  a.    jnz(m_funcPrologueRedispatch);
  ASSERT(a.code.frontier - aStart <= kFuncGuardLen);
  a.emitNop(kFuncGuardLen - (a.code.frontier - aStart));
  ASSERT(a.code.frontier - aStart == kFuncGuardLen);
  return aStart;
}

TCA
skipFuncCheck(TCA dest) {
  if (!dest || dest == (TCA)fcallHelperThunk) return dest;
  return dest + kFuncGuardLen;
}

/*
 * funcPrologue --
 *
 * Given a callee and a number of args, match up to the callee's
 * argument expectations and dispatch.
 *
 * Call/return hand-shaking is a bit funny initially. At translation time,
 * we don't necessarily know what function we're calling. For instance,
 *
 *   f(g());
 *
 * Will lead to a set of basic blocks like:
 *
 * b1: pushfuncd "f"
 *     pushfuncd "g"
 *     fcall
 * b2: fcall
 *
 * The fcallc labelled "b2" above is not statically bindable in our
 * execution model.
 *
 * We decouple the call work into a per-callsite portion, responsible
 * for recording the return address, and a per-(callee, numArgs) portion,
 * responsible for fixing up arguments and dispatching to remaining
 * code. We call the per-callee portion a "prologue."
 *
 * Also, we are called from two distinct environments. From REQ_BIND_CALL,
 * we're running "between" basic blocks, with all VM registers sync'ed.
 * However, we're also called in the middle of basic blocks, when dropping
 * entries into func->m_prologues. So don't go around using the
 * translation-time values of vmfp()/vmsp(), since they have an
 * unpredictable relationship to the source.
 */
bool
TranslatorX64::checkCachedPrologue(const Func* func, int paramIdx,
                                   TCA& prologue) const {
  prologue = (TCA)func->getPrologue(paramIdx);
  if (prologue != (TCA)fcallHelperThunk && !s_replaceInFlight) {
    TRACE(1, "cached prologue %s(%d) -> cached %p\n",
          func->fullName()->data(), paramIdx, prologue);
    ASSERT(isValidCodeAddress(prologue));
    return true;
  }
  return false;
}

TCA
TranslatorX64::funcPrologue(Func* func, int nPassed) {
  func->validate();
  TRACE(1, "funcPrologue %s(%d)\n", func->fullName()->data(), nPassed);
  int numParams = func->numParams();
  int paramIndex = nPassed <= numParams ? nPassed : numParams + 1;

  bool funcIsMagic = func->isMagic();

  // Do a quick test before grabbing the write lease
  TCA prologue;
  if (checkCachedPrologue(func, paramIndex, prologue)) return prologue;

  // If the translator is getting replaced out from under us, refuse to
  // provide a prologue; we don't know whether this request is running on the
  // old or new context.
  LeaseHolder writer(s_writeLease);
  if (!writer || s_replaceInFlight) return NULL;
  // Double check the prologue array now that we have the write lease
  // in case another thread snuck in and set the prologue already.
  if (checkCachedPrologue(func, paramIndex, prologue)) return prologue;

  SpaceRecorder sr("_FuncPrologue", a);
  // Careful: this isn't necessarily the real entry point. For funcIsMagic
  // prologues, this is just a possible prologue.
  TCA aStart    = a.code.frontier;
  TCA start     = aStart;
  TCA stubStart = astubs.code.frontier;

  // Guard: we're in the right callee. This happens in magicStart for
  // magic callees.
  if (!funcIsMagic) {
    start = aStart = emitFuncGuard(a, func);
  }

  emitRB(a, RBTypeFuncPrologueTry, func->fullName()->data());
  // Guard: we have stack enough stack space to complete this function.
  emitStackCheck(cellsToBytes(func->maxStackCells()), func->base());

  // NB: We have the whole register file to play with, since we know we're
  // between BB's. So, we hardcode some registers here rather than using
  // the scratch allocator.
  TRACE(2, "funcPrologue: user function: %s\n", func->name()->data());

  // Add a counter for the translation if requested
  if (RuntimeOption::EvalJitTransCounters) {
    emitTransCounterInc(a);
  }

  if (!funcIsMagic) {
    // entry point for magic methods comes later
    emitRB(a, RBTypeFuncEntry, func->fullName()->data());
  }

  SrcKey skFuncBody = emitPrologue(func, nPassed);

  if (funcIsMagic) {
    // entry points for magic methods is here
    TCA magicStart = emitFuncGuard(a, func);
    ASSERT(numParams == 2);
    emitRB(a, RBTypeFuncEntry, func->fullName()->data());
    // Special __call prologue
    a.  mov_reg64_reg64(rStashedAR, argNumToRegName[0]);
    emitCall(a, TCA(TranslatorX64::shuffleArgsForMagicCall));
    // if shuffleArgs returns 0, that means this was not a magic call
    // and we should proceed to a prologue specialized for nPassed;
    // otherwise, proceed to a prologue specialized for nPassed==numParams (2).
    if (nPassed == 2) {
      a.jmp(start);
    } else {
      a.test_reg64_reg64(rax, rax);
      // z ==> not a magic call, go to prologue for nPassed
      if (deltaFits(start - (a.code.frontier + kJcc8Len), sz::byte)) {
        a.jcc8(CC_Z, start);
      } else {
        a.jcc(CC_Z, start);
      }
      // this was a magic call
      // nPassed == 2
      // Fix up hardware stack pointer
      nPassed = 2;
      a.  lea_reg64_disp_reg64(rStashedAR, -cellsToBytes(nPassed), rVmSp);
      // Optimization TODO: Reuse the prologue for args == 2
      emitPrologue(func, nPassed);
    }
    start = magicStart;
  }
  ASSERT(funcGuardIsForFunc(start, func));
  TRACE(2, "funcPrologue tx64 %p %s(%d) setting prologue %p\n",
        this, func->fullName()->data(), nPassed, start);
  ASSERT(isValidCodeAddress(start));
  func->setPrologue(paramIndex, start);

  addTranslation(TransRec(skFuncBody, func->unit()->md5(),
                          TransProlog, aStart, a.code.frontier - aStart,
                          stubStart, astubs.code.frontier - stubStart));

  recordGdbTranslation(skFuncBody, func->unit(),
                       a, aStart,
                       false, true);
  recordBCInstr(OpFuncPrologue, a, start);

  return start;
}

static TCA callAndResume(ActRec *ar) {
  VMRegAnchor _(ar,true);
  g_vmContext->doFCall<true>(ar, g_vmContext->m_pc);
  return Translator::Get()->getResumeHelper();
}

extern "C"
TCA fcallHelper(ActRec* ar) {
  TCA tca =
    Translator::Get()->funcPrologue((Func*)ar->m_func, ar->numArgs());
  if (tca) {
    return tca;
  }
  return callAndResume(ar);
}

TCA
TranslatorX64::emitInterceptPrologue(Func* func, TCA next) {
  TCA start = a.code.frontier;
  a.mov_imm64_reg((uintptr_t)&func->maybeIntercepted(), rax);
  a.cmp_imm8_disp_reg8(0, 0, rax);
  TCA jcc8PatchAddr = NULL;
  if (next == NULL) {
    jcc8PatchAddr = a.code.frontier;
    a.jcc8(CC_E, jcc8PatchAddr);
  } else {
    a.jcc(CC_E, next);
  }
  // Prologues are not really sites for function entry yet; we can get
  // here via an optimistic bindCall. Check that the func is as expected.

  a.    mov_imm64_reg(uint64_t(func), rax);
  a.    cmp_reg64_disp_reg64(rax, AROFF(m_func), rStashedAR);
  {
    JccBlock<CC_NZ> skip(a);
    a.call(getInterceptHelper());
  }
  if (jcc8PatchAddr != NULL) {
    a.patchJcc8(jcc8PatchAddr, a.code.frontier);
  }
  return start;
}

void
TranslatorX64::interceptPrologues(Func* func) {
  if (!RuntimeOption::EvalJitEnableRenameFunction &&
      !(func->attrs() & AttrDynamicInvoke)) {
    return;
  }
  if (func->maybeIntercepted() == -1) {
    return;
  }
  func->maybeIntercepted() = -1;
  ASSERT(s_writeLease.amOwner());
  int maxNumPrologues = func->numPrologues();
  for (int i = 0; i < maxNumPrologues; i++) {
    TCA prologue = func->getPrologue(i);
    if (prologue == (unsigned char*)fcallHelperThunk)
      continue;
    ASSERT(funcGuardIsForFunc(prologue, func));
    // There might already be calls hard-coded to this via FCall.
    // blow away immediate comparison, so that we always use the Func*'s
    // prologue table. We use 0 (== NULL on our architecture) as the bit
    // pattern for an impossible Func.
    //
    // Note that we're modifying reachable code.
    disableFuncGuard(prologue, func);
    ASSERT(funcGuardIsForFunc(prologue, NULL));

    // There's a prologue already generated; redirect it to first
    // call the intercept helper. First, reset it (leaking the old
    // prologue), so funcPrologue will re-emit it.
    func->setPrologue(i, (TCA)fcallHelperThunk);
    TCA addr = funcPrologue(func, i);
    ASSERT(funcGuardIsForFunc(addr, func));
    ASSERT(addr);
    func->setPrologue(i, addr);
    TRACE(1, "interceptPrologues %s prologue[%d]=%p\n",
          func->fullName()->data(), i, (void*)addr);
  }
}

SrcKey
TranslatorX64::emitPrologue(Func* func, int nPassed) {
  int numParams = func->numParams();
  ASSERT(IMPLIES(func->maybeIntercepted() == -1,
                 m_interceptsEnabled));
  if (m_interceptsEnabled &&
      !func->isPseudoMain() &&
      (RuntimeOption::EvalJitEnableRenameFunction ||
       func->attrs() & AttrDynamicInvoke)) {
    emitInterceptPrologue(func);
  }

  Offset dvInitializer = InvalidAbsoluteOffset;

  if (nPassed > numParams) {
    // Too many args; a weird case, so just callout. Stash ar
    // somewhere callee-saved.
    if (false) { // typecheck
      TranslatorX64::trimExtraArgs((ActRec*)NULL);
    }
    a.  mov_reg64_reg64(rStashedAR, argNumToRegName[0]);
    emitCall(a, TCA(TranslatorX64::trimExtraArgs));
    // We'll fix rVmSp below.
  } else if (nPassed < numParams) {
    // Figure out which, if any, default value initializer to go to
    for (int i = nPassed; i < numParams; ++i) {
      const Func::ParamInfo& pi = func->params()[i];
      if (pi.hasDefaultValue()) {
        dvInitializer = pi.funcletOff();
        break;
      }
    }
    TRACE(1, "Only have %d of %d args; getting dvFunclet\n",
          nPassed, numParams);
    emitImmReg(a, nPassed, rax);
    // do { *(--rVmSp) = NULL; nPassed++; } while (nPassed < numParams);
    // This should be an unusual case, so optimize for code density
    // rather than execution speed; i.e., don't unroll the loop.
    TCA loopTop = a.code.frontier;
    a.  sub_imm32_reg64(sizeof(Cell), rVmSp);
    a.  add_imm32_reg32(1, rax);
    // XXX "missing argument" warnings need to go here
    emitStoreUninitNull(a, 0, rVmSp);
    a.  cmp_imm32_reg32(numParams, rax);
    a.  jcc8(CC_L, loopTop);
  }

  // Entry point for numParams == nPassed is here.
  // Args are kosher. Frame linkage: set fp = ar.
  a.    mov_reg64_reg64(rStashedAR, rVmFp);

  // We're in the callee frame; initialize locals. Unroll the loop all
  // the way if there are a modest number of locals to update;
  // otherwise, do it in a compact loop. If we're in a generator body,
  // named locals will be initialized by UnpackCont so we can leave
  // them alone here.
  int uninitLimit = func->isGenerator() ? func->numNamedLocals() : numParams;
  int numUninitLocals = func->numLocals() - uninitLimit;
  ASSERT(numUninitLocals >= 0);
  if (numUninitLocals > 0) {
    SpaceRecorder sr("_InitializeLocals", a);

    // If there are too many locals, then emitting a loop to initialize locals
    // is more compact, rather than emitting a slew of movs inline.
    if (numUninitLocals > kLocalsToInitializeInline) {
      PhysReg loopReg = rcx;

      // rVmFp + rcx points to the count/type fields of the TypedValue we're
      // about to write to.
      int loopStart = -func->numLocals() * sizeof(TypedValue)
        + TVOFF(_count);
      int loopEnd = -uninitLimit * sizeof(TypedValue)
        + TVOFF(_count);

      emitImmReg(a, loopStart, loopReg);
      emitImmReg(a, 0, rdx);

      TCA topOfLoop = a.code.frontier;
      // do {
      //   rVmFp[rcx].m_type = KindOfUninit;
      // } while(++rcx != loopEnd);

      //  mov %rdx, 0x0(%rVmFp, %rcx, 1)
      a.  emitRM(instr_mov, rVmFp, loopReg, 1, 0, rdx);
      a.  add_imm32_reg64(sizeof(Cell), loopReg);
      a.  cmp_imm32_reg64(loopEnd, loopReg);
      a.  jcc8(CC_NE, topOfLoop);
    } else {
      PhysReg base;
      int disp, k;
      for (k = uninitLimit; k < func->numLocals(); ++k) {
        locToRegDisp(Location(Location::Local, k), &base, &disp);
        emitStoreUninitNull(a, disp, base);
      }
    }
  }

  // Move rVmSp to the right place: just past all locals
  int frameCells = func->numSlotsInFrame();
  a.   lea_reg64_disp_reg64(rVmFp, -cellsToBytes(frameCells), rVmSp);
  const Opcode* destPC = func->unit()->entry() + func->base();
  if (dvInitializer != InvalidAbsoluteOffset) {
    // dispatch to funclet.
    destPC = func->unit()->entry() + dvInitializer;
  }
  SrcKey funcBody(func, destPC);

  // Check surprise flags in the same place as the interpreter: after
  // setting up the callee's frame but before executing any of its
  // code
  emitCheckSurpriseFlagsEnter(false, funcBody.m_offset - func->base(),
                              frameCells);

  emitBindJmp(funcBody);
  return funcBody;
}

static const PhysReg serviceReqArgRegs[] = {
  /* rdi: contains req */ rsi, rdx, rcx, r8, r9
};

void
TranslatorX64::emitBindCall(const Tracelet& t,
                            const NormalizedInstruction &ni,
                            Offset atCall, Offset afterCall) {
  int numArgs = ni.imm[0].u_IVA;

  // If this is a call to a builtin and we don't need any argument
  // munging, we can skip the prologue system and do it inline.
  if (ni.funcd && ni.funcd->isBuiltin() &&
      numArgs == ni.funcd->numParams()) {
    ASSERT(ni.funcd->numLocals() == ni.funcd->numParams());
    ASSERT(ni.funcd->numIterators() == 0);
    a.  lea_reg64_disp_reg64(rVmSp, cellsToBytes(numArgs), rVmFp);
    emitCheckSurpriseFlagsEnter(true, 0, numArgs);
    // rVmSp is already correctly adjusted, because there's no locals
    // other than the arguments passed.
    return emitNativeImpl(ni.funcd, false /* don't jump to return */);
  }

  // Stash callee's rVmFp into rStashedAR for the callee's prologue
  a.    lea_reg64_disp_reg64(rVmSp, cellsToBytes(numArgs), rStashedAR);
  // Whatever prologue we're branching to will check at runtime that we
  // went to the right Func*, correcting if necessary. We treat the first
  // Func we encounter as a decent prediction. Make space to burn in a
  // TCA.
  ReqBindCall* req = m_globalData.alloc<ReqBindCall>();
  a.    mov_reg64_reg64(rStashedAR, serviceReqArgRegs[1]);
  prepareForSmash(kJmpLen);
  TCA start = a.code.frontier;
  a.    jmp(emitServiceReq(false, REQ_BIND_CALL, 1ull, req));

  TRACE(1, "will bind static call: tca %p, this %p, funcd %p\n",
        start, this, ni.funcd);
  req->m_toSmash = start;
  req->m_nArgs = numArgs;
  req->m_sourceInstr = ni.source;
  req->m_isImmutable = (bool)ni.funcd;
}

// for documentation see bindJmpccFirst below
void
TranslatorX64::emitCondJmp(const SrcKey &skTaken, const SrcKey &skNotTaken,
                           ConditionCode cc) {
  // should be true for SrcKeys generated via OpJmpZ/OpJmpNZ
  ASSERT(skTaken.m_funcId == skNotTaken.m_funcId);

  // reserve space for a smashable jnz/jmp pair; both initially point
  // to our stub
  prepareForSmash(kJmpLen + kJmpccLen);
  TCA old = a.code.frontier;

  moveToAlign(astubs);
  TCA stub = astubs.code.frontier;

  // begin code for the stub

  // We need to be careful here, as we are passing an extra paramter to
  //   REQ_BIND_JMPCC_FIRST. However we can't pass this parameter via
  //   emitServiceReq because that only supports constants/immediates, so
  //   compute the last argument via setcc.
  astubs.setcc(cc, serviceReqArgRegs[4]);
  emitServiceReq(false /* align */, REQ_BIND_JMPCC_FIRST, 4ull,
                 old,
                 uint64_t(skTaken.offset()),
                 uint64_t(skNotTaken.offset()),
                 uint64_t(cc));

  a.jcc(cc, stub); // MUST use 4-byte immediate form
  a.jmp(stub); // MUST use 4-byte immediate form
}

static void skToName(const SrcKey& sk, char* name) {
  sprintf(name, "sk_%08lx_%05d",
          long(sk.m_funcId), sk.offset());
}

static void skToClusterName(const SrcKey& sk, char* name) {
  sprintf(name, "skCluster_%08lx_%05d",
          long(sk.m_funcId), sk.offset());
}

static void translToName(const TCA tca, char* name) {
  sprintf(name, "tc_%p", tca);
}

void TranslatorX64::drawCFG(std::ofstream& out) const {
  if (!isTransDBEnabled()) return;
  const char* indent = "    ";
  static int genCount;
  int numSrcKeys = 0;
  int numTranslations = 0;
  out << "digraph srcdb" << genCount++ <<" {\n";
  out << indent << "size = \"8,11\";\n";
  out << indent << "ratio = fill;\n";
  for (SrcDB::const_iterator entry = m_srcDB.begin();
       entry != m_srcDB.end(); ++entry) {
    const SrcKey sk = SrcKey::fromAtomicInt(entry->first);
    // 1 subgraph per srcKey.
    char name[64];
    skToClusterName(sk, name);
    numSrcKeys++;
    out << indent << "subgraph " << name << "{\n";
    char* indent = "        ";
    skToName(sk, name);
    out << indent << name << "[shape=box];\n";
    const vector<TCA>& transls = entry->second->translations();
    for (vector<TCA>::const_iterator t = transls.begin(); t != transls.end();
         ++t) {
      out << indent << "// Translations: " << transls.size() << "\n";
      char transname[64];
      translToName(*t, transname);
      numTranslations++;
      out << indent << transname << "[fontsize=11.0];\n";
      out << indent << name << " -> " << transname << ";\n";
    }
    // And, all translations on the same line
    out << indent << "{ rank = same; ";
    out << name << " ";
    for (vector<TCA>::const_iterator t = transls.begin(); t != transls.end();
         ++t) {
      char transname[64];
      translToName(*t, transname);
      out << transname << " ";
    }
    out << indent << "}\n"; // subgraph
    out << indent << "}\n";
  }

  // OK! Those were all the nodes. Now edges. While edges are physically
  // from translation to translation, they're virtually from srcKey to
  // srcKey, and that is how the db represents them.
  for (SrcDB::const_iterator entry = m_srcDB.begin(); entry != m_srcDB.end();
       ++entry) {
    char destName[64];
    skToName(SrcKey::fromAtomicInt(entry->first), destName);
    const vector<IncomingBranch>& ibs = entry->second->incomingBranches();
    out << indent << "// incoming branches to " << destName << "\n";
    for (vector<IncomingBranch>::const_iterator ib = ibs.begin();
         ib != ibs.end(); ++ib) {
      // Find the start of the translation that contains this branch
      const char *branchTypeToColorStr[] = {
        "black", // JMP
        "green", // JZ
        "red",   // JNZ
      };
      TransDB::const_iterator lowerTCA = m_transDB.lower_bound(ib->m_src);
      ASSERT(lowerTCA != m_transDB.end());
      char srcName[64];
      const TransRec* transRec = this->getTransRec(lowerTCA->second);
      ASSERT(transRec);
      skToName(transRec->src, srcName);
      out << indent << srcName << " -> " << destName << "[ color = " <<
        branchTypeToColorStr[ib->m_type] << "];\n";
    }
  }
  out << indent << "// " << numSrcKeys << " srckeys, " << numTranslations <<
    " tracelets\n";
  out << "}\n\n";
}

/*
 * bindJmp --
 *
 *   Runtime service handler that patches a jmp to the translation of
 *   u:dest from toSmash.
 */
TCA
TranslatorX64::bindJmp(TCA toSmash, SrcKey destSk, bool isAddr) {
  TCA tDest = getTranslation(&destSk, false);
  if (!tDest) return NULL;
  LeaseHolder writer(s_writeLease);
  if (!writer) return tDest;
  SrcRec* sr = getSrcRec(destSk);
  if (isAddr) {
    sr->chainFrom(a, IncomingBranch((TCA*)toSmash));
  } else {
    sr->chainFrom(getAsmFor(toSmash), IncomingBranch(toSmash));
  }
  return tDest;
}

/*
 * When we end a tracelet with a conditional jump, emitCondJmp first emits:
 *
 *   1:         j<CC> stubJmpccFirst
 *              jmp   stubJmpccFirst
 *
 * Our "taken" argument tells us whether the branch at 1: was taken or
 * not; and therefore which of offTaken and offNotTaken to continue executing.
 * If we did take the branch, we now rewrite the code so that the branch is
 * straightened. This predicts that subsequent executions will go the same way
 * as the first execution.
 *
 *              jn<CC> stubJmpccSecond:offNotTaken
 *              nop5   ; fallthru, or jmp if there's already a translation.
 * offTaken:
 *
 * If we did not take the branch, we leave the sense of the condition
 * intact, while patching it up to go to the unexplored code:
 *
 *              j<CC> stubJmpccSecond:offTaken
 *              nop5
 * offNotTaken:
 */
TCA
TranslatorX64::bindJmpccFirst(TCA toSmash,
                              Offset offTaken, Offset offNotTaken,
                              bool taken,
                              ConditionCode cc) {
  const Func* f = curFunc();
  LeaseHolder writer(s_writeLease);
  if (!writer) return NULL;
  Offset offWillExplore = taken ? offTaken : offNotTaken;
  Offset offWillDefer = taken ? offNotTaken : offTaken;
  SrcKey dest(f, offWillExplore);
  TRACE(3, "bindJmpccFirst: explored %d, will defer %d; overwriting cc%02x "
        "taken %d\n",
        offWillExplore, offWillDefer, cc, taken);

  // We want the branch to point to whichever side has not been explored
  // yet.
  if (taken) cc = ccNegate(cc);
  TCA stub =
    emitServiceReq(false, TranslatorX64::REQ_BIND_JMPCC_SECOND, 3,
                   toSmash, uint64_t(offWillDefer), uint64_t(cc));

  Asm &as = getAsmFor(toSmash);
  // Its not clear where chainFrom should go to if as is astubs
  ASSERT(&as == &a);

  // can we just directly fall through?
  // a jmp + jz takes 5 + 6 = 11 bytes
  bool fallThru = toSmash + kJmpccLen + kJmpLen == as.code.frontier &&
    !m_srcDB.find(dest);

  TCA tDest;
  tDest = getTranslation(&dest, !fallThru /* align */);
  if (!tDest) {
    return 0;
  }
  ASSERT(s_writeLease.amOwner());
  /*
   * Roll over the jcc and the jmp/fallthru. E.g., from:
   *
   *     toSmash:    jcc   <jmpccFirstStub>
   *     toSmash+6:  jmp   <jmpccFirstStub>
   *     toSmash+11: <probably the new translation == tdest>
   *
   * to:
   *
   *     toSmash:    j[n]z <jmpccSecondStub>
   *     toSmash+6:  nop5
   *     toSmash+11: newHotness
   */
  CodeCursor cg(as, toSmash);
  a.jcc(cc, stub);
  getSrcRec(dest)->chainFrom(as, IncomingBranch(as.code.frontier));
  TRACE(5, "bindJmpccFirst: overwrote with cc%02x taken %d\n", cc, taken);
  return tDest;
}

// smashes a jcc to point to a new destination
TCA
TranslatorX64::bindJmpccSecond(TCA toSmash, const Offset off,
                               ConditionCode cc) {
  const Func* f = curFunc();
  SrcKey dest(f, off);
  TCA branch = getTranslation(&dest, true);
  LeaseHolder writer(s_writeLease, NO_ACQUIRE);
  if (branch && writer.acquire()) {
    SrcRec* destRec = getSrcRec(dest);
    destRec->chainFrom(getAsmFor(toSmash), IncomingBranch(cc, toSmash));
  }
  return branch;
}

/*
 * emitBindJmp --
 *
 *   Emit code to lazily branch to the srckey in next. Assumes current
 *   basic block is closed (outputs synced, etc.).
 */
void
TranslatorX64::emitBindJmp(X64Assembler& _a, const SrcKey& dest,
                           ServiceRequest req) {
  prepareForSmash(_a, kJmpLen);
  TCA toSmash = _a.code.frontier;
  if (&_a == &astubs) {
    _a.   jmp(toSmash);
  }

  TCA sr = emitServiceReq(false, req, 2,
                          toSmash, uint64_t(dest.offset()));

  if (&_a == &astubs) {
    CodeCursor cc(_a, toSmash);
    _a.   jmp(sr);
  } else {
    _a.   jmp(sr);
  }
}

void
TranslatorX64::emitBindJmp(const SrcKey& dest) {
  emitBindJmp(a, dest);
}

void
TranslatorX64::emitStringCheck(X64Assembler& _a,
                               PhysReg base, int offset, PhysReg tmp) {
  // Treat KindOfString and KindOfStaticString identically; they
  // are bitwise identical. This is a port of our IS_STRING_TYPE
  // macro to assembly, and will have to change in sync with it.
  static_assert(IS_STRING_TYPE(7) && IS_STRING_TYPE(6),
                "Assembly version of IS_STRING_TYPE needs to be updated");
  _a.   load_reg64_disp_reg32(base, offset, tmp);
  _a.   and_imm32_reg32((signed char)(0xfe), tmp); // use 1-byte immediate
  _a.   cmp_imm32_reg32(6, tmp);
}

void
TranslatorX64::emitTypeCheck(X64Assembler& _a, DataType dt,
                             PhysReg base, int offset,
                             PhysReg tmp/*= InvalidReg*/) {
  offset += TVOFF(m_type);
  if (IS_STRING_TYPE(dt)) {
    LazyScratchReg scr(m_regMap);
    if (tmp == InvalidReg) {
      scr.alloc();
      tmp = *scr;
    }
    emitStringCheck(_a, base, offset, tmp);
  } else {
    _a. cmp_imm32_disp_reg32(dt, offset, base);
  }
}

void
TranslatorX64::checkType(X64Assembler& a,
                         const Location& l,
                         const RuntimeType& rtt,
                         SrcRec& fail) {
  // We can get invalid inputs as a side effect of reading invalid
  // items out of BBs we truncate; they don't need guards.
  if (rtt.isVagueValue()) return;

  PhysReg base;
  int disp = 0;
  SpaceRecorder sr("_CheckType", a);

  TRACE(1, Trace::prettyNode("Precond", DynLocation(l, rtt)) + "\n");

  locToRegDisp(l, &base, &disp);
  TRACE(2, "TypeCheck: %d(%%r%d)\n", disp, base);
  // Negative offsets from RSP are not yet allocated; they had
  // better not be inputs to the tracelet.
  ASSERT(l.space != Location::Stack || disp >= 0);
  if (Trace::moduleEnabled(Trace::stats, 2)) {
    Stats::emitInc(a, Stats::TraceletGuard_branch);
  }
  if (rtt.isIter()) {
    a.   cmp_imm32_disp_reg32(rtt.typeCheckValue(),
                              disp + rtt.typeCheckOffset(),
                              base);
  } else {
    emitTypeCheck(a, rtt.typeCheckValue(), base, disp, rax);
  }
  emitFallbackJmp(fail);
}

void
TranslatorX64::emitFallbackJmp(SrcRec& dest) {
  prepareForSmash(kJmpccLen);
  dest.emitFallbackJump(a, IncomingBranch(CC_NZ, a.code.frontier));
}

static inline uint64_t
packBitVec(const vector<bool>& bits, unsigned i) {
  uint64_t retval = 0;
  ASSERT(i % 64 == 0);
  ASSERT(i < bits.size());
  while (i < bits.size()) {
    retval |= bits[i] << (i % 64);
    if ((++i % 64) == 0) {
      break;
    }
  }
  return retval;
}

void
TranslatorX64::checkRefs(X64Assembler& a,
                         const SrcKey& sk,
                         const RefDeps& refDeps,
                         SrcRec& fail) {
  if (refDeps.size() == 0) {
    return;
  }

  // We're still between BB's, so ignore the register allocator.
  PhysReg rNumParams = rdi;
  PhysReg rMask = rdx;
  PhysReg rBits = rax;
  PhysReg rExpectedBits = rcx;
  PhysReg rBitsValue = rsi;
  PhysReg rFunc = r8;

  // Set up guards for each pushed ActRec that we've made reffiness
  // assumptions about
  for (RefDeps::ArMap::const_iterator it = refDeps.m_arMap.begin();
       it != refDeps.m_arMap.end(); ++it) {
    // Be careful! The actual Func might have fewer refs than the number
    // of args we're passing. To forestall this, we're going to have to
    // keep checking i against the number of params. We consider invocations
    // with too many arguments to have passed their checks.
    int entryArDelta = it->first;
    int32_t funcOff = cellsToBytes(entryArDelta) + offsetof(ActRec, m_func);
    a.    load_reg64_disp_reg64(rVmSp, funcOff, rFunc); // rFunc <- Func*
    a.    load_reg64_disp_reg32(rFunc, Func::numParamsOff(),
                                rNumParams);
    a.    load_reg64_disp_reg64(rFunc, Func::refBitVecOff(),
                                rBits);  // rBits <- m_refBitVec

    for (unsigned i = 0; i < it->second.m_mask.size(); i += 64) {
      ASSERT(i < it->second.m_vals.size());
      uint64_t mask = packBitVec(it->second.m_mask, i);
      if (mask == 0) {
        continue;
      }
      uint64_t value = packBitVec(it->second.m_vals, i);

      emitImmReg(a, mask,  rMask);
      emitImmReg(a, value, rExpectedBits);

      /*
       * Before trying to load this block off the bit vector, make
       * sure it actually exists.  It's ok to index past numArgs
       * within one of these words, because the remaining bits will be
       * set to zero (or one in the case of the variadic by ref
       * builtins).
       */
      if (Trace::moduleEnabled(Trace::stats, 2)) {
        Stats::emitInc(a, Stats::TraceletGuard_branch);
      }
      a.  cmp_imm32_reg32(i + 1, rNumParams);
      {
        IfElseBlock<CC_L> ifFewEnoughArgs(a);

        // Load the appropriate qword off of the top actRec's func*.
        SKTRACE(2, sk, "reffiness mask %lx value %lx, ar @%d\n",
                mask, value, entryArDelta);
        a.  load_reg64_disp_reg64(rBits, sizeof(uint64) * (i / 64),
                                  rBitsValue);  // rBitsValue <- rBits[i / 64]
        a.  and_reg64_reg64(rMask, rBitsValue); // rBitsValue &= rMask
        a.  cmp_reg64_reg64(rBitsValue, rExpectedBits);
        emitFallbackJmp(fail);

        ifFewEnoughArgs.Else();

        a.  test_imm32_disp_reg32(AttrVariadicByRef,
                                  Func::attrsOff(),
                                  rFunc);
        {
          IfElseBlock<CC_NZ> ifNotWeirdBuiltin(a);

          // Other than these builtins, we need to have all by value
          // args in this case.
          a.  test_reg64_reg64(rExpectedBits, rExpectedBits);
          emitFallbackJmp(fail);

          ifNotWeirdBuiltin.Else();

          // If it is one of the weird builtins that has reffiness for
          // additional args, we have to make sure our expectation is
          // that these additional args are by ref.
          a.  cmp_imm32_reg64((signed int)(-1ull & mask), rExpectedBits);
          emitFallbackJmp(fail);
        }
      }
    }
  }
}

/*
 * emitRetFromInterpretedFrame --
 *
 *   When the interpreter pushes a call frame, there is necessarily no
 *   machine RIP available to return to. This helper fishes out the
 *   destination from the frame and redirects execution to it via enterTC.
 */
TCA
TranslatorX64::emitRetFromInterpretedFrame() {
  int32_t arBase = sizeof(ActRec) - sizeof(Cell);
  moveToAlign(astubs);
  TCA stub = astubs.code.frontier;
  // Marshall our own args by hand here.
  astubs.   lea_reg64_disp_reg64(rVmSp, -arBase, serviceReqArgRegs[0]);
  astubs.   mov_reg64_reg64(rVmFp, serviceReqArgRegs[1]);
  (void) emitServiceReq(false, REQ_POST_INTERP_RET, 0ull);
  return stub;
}

/*
 * fcallHelperThunk
 * Note: Assumes rStashedAR is r15
 */
asm (
  ".byte 0\n"
  ".align 16\n"
  ".globl __fcallHelperThunk\n"
  "__fcallHelperThunk:\n"
#ifdef HHVM
  "mov %r15, %rdi\n"
  "call fcallHelper\n"
  "jmp *%rax\n"
#endif
  "ud2\n"
);

/*
 * enterTCHelper
 *
 * This helper routine is written in x64 assembly to take care of the details
 * when transferring control between jitted code and the translator.
 *   rdi:  Cell* vm_sp
 *   rsi:  Cell* vm_fp
 *   rdx:  unsigned char* start
 *   rcx:  TReqInfo* infoPtr
 *   r8:   ActRec* firstAR
 *   r9:   uint8_t* targetCacheBase
 *
 * Note: enterTCHelper does not save callee-saved registers except
 * %rbp.  This means when we call it from C++, we have to tell gcc to
 * clobber all the other callee-saved registers.
 */
asm (
  ".byte 0\n"
  ".align 16\n"
"__enterTCHelper:\n"
  // Prologue
  ".cfi_startproc\n"
  "push %rbp\n"
  ".cfi_adjust_cfa_offset 8\n"  // offset to previous frame relative to %rsp
  ".cfi_offset rbp, -16\n"      // Where to find previous value of rbp

  // Set firstAR->m_savedRbp to point to this frame.
  "mov %rsp, (%r8)\n"

  // Save infoPtr
  "push %rcx\n"
  ".cfi_adjust_cfa_offset 8\n"

  // Set up special registers used for translated code.
  "mov %rdi, %rbx\n"          // rVmSp
  "mov %r9, %r12\n"           // rVmTl
  "mov %rsi, %rbp\n"          // rVmFp
  "mov 0x30(%rcx), %r15\n"    // restore previous r15 value

  // The translated code we are about to enter does not follow the
  // standard prologue of pushing rbp at entry, so we are purposely 8
  // bytes short of 16-byte alignment before this call instruction so
  // that the return address being pushed will make the native stack
  // 16-byte aligned.
  "call *%rdx\n"

  // Restore infoPtr into r14
  "pop %r14\n"
  ".cfi_adjust_cfa_offset -8\n"

  // Copy the values passed from jitted code into *infoPtr
  "mov %rdi, 0x0(%r14)\n"
  "mov %rsi, 0x8(%r14)\n"
  "mov %rdx, 0x10(%r14)\n"
  "mov %rcx, 0x18(%r14)\n"
  "mov %r8,  0x20(%r14)\n"
  "mov %r9,  0x28(%r14)\n"
  "mov %r15, 0x30(%r14)\n"

  // Epilogue
  "pop %rbp\n"
  ".cfi_restore rbp\n"
  ".cfi_adjust_cfa_offset -8\n"
  "ret\n"
  ".cfi_endproc\n"
);

struct TReqInfo {
  uintptr_t requestNum;
  uintptr_t args[5];
  uintptr_t savedR15; // this TC register is preserved across service requests
};

void enterTCHelper(Cell* vm_sp,
                   Cell* vm_fp,
                   unsigned char* start,
                   TReqInfo* infoPtr,
                   ActRec* firstAR,
                   uint8_t* targetCacheBase) asm ("__enterTCHelper");

struct DepthGuard {
  static __thread int m_depth;
  DepthGuard()  { m_depth++; TRACE(2, "DepthGuard: %d {\n", m_depth); }
  ~DepthGuard() { TRACE(2, "DepthGuard: %d }\n", m_depth); m_depth--; }
};
__thread int DepthGuard::m_depth;
void
TranslatorX64::enterTC(SrcKey sk) {
  using namespace TargetCache;
  CT_ASSERT(rVmSp == rbx);
  CT_ASSERT(rVmFp == rbp);
  CT_ASSERT(rVmTl == r12);
  TCA start = getTranslation(&sk, true);

  DepthGuard d;
  TReqInfo info;
  const uintptr_t& requestNum = info.requestNum;
  uintptr_t* args = info.args;
  for (;;) {
    ASSERT(vmfp() >= vmsp() - 1);
    ASSERT(sizeof(Cell) == 16);
    ASSERT(((uintptr_t)vmsp() & (sizeof(Cell) - 1)) == 0);
    ASSERT(((uintptr_t)vmfp() & (sizeof(Cell) - 1)) == 0);

    TRACE(4, "enterTC: %p fp%p(%s) sp%p enter {\n", start,
          vmfp(), ((ActRec*)vmfp())->m_func->name()->data(), vmsp());
    s_writeLease.gremlinUnlock();
    // Keep dispatching until we end up somewhere the translator
    // recognizes, or we luck out and the leaseholder exits.
    while (!start) {
      TRACE(4, "enterTC forwarding BB to interpreter\n");
      g_vmContext->m_pc = curUnit()->at(sk.offset());
      INC_TPC(interp_bb);
      g_vmContext->dispatchBB();
      sk = SrcKey(curFunc(), g_vmContext->getPC());
      start = getTranslation(&sk, true);
    }
    ASSERT(start);
    ASSERT(isValidCodeAddress(start));
    tl_regState = REGSTATE_DIRTY;
    ASSERT(!s_writeLease.amOwner());
    curFunc()->validate();
    INC_TPC(enter_tc);

    // The asm volatile here is to force C++ to spill anything that
    // might be in a callee-saved register (aside from rbp).
    // enterTCHelper does not preserve these registers.
    asm volatile("" : : : "rbx","r12","r13","r14","r15");
    enterTCHelper(vmsp(), vmfp(), start, &info, vmFirstAR(),
                  tl_targetCaches.base);
    asm volatile("" : : : "rbx","r12","r13","r14","r15");

    tl_regState = REGSTATE_CLEAN; // Careful: pc isn't sync'ed yet.
    // Debugging code: cede the write lease half the time.
    if (debug && (RuntimeOption::EvalJitStressLease)) {
      if (d.m_depth == 1 && (rand() % 2) == 0) {
        s_writeLease.gremlinLock();
      }
    }

    TRACE(4, "enterTC: %p fp%p sp%p } return\n", start,
          vmfp(), vmsp());
    TRACE(4, "enterTC: request(%s) args: %lx %lx %lx %lx %lx\n",
          reqName(requestNum),
          args[0], args[1], args[2], args[3], args[4]);
    ASSERT(vmfp() >= vmsp() - 1 || requestNum == REQ_EXIT);

    if (debug) {
      // Ensure that each case either returns, or drives start to a valid
      // value.
      start = TCA(0xbee5face);
    }

    // The contract is that each case will either exit, by returning, or
    // set sk to the place where execution should resume, and optionally
    // set start to the hardware translation of the resumption point.
    //
    // start and sk might be subtly different; i.e., there are cases where
    // start != NULL && start != getTranslation(sk). For instance,
    // REQ_BIND_CALL has not finished executing the OpCall when it gets
    // here, and has even done some work on its behalf. sk == OpFCall,
    // while start == the point in the TC that's "half-way through" the
    // Call instruction. If we punt to the interpreter, the interpreter
    // will redo some of the work that the translator has already done.
    INC_TPC(service_req);
    switch (requestNum) {
      case REQ_EXIT: {
        // fp is not valid anymore
        vmfp() = NULL;
        return;
      }

      case REQ_BIND_CALL: {
        ReqBindCall* req = (ReqBindCall*)args[0];
        ActRec* calleeFrame = (ActRec*)args[1];
        TCA toSmash = req->m_toSmash;
        Func *func = const_cast<Func*>(calleeFrame->m_func);
        int nArgs = req->m_nArgs;
        bool isImmutable = req->m_isImmutable;
        TCA dest = tx64->funcPrologue(func, nArgs);
        TRACE(2, "enterTC: bindCall %s -> %p\n", func->name()->data(), dest);
        if (isImmutable) {
          // If we *know* we're calling the right function, don't bother
          // with the dynamic check of ar->m_func.
          dest = skipFuncCheck(dest);
          TRACE(2, "enterTC: bindCall immutably %s -> %p\n",
                func->name()->data(), dest);
        }
        LeaseHolder writer(s_writeLease, NO_ACQUIRE);
        if (dest && writer.acquire()) {
          TRACE(2, "enterTC: bindCall smash %p -> %p\n", toSmash, dest);
          smash(tx64->getAsmFor(toSmash), toSmash, dest);
          // sk: stale, but doesn't matter since we have a valid dest TCA.
        } else {
          // We need translator help; we're not at the callee yet, so
          // roll back. The prelude has done some work already, but it
          // should be safe to redo.
          TRACE(2, "enterTC: bindCall rollback smash %p -> %p\n",
                toSmash, dest);
          sk = req->m_sourceInstr;
        }
        start = dest;
      } break;

      case REQ_BIND_SIDE_EXIT:
      case REQ_BIND_JMP:
      case REQ_BIND_ADDR: {
        TCA toSmash = (TCA)args[0];
        Offset off = args[1];
        sk = SrcKey(curFunc(), off);
        if (requestNum == REQ_BIND_SIDE_EXIT) {
          SKTRACE(4, sk, "side exit taken!\n");
        }
        start = bindJmp(toSmash, sk, requestNum == REQ_BIND_ADDR);
      } break;

      case REQ_BIND_JMPCC_FIRST: {
        TCA toSmash = (TCA)args[0];
        Offset offTaken = (Offset)args[1];
        Offset offNotTaken = (Offset)args[2];
        ConditionCode cc = ConditionCode(args[3]);
        bool taken = int64(args[4]) & 1;
        start = bindJmpccFirst(toSmash, offTaken, offNotTaken, taken, cc);
        // SrcKey: we basically need to emulate the fail
        sk = SrcKey(curFunc(), taken ? offTaken : offNotTaken);
      } break;

      case REQ_BIND_JMPCC_SECOND: {
        TCA toSmash = (TCA)args[0];
        Offset off = (Offset)args[1];
        ConditionCode cc = ConditionCode(args[2]);
        start = bindJmpccSecond(toSmash, off, cc);
        sk = SrcKey(curFunc(), off);
      } break;

      case REQ_BIND_REQUIRE: {
        ReqLitStaticArgs* rlsa = (ReqLitStaticArgs*)args[0];
        sk = SrcKey((Func*)args[1], (Offset)args[2]);
        start = getTranslation(&sk, true);
        if (start) {
          LeaseHolder writer(s_writeLease);
          if (writer) {
            SrcRec* sr = getSrcRec(sk);
            sr->chainFrom(a, IncomingBranch(&rlsa->m_pseudoMain));
          }
        }
      } break;

      case REQ_RETRANSLATE: {
        INC_TPC(retranslate);
        sk = SrcKey(curFunc(), (Offset)args[0]);
        start = retranslate(sk, true);
        SKTRACE(2, sk, "retranslated @%p\n", start);
      } break;

      case REQ_INTERPRET: {
        Offset off = args[0];
        int numInstrs = args[1];
        g_vmContext->m_pc = curUnit()->at(off);
        /*
         * We know the compilation unit has not changed; basic blocks do
         * not span files. I claim even exceptions do not violate this
         * axiom.
         */
        ASSERT(numInstrs >= 0);
        ONTRACE(5, SrcKey(curFunc(), off).trace("interp: enter\n"));
        if (numInstrs) {
          s_perfCounters[tpc_interp_instr] += numInstrs;
          g_vmContext->dispatchN(numInstrs);
        } else {
          // numInstrs == 0 means it wants to dispatch until BB ends
          INC_TPC(interp_bb);
          g_vmContext->dispatchBB();
        }
        SrcKey newSk(curFunc(), g_vmContext->getPC());
        SKTRACE(5, newSk, "interp: exit\n");
        sk = newSk;
        start = getTranslation(&newSk, true);
      } break;

      case REQ_POST_INTERP_RET: {
        // This is only responsible for the control-flow aspect of the Ret:
        // getting to the destination's translation, if any.
        ActRec* ar = (ActRec*)args[0];
        ActRec* caller = (ActRec*)args[1];
        ASSERT((Cell*) ar < vmsp());      // ar is already logically popped
        ASSERT((Cell*) caller > vmsp());  // caller is now active
        ASSERT((Cell*) caller == vmfp());
        Unit* destUnit = caller->m_func->unit();
        // Set PC so logging code in getTranslation doesn't get confused.
        vmpc() = destUnit->at(caller->m_func->base() + ar->m_soff);
        SrcKey dest(caller->m_func, vmpc());
        sk = dest;
        start = getTranslation(&dest, true);
        TRACE(3, "REQ_POST_INTERP_RET: from %s to %s\n",
              ar->m_func->fullName()->data(),
              caller->m_func->fullName()->data());
      } break;

      case REQ_RESUME: {
        SrcKey dest(curFunc(), vmpc());
        sk = dest;
        start = getTranslation(&dest, true);
      } break;

      case REQ_STACK_OVERFLOW: {
        /*
         * we need to construct the pc of the fcall from the return
         * address (which will be after the fcall). Because fcall is
         * a variable length instruction, and because we sometimes
         * delete instructions from the instruction stream, we
         * need to use fpi regions to find the fcall.
         */
        const FPIEnt* fe = curFunc()->findPrecedingFPI(
          curUnit()->offsetOf(vmpc()));
        vmpc() = curUnit()->at(fe->m_fcallOff);
        ASSERT(*vmpc() == OpFCall);
        raise_error("Stack overflow");
        return;
      }
    }
  }
  NOT_REACHED();
}

void TranslatorX64::resume(SrcKey sk) {
  enterTC(sk);
}

/*
 * emitServiceReq --
 *
 *   Call a translator service co-routine. The code emitted here is
 *   reenters the enterTC loop, invoking the requested service. Control
 *   will be returned non-locally to the next logical instruction in
 *   the TC.
 *
 *   Return value is a destination; we emit the bulky service
 *   request code into astubs.
 */

TCA
TranslatorX64::emitServiceReqVA(bool align, ServiceRequest req, int numArgs,
                                va_list args) {
  if (align) {
    moveToAlign(astubs);
  }
  TCA retval = astubs.code.frontier;
  emitEagerVMRegSave(astubs, SaveFP);
  /*
   * Move args into appropriate regs.
   */
  TRACE(3, "Emit Service Req %s(", reqName(req));
  for (int i = 0; i < numArgs; i++) {
    uint64_t argVal = va_arg(args, uint64_t);
    TRACE(3, "%ld,", argVal);
    emitImmReg(astubs, argVal, serviceReqArgRegs[i]);
  }
  TRACE(3, ")\n");
  emitImmReg(astubs, req, rdi);
  /*
   * Weird hand-shaking with enterTC: reverse-call a service routine.
   */
  astubs.    ret();
  recordBCInstr(OpServiceRequest, astubs, retval);
  translator_not_reached(astubs);
  return retval;
}

TCA
TranslatorX64::emitServiceReq(ServiceRequest req, int numArgs, ...) {
  va_list args;
  va_start(args, numArgs);
  TCA retval = emitServiceReqVA(true, req, numArgs, args);
  va_end(args);
  return retval;
}

TCA
TranslatorX64::emitServiceReq(bool align, ServiceRequest req, int numArgs,
                              ...) {
  va_list args;
  va_start(args, numArgs);
  TCA retval = emitServiceReqVA(align, req, numArgs, args);
  va_end(args);
  return retval;
}

TCA
TranslatorX64::emitTransCounterInc(X64Assembler& a) {
  TCA start = a.code.frontier;
  if (!isTransDBEnabled()) return start;
  uint64* counterAddr = getTransCounterAddr();

  a.mov_imm64_reg((uint64)counterAddr, rScratch);
  a.emitLockPrefix();
  a.inc_mem64(rScratch, 0);

  return start;
}

void
TranslatorX64::spillTo(DataType type, PhysReg reg, bool writeType,
                       PhysReg base, int disp) {
  X64Assembler& a = *m_spillFillCode;
  // Zero out the count at the same time as writing the type.
  SpaceRecorder sr("_Spill", a);

  Stats::emitInc(a, Stats::Tx64_Spill);
  if (writeType) {
    a.    store_imm32_disp_reg(type, disp + TVOFF(m_type), base);
  }
  if (!IS_NULL_TYPE(type)) {
    a.    store_reg64_disp_reg64(reg, disp + TVOFF(m_data), base);
  }
}

void
TranslatorX64::spill(const Location& loc, DataType type,
                     PhysReg reg, bool writeType) {
  PhysReg base;
  int disp;
  locToRegDisp(loc, &base, &disp);
  spillTo(type, reg, writeType, base, disp);
  TRACE(2, "%s: (%s, %lld) -> v: %d(r%d) type%d\n",
        __func__,
        loc.spaceName(), loc.offset, int(disp + TVOFF(m_data)), base, type);
}

void
TranslatorX64::fill(const Location& loc, PhysReg reg) {
  SpaceRecorder sr("_Fill", *m_spillFillCode);
  PhysReg base;
  int disp;
  locToRegDisp(loc, &base, &disp);
  TRACE(2, "fill: (%s, %lld) -> reg %d\n",
        loc.spaceName(), loc.offset, reg);
  m_spillFillCode->load_reg64_disp_reg64(base, disp + TVOFF(m_data), reg);
}

void TranslatorX64::fillByMov(PhysReg src, PhysReg dst) {
  SpaceRecorder sr("_FillMov", *m_spillFillCode);
  ASSERT(src != dst);
  m_spillFillCode->mov_reg64_reg64(src, dst);
}

void
TranslatorX64::loadImm(int64 immVal, PhysReg reg) {
  SpaceRecorder sr("_FillImm", *m_spillFillCode);
  TRACE(2, "loadImm: 0x%llx -> reg %d\n", immVal, reg);
  emitImmReg(*m_spillFillCode, immVal, reg);
}

void
TranslatorX64::poison(PhysReg dest) {
  static const bool poison = false;
  if (poison) {
    emitImmReg(*m_spillFillCode, 0xbadf00d105e5babe, dest);
  }
}

/**
 * Spill all dirty registers, mark all registers as 'free' in the
 * register file, and update rVmSp to point to the top of stack at
 * the end of the tracelet.
 */
void
TranslatorX64::syncOutputs(const Tracelet& t) {
  syncOutputs(t.m_stackChange);
}

/**
 * Same as above, except that it sets rVmSp to point to the top of
 * stack at the beginning of the specified instruction.
 */
void
TranslatorX64::syncOutputs(const NormalizedInstruction& i) {
  syncOutputs(i.stackOff);
}

void
TranslatorX64::syncOutputs(int stackOff) {
  SpaceRecorder sr("_SyncOuts", a);
  TCA start = a.code.frontier;
  // Mark all stack locations above the top of stack as dead
  m_regMap.scrubStackEntries(stackOff);
  // Spill all dirty registers
  m_regMap.cleanAll();
  if (stackOff != 0) {
    TRACE(1, "syncOutputs: rVmSp + %d\n", stackOff);
    // t.stackChange is in negative Cells, not bytes.
    a.    add_imm32_reg64(-cellsToBytes(stackOff), rVmSp);
  }
  // All registers have been smashed for realz, yo
  m_regMap.smashRegs(kAllRegs);
  recordBCInstr(OpSyncOutputs, a, start);
}

/*
 * getBinaryStackInputs --
 *
 *   Helper for a common pattern of instruction, where two items are popped
 *   and one is pushed. The second item on the stack at the beginning of
 *   the instruction is both a source and destination.
 */
static void
getBinaryStackInputs(const RegAlloc& regmap, const NormalizedInstruction& i,
                     PhysReg& rsrc, PhysReg& rsrcdest) {
  ASSERT(i.inputs.size()   == 2);
  ASSERT(i.outStack && !i.outLocal);
  rsrcdest = regmap.getReg(i.outStack->location);
  rsrc     = regmap.getReg(i.inputs[0]->location);
  ASSERT(regmap.getReg(i.inputs[1]->location) == rsrcdest);
}

// emitBox --
//   Leave a boxed version of input in RAX. Destroys the register
//   mapping.
void
TranslatorX64::emitBox(DataType t, PhysReg rSrc) {
  if (false) { // typecheck
    TypedValue* retval = tvBoxHelper(KindOfArray, 0xdeadbeef01ul);
    (void)retval;
  }
  // tvBoxHelper will set the refcount of the inner cell to 1
  // for us. Because the inner cell now holds a reference to the
  // original value, we don't need to perform a decRef.
  EMIT_CALL(a, tvBoxHelper, IMM(t), R(rSrc));
}

// emitUnboxTopOfStack --
//   Unbox the known-to-be Variant on top of stack in place.
void
TranslatorX64::emitUnboxTopOfStack(const NormalizedInstruction& i) {
  const vector<DynLocation*>& inputs  = i.inputs;

  ASSERT(inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(inputs[0]->isStack());
  ASSERT(i.outStack && i.outStack->location == inputs[0]->location);
  DataType outType = inputs[0]->rtt.innerType();
  ASSERT(outType != KindOfInvalid);
  ASSERT(outType == i.outStack->outerType());
  PhysReg rSrc = getReg(inputs[0]->location);
  // Detach the register rSrc from the input location. We must
  // do this dance because the input and output location are the
  // same and we want to have separate registers for the input
  // and output.
  m_regMap.invalidate(inputs[0]->location);
  m_regMap.bind(rSrc, Location(), KindOfInvalid, RegInfo::SCRATCH);
  // This call to allocOutputRegs will allocate a new register
  // for the output location
  m_regMap.allocOutputRegs(i);
  PhysReg rDest = getReg(i.outStack->location);
  emitDeref(a, rSrc, rDest);
  emitIncRef(rDest, outType);
  // decRef the var on the evaluation stack
  emitDecRef(i, rSrc, KindOfRef);
  m_regMap.freeScratchReg(rSrc);
}

// setOpOpToOpcodeOp --
//   The SetOp opcode space has nothing to do with the bytecode opcode
//   space. Reasonable people like it that way, so translate them here.
static Opcode
setOpOpToOpcodeOp(SetOpOp soo) {
  switch(soo) {
#define SETOP_OP(_soo, _bc) case _soo: return _bc;
    SETOP_OPS
#undef SETOP_OP
    default: ASSERT(false);
  }
  return -1;
}

void
TranslatorX64::binaryIntegerArith(const NormalizedInstruction& i,
                                  Opcode op,
                                  PhysReg srcReg,
                                  PhysReg srcDestReg) {
  switch (op) {
#define CASEIMM(OpBc, x64op)                                       \
    case OpBc: {                                                   \
      if (i.hasConstImm) {                                         \
        a.   x64op ## _imm64_reg64(i.constImm.u_I64A, srcDestReg); \
      } else {                                                     \
        a.   x64op ## _reg64_reg64(srcReg, srcDestReg);            \
      } } break;
#define CASE(OpBc, x64op)                                          \
    case OpBc: {                                                   \
        a.   x64op ## _reg64_reg64(srcReg, srcDestReg);            \
    } break;
    CASEIMM(OpAdd,    add)
    CASEIMM(OpSub,    sub)
    CASEIMM(OpBitAnd, and)
    CASEIMM(OpBitOr,  or)
    CASEIMM(OpBitXor, xor)
    CASE(OpMul,       imul)
#undef CASE
#undef CASEIMM

    default: {
      not_reached();
    };
  }
}

void
TranslatorX64::binaryArithCell(const NormalizedInstruction &i,
                               Opcode op, const DynLocation& in1,
                               const DynLocation& inout) {
  ASSERT(in1.rtt.isInt());
  ASSERT(inout.rtt.isInt());
  ASSERT(in1.outerType() != KindOfRef);
  ASSERT(in1.isStack());
  ASSERT(inout.outerType() != KindOfRef);
  ASSERT(inout.isStack());
  m_regMap.allocOutputRegs(i);
  PhysReg     srcReg = m_regMap.getReg(in1.location);
  PhysReg srcDestReg = m_regMap.getReg(inout.location);
  binaryIntegerArith(i, op, srcReg, srcDestReg);
}

void
TranslatorX64::binaryArithLocal(const NormalizedInstruction &i,
                                Opcode op,
                                const DynLocation& in1,
                                const DynLocation& in2,
                                const DynLocation& out) {
  // The caller must guarantee that these conditions hold
  ASSERT(in1.rtt.isInt());
  ASSERT(in2.rtt.isInt());
  ASSERT(in1.outerType() != KindOfRef);
  ASSERT(in1.isStack());
  ASSERT(in2.isLocal());
  ASSERT(out.isStack());

  PhysReg srcReg = m_regMap.getReg(in1.location);
  PhysReg outReg = m_regMap.getReg(out.location);
  PhysReg localReg = m_regMap.getReg(in2.location);
  if (in2.outerType() != KindOfRef) {
    // The local is not a var, so we can operate directly on the
    // local's register. We will need to update outReg after the
    // operation.
    binaryIntegerArith(i, op, srcReg, localReg);
    // We operated directly on the local's register, so we need to update
    // outReg
    emitMovRegReg(localReg, outReg);
  } else {
    // The local is a var, so we have to read its value into outReg
    // on operate on that. We will need to write the result back
    // to the local after the operation.
    emitDeref(a, localReg, outReg);
    binaryIntegerArith(i, op, srcReg, outReg);
    // We operated on outReg, so we need to write the result back to the
    // local
    a.    store_reg64_disp_reg64(outReg, 0, localReg);
  }
}

static void interp_set_regs(ActRec* ar, Cell* sp, Offset pcOff) {
  ASSERT(tl_regState == REGSTATE_DIRTY);
  tl_regState = REGSTATE_CLEAN;
  vmfp() = (Cell*)ar;
  vmsp() = sp;
  vmpc() = curUnit()->at(pcOff);
  ASSERT(vmsp() <= vmfp());
}

#define O(opcode, imm, pusph, pop, flags) \
/**
 * The interpOne methods saves m_pc, m_fp, and m_sp ExecutionContext,
 * calls into the interpreter, and then return a pointer to the
 * current ExecutionContext.
 */  \
VMExecutionContext*                                                     \
interpOne##opcode(ActRec* ar, Cell* sp, Offset pcOff) {                 \
  interp_set_regs(ar, sp, pcOff);                                       \
  SKTRACE(5, SrcKey(curFunc(), vmpc()), "%40s %p %p\n",                 \
          "interpOne" #opcode " before (fp,sp)",                        \
          vmfp(), vmsp());                                              \
  ASSERT(*vmpc() == Op ## opcode);                                      \
  VMExecutionContext* ec = g_vmContext;                                 \
  Stats::inc(Stats::Instr_InterpOne ## opcode);                         \
  INC_TPC(interp_one)                                                   \
  /* Correct for over-counting in TC-stats. */                          \
  Stats::inc(Stats::Instr_TC, -1);                                      \
  ec->op##opcode();                                                     \
  /*
   * Only set regstate back to dirty if an exception is not
   * propagating.  If an exception is throwing, regstate for this call
   * is actually still correct, and we don't have information in the
   * fixup map for interpOne calls anyway.
   */ \
  tl_regState = REGSTATE_DIRTY;                                         \
  return ec;;                                                           \
}

OPCODES
#undef O

void* interpOneEntryPoints[] = {
#define O(opcode, imm, pusph, pop, flags) \
  (void*)(interpOne ## opcode),
OPCODES
#undef O
};

void TranslatorX64::fixupWork(VMExecutionContext* ec, ActRec* rbp) const {
  ASSERT(RuntimeOption::EvalJit);
  ActRec* nextAr = rbp;
  do {
    rbp = nextAr;
    FixupMap::VMRegs regs;
    TRACE(10, "considering frame %p, %p\n", rbp, (void*)rbp->m_savedRip);
    if (g_vmContext->m_stack.isValidAddress(rbp->m_savedRbp) &&
        m_fixupMap.getFrameRegs(rbp, &regs)) {
      TRACE(10, "fixup func %s fp %p sp %p pc %p\n",
            regs.m_fp->m_func->name()->data(),
            regs.m_fp, regs.m_sp, regs.m_pc);
      ec->m_fp = const_cast<ActRec*>(regs.m_fp);
      ec->m_pc = regs.m_pc;
      vmsp() = regs.m_sp;
      return;
    }
    nextAr = (ActRec*)rbp->m_savedRbp;
  } while (rbp && rbp != nextAr);
  // OK, we've exhausted the entire actRec chain.
  // We are only invoking ::fixup() from contexts that were known
  // to be called out of the TC, so this cannot happen.
  NOT_REACHED();
}

void TranslatorX64::fixup(VMExecutionContext* ec) const {
  // Start looking for fixup entries at the current (C++) frame.  This
  // will walk the frames upward until we find a TC frame.
  ActRec* rbp;
  asm volatile("mov %%rbp, %0" : "=r"(rbp));
  fixupWork(ec, rbp);
}

void
TranslatorX64::syncWork() {
  ASSERT(tl_regState == REGSTATE_DIRTY);
  fixup(g_vmContext);
  tl_regState = REGSTATE_CLEAN;
  Stats::inc(Stats::TC_Sync);
}

void
TranslatorX64::emitInterpOne(const Tracelet& t,
                             const NormalizedInstruction &ni) {
  // Write any dirty values to memory
  m_regMap.cleanAll();
  // Call into the appropriate interpOne method. Note that this call will
  // preserve the callee-saved registers including rVmFp and rVmSp.
  if (false) { /* typecheck */
    UNUSED VMExecutionContext* ec = interpOnePopC((ActRec*)vmfp(), vmsp(), 0);
  }
  void* func = interpOneEntryPoints[ni.op()];
  TRACE(3, "ip %p of unit %p -> interpOne @%p\n", ni.pc(), ni.unit(), func);
  EMIT_CALL(a, func,
             R(rVmFp),
             RPLUS(rVmSp, -int32_t(cellsToBytes(ni.stackOff))),
             IMM(ni.source.offset()));
  // The interpreter may have written to memory, so we need to invalidate
  // all locations
  m_regMap.reset();
  // The interpOne method returned a pointer to the current
  // ExecutionContext in rax, so we can read the 'm_*' fields
  // by adding the appropriate offset to rax and dereferencing.

  // If this instruction ends the tracelet, we have some extra work to do.
  if (ni.breaksBB) {
    // Read the 'm_fp' and 'm_stack.m_top' fields into the rVmFp and
    // rVmSp registers.
    a.  load_reg64_disp_reg64(rax, offsetof(VMExecutionContext, m_fp),
                              rVmFp);
    a.  load_reg64_disp_reg64(rax, offsetof(VMExecutionContext, m_stack) +
                              Stack::topOfStackOffset(), rVmSp);
    if (opcodeChangesPC(ni.op())) {
      // If interpreting this instruction can potentially set PC to point
      // to something other than the next instruction in the bytecode, so
      // we need to emit a service request to figure out where to go next
      TCA stubDest = emitServiceReq(REQ_RESUME, 0ull);
      a.    jmp(stubDest);
    } else {
      // If this instruction always advances PC to the next instruction in
      // the bytecode, then we know what SrcKey to bind to
      emitBindJmp(nextSrcKey(t, ni));
    }
  }
}

static inline TXFlags
plan(bool cond, TXFlags successFlags) {
  return cond ? successFlags : Interp;
}

static inline TXFlags simplePlan(bool cond) { return plan(cond, Simple); }
static inline TXFlags supportedPlan(bool cond) { return plan(cond, Supported); }
static inline TXFlags nativePlan(bool cond) { return plan(cond, Native); }

static inline TXFlags planHingesOnRefcounting(DataType type) {
  return !IS_REFCOUNTED_TYPE(type) ? Native :
         !canDtorStubReenter(type) ? Simple :
         Supported;
}

static TXFlags
planBinaryArithOp(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  return nativePlan(i.inputs[0]->isInt() && i.inputs[1]->isInt());
}

void
TranslatorX64::analyzeBinaryArithOp(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = planBinaryArithOp(i);
}

void
TranslatorX64::translateBinaryArithOp(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  const Opcode op = i.op();
  ASSERT(op == OpSub || op == OpMul || op == OpBitAnd ||
         op == OpBitOr || op == OpBitXor);
  ASSERT(planBinaryArithOp(i));
  ASSERT(i.inputs.size() == 2);

  binaryArithCell(i, op, *i.inputs[0], *i.outStack);
}

static inline bool sameDataTypes(DataType t1, DataType t2) {
  return TypeConstraint::equivDataTypes(t1, t2);
}

static TXFlags
planSameOp_SameTypes(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  const RuntimeType& left = i.inputs[0]->rtt;
  const RuntimeType& right = i.inputs[1]->rtt;
  DataType leftType = left.outerType();
  DataType rightType = right.outerType();
  return nativePlan(sameDataTypes(leftType, rightType) &&
                    (left.isNull() || leftType == KindOfBoolean ||
                     left.isInt() || left.isString()));
}

static TXFlags
planSameOp_DifferentTypes(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  DataType leftType = i.inputs[0]->outerType();
  DataType rightType = i.inputs[1]->outerType();
  if (!sameDataTypes(leftType, rightType)) {
    if (IS_REFCOUNTED_TYPE(leftType) || IS_REFCOUNTED_TYPE(rightType)) {
      // For dissimilar datatypes, we might call out to handle a refcount.
      return Supported;
    }
    return Native;
  }
  return Interp;
}

void
TranslatorX64::analyzeSameOp(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(!(planSameOp_SameTypes(i) && planSameOp_DifferentTypes(i)));
  i.m_txFlags = TXFlags(planSameOp_SameTypes(i) | planSameOp_DifferentTypes(i));
  i.manuallyAllocInputs = true;
}

void
TranslatorX64::translateSameOp(const Tracelet& t,
                               const NormalizedInstruction& i) {
  const Opcode op = i.op();
  ASSERT(op == OpSame || op == OpNSame);
  const vector<DynLocation*>& inputs  = i.inputs;
  bool instrNeg = (op == OpNSame);
  ASSERT(inputs.size() == 2);
  ASSERT(i.outStack && !i.outLocal);
  DataType leftType = i.inputs[0]->outerType();
  DataType rightType DEBUG_ONLY = i.inputs[1]->outerType();
  ASSERT(leftType != KindOfRef);
  ASSERT(rightType != KindOfRef);

  if (planSameOp_DifferentTypes(i)) {
    // Some easy cases: when the valueTypes do not match,
    // NSame -> true and Same -> false.
    SKTRACE(1, i.source, "different types %d %d\n",
            leftType, rightType);
    emitDecRefInput(a, i, 0);
    emitDecRefInput(a, i, 1);
    m_regMap.allocOutputRegs(i);
    emitImmReg(a, instrNeg, getReg(i.outStack->location));
    return; // Done
  }

  ASSERT(planSameOp_SameTypes(i));

  if (IS_NULL_TYPE(leftType)) {
    m_regMap.allocOutputRegs(i);
    // null === null is always true
    SKTRACE(2, i.source, "straightening null/null comparison\n");
    emitImmReg(a, !instrNeg, getReg(i.outStack->location));
    return; // Done
  }
  if (IS_STRING_TYPE(leftType)) {
    int args[2];
    args[0] = 0;
    args[1] = 1;
    allocInputsForCall(i, args);
    EMIT_CALL(a, same_str_str,
               V(inputs[0]->location),
               V(inputs[1]->location));
    if (instrNeg) {
      a.  xor_imm32_reg32(1, rax);
    }
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
    return; // Done
  }
  m_regMap.allocInputRegs(i);
  PhysReg src, srcdest;
  getBinaryStackInputs(m_regMap, i, src, srcdest);
  m_regMap.allocOutputRegs(i);
  ASSERT(getReg(i.outStack->location) == srcdest);
  a.    cmp_reg64_reg64(src, srcdest);
  if (op == OpSame) {
    a.  sete(srcdest);
  } else {
    a.  setne(srcdest);
  }
  a.    mov_reg8_reg64_unsigned(srcdest, srcdest);
}

static bool
trivialEquivType(const RuntimeType& rtt) {
  DataType t = rtt.valueType();
  return t == KindOfUninit || t == KindOfNull || t == KindOfBoolean ||
    rtt.isInt() || rtt.isString();
}

static void
emitConvertToBool(X64Assembler &a, PhysReg src, PhysReg dest, bool instrNeg) {
  a.    test_reg64_reg64(src, src);
  if (instrNeg) {
    a.  setz(dest);
  } else {
    a.  setnz(dest);
  }
  a.    mov_reg8_reg64_unsigned(dest, dest);
}

void
TranslatorX64::analyzeEqOp(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  RuntimeType &lt = i.inputs[0]->rtt;
  RuntimeType &rt = i.inputs[1]->rtt;
  i.m_txFlags = nativePlan(trivialEquivType(lt) &&
                           trivialEquivType(rt));
  if (i.isNative() &&
      IS_NULL_TYPE(lt.outerType()) &&
      IS_NULL_TYPE(rt.outerType())) {
    i.manuallyAllocInputs = true;
  }
}

void
TranslatorX64::translateEqOp(const Tracelet& t,
                             const NormalizedInstruction& i) {
  const Opcode op = i.op();
  ASSERT(op == OpEq || op == OpNeq);
  ASSERT(i.isNative());
  const vector<DynLocation*>& inputs  = i.inputs;
  bool instrNeg = (op == OpNeq);
  ASSERT(inputs.size() == 2);
  ASSERT(i.outStack && !i.outLocal);
  DataType leftType = i.inputs[0]->outerType();
  DataType rightType = i.inputs[1]->outerType();
  ASSERT(leftType != KindOfRef);
  ASSERT(rightType != KindOfRef);

  ConditionCode ccBranch = CC_E;
  if (instrNeg) ccBranch = ccNegate(ccBranch);

  // Inputless case.
  if (IS_NULL_TYPE(leftType) && IS_NULL_TYPE(rightType)) {
    ASSERT(i.manuallyAllocInputs);
    // null == null is always true
    bool result = !instrNeg;
    SKTRACE(2, i.source, "straightening null/null comparison\n");
    if (i.changesPC) {
      fuseBranchAfterStaticBool(t, i, result);
    } else {
      m_regMap.allocOutputRegs(i);
      emitImmReg(a, result, getReg(i.outStack->location));
    }
    return; // Done
  }

  if (IS_STRING_TYPE(leftType) || IS_STRING_TYPE(rightType)) {
    void* fptr = NULL;
    bool leftIsString = false;
    bool eqNullStr = false;
    switch (leftType) {
      STRINGCASE(): {
        leftIsString = true;
        switch (rightType) {
          STRINGCASE(): fptr = (void*)eq_str_str; break;
          case KindOfInt64: fptr = (void*)eq_int_str; break;
          case KindOfBoolean: fptr = (void*)eq_bool_str; break;
          NULLCASE(): fptr = (void*)eq_null_str; eqNullStr = true; break;
          default: ASSERT(false); break;
        }
      } break;
      case KindOfInt64: fptr = (void*)eq_int_str; break;
      case KindOfBoolean: fptr = (void*)eq_bool_str; break;
      NULLCASE(): fptr = (void*)eq_null_str; eqNullStr = true; break;
      default: ASSERT(false); break;
    }
    if (eqNullStr) {
      ASSERT(fptr == (void*)eq_null_str);
      EMIT_CALL(a, fptr,
                 V(inputs[leftIsString ? 0 : 1]->location));
    } else {
      ASSERT(fptr != NULL);
      EMIT_CALL(a, fptr,
                 V(inputs[leftIsString ? 1 : 0]->location),
                 V(inputs[leftIsString ? 0 : 1]->location));
    }
    if (i.changesPC) {
      fuseBranchSync(t, i);
      a.   test_imm32_reg32(1, rax);
      fuseBranchAfterBool(t, i, ccNegate(ccBranch));
      return;
    }
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
    if (instrNeg) {
      a.  xor_imm32_reg32(1, rax);
    }
    return;
  }

  m_regMap.allocOutputRegs(i);
  PhysReg src, srcdest;
  getBinaryStackInputs(m_regMap, i, src, srcdest);
  ASSERT(getReg(i.outStack->location) == srcdest);
  if (i.changesPC) {
    fuseBranchSync(t, i);
  }
  if (IS_NULL_TYPE(leftType) || IS_NULL_TYPE(rightType)) {
    if (IS_NULL_TYPE(leftType)) {
      a.   test_reg64_reg64(srcdest, srcdest);
    } else {
      ASSERT(IS_NULL_TYPE(rightType));
      a.   test_reg64_reg64(src, src);
    }
  } else if (leftType  == KindOfBoolean ||
             rightType == KindOfBoolean) {
    // OK to destroy src and srcdest in-place; their stack locations are
    // blown away by this instruction.
    if (leftType != KindOfBoolean)
      emitConvertToBool(a, src, src, false);
    if (rightType != KindOfBoolean)
      emitConvertToBool(a, srcdest, srcdest, false);
    a.   cmp_reg64_reg64(src, srcdest);
  } else {
    a.   cmp_reg64_reg64(src, srcdest);
  }
  if (i.changesPC) {
    fuseBranchAfterBool(t, i, ccBranch);
    return;
  }
  if (instrNeg) {
    a.   setnz          (srcdest);
  } else {
    a.   setz           (srcdest);
  }
  a.     mov_reg8_reg64_unsigned(srcdest, srcdest);
}

void
TranslatorX64::analyzeLtGtOp(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  const RuntimeType& left = i.inputs[0]->rtt;
  DataType leftType = left.outerType();
  DataType rightType = i.inputs[1]->outerType();
  i.m_txFlags = nativePlan(sameDataTypes(leftType, rightType) &&
                           (left.isNull() ||
                            leftType == KindOfBoolean ||
                            left.isInt()));
  if (i.isNative() && IS_NULL_TYPE(left.outerType())) {
    // No inputs. w00t.
    i.manuallyAllocInputs = true;
  }
}

void
TranslatorX64::translateLtGtOp(const Tracelet& t,
                               const NormalizedInstruction& i) {
  const Opcode op = i.op();
  ASSERT(op == OpLt || op == OpLte || op == OpGt || op == OpGte);
  ASSERT(i.isNative());
  ASSERT(i.inputs.size() == 2);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.inputs[0]->outerType() != KindOfRef);
  ASSERT(i.inputs[1]->outerType() != KindOfRef);

  bool fEquals = (op == OpLte || op == OpGte);
  bool fLessThan = (op == OpLt || op == OpLte);

  m_regMap.allocOutputRegs(i);
  if (IS_NULL_TYPE(i.inputs[0]->outerType())) {
    ASSERT(IS_NULL_TYPE(i.inputs[1]->outerType()));
    // null < null is always false, null <= null is always true
    SKTRACE(2, i.source, "straightening null/null comparison\n");
    PhysReg rOut = getReg(i.outStack->location);
    bool resultIsTrue = (op == OpLte || op == OpGte);
    if (i.changesPC) {
      fuseBranchAfterStaticBool(t, i, resultIsTrue);
    } else {
      emitImmReg(a, resultIsTrue, rOut);
    }
    return;
  }
  PhysReg src, srcdest;
  getBinaryStackInputs(m_regMap, i, src, srcdest);
  ASSERT(getReg(i.outStack->location) == srcdest);
  if (i.changesPC) {
    fuseBranchSync(t, i);
  }
  a.       cmp_reg64_reg64(src, srcdest);
  static const ConditionCode opToCc[2][2] = {
    // !fEquals fEquals
    { CC_G,     CC_GE }, // !fLessThan
    { CC_L,     CC_LE }, // fLessThan
  };
  ConditionCode cc = opToCc[fLessThan][fEquals];
  if (i.changesPC) {
    // Fuse the coming branch.
    fuseBranchAfterBool(t, i, cc);
    return;
  }
  a.       setcc(cc, srcdest);
  a.       mov_reg8_reg64_unsigned(srcdest, srcdest);
}

static TXFlags
planUnaryBooleanOp(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  RuntimeType& rtt = i.inputs[0]->rtt;
  DataType inType = rtt.valueType();
  if (inType == KindOfArray) {
    return Supported;
  }
  if (rtt.isString()) {
    return Simple;
  }
  return nativePlan(rtt.isNull() ||
                    inType == KindOfBoolean || rtt.isInt());
}

void
TranslatorX64::analyzeUnaryBooleanOp(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = planUnaryBooleanOp(i);
}

void
TranslatorX64::translateUnaryBooleanOp(const Tracelet& t,
                                       const NormalizedInstruction& i) {
  const Opcode op = i.op();
  ASSERT(op == OpCastBool || op == OpEmptyL);
  const vector<DynLocation*>& inputs  = i.inputs;
  ASSERT(inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);
  bool instrNeg = (op == OpEmptyL);
  DataType inType = inputs[0]->valueType();
  const Location& inLoc = inputs[0]->location;
  bool boxedForm = (inputs[0]->outerType() == KindOfRef);


  switch (inType) {
    NULLCASE(): {
      m_regMap.allocOutputRegs(i);
      PhysReg outReg = getReg(i.outStack->location);
      emitImmReg(a, instrNeg, outReg);
      ASSERT(i.isNative());
    } break;
    case KindOfBoolean: {
      if (op == OpCastBool) {
        // Casting bool to bool is a nop.  CastBool's input must be
        // a cell on the stack as per the bytecode specification.
        ASSERT(inputs[0]->isStack());
        ASSERT(inputs[0]->outerType() != KindOfRef);
        ASSERT(inputs[0]->location.space == Location::Stack);
        ASSERT(i.isNative());
        break;
      }
      m_regMap.allocOutputRegs(i);
      PhysReg reg = getReg(inLoc);
      PhysReg outReg = getReg(i.outStack->location);
      if (boxedForm) {
        emitDeref(a, reg, outReg);
      } else {
        emitMovRegReg(reg, outReg);
      }
      if (instrNeg) {
        a.  xor_imm32_reg32(1, outReg);
      }
    } break;
    case KindOfInt64: {
      m_regMap.allocOutputRegs(i);
      PhysReg reg = getReg(inLoc);
      PhysReg outReg = getReg(i.outStack->location);
      ScratchReg scratch(m_regMap);
      if (boxedForm) {
        emitDeref(a, reg, *scratch);
        emitConvertToBool(a, *scratch, outReg, instrNeg);
      } else {
        emitConvertToBool(a, reg, outReg, instrNeg);
      }
    } break;
    STRINGCASE():
    case KindOfArray: {
      bool doDecRef = (inputs[0]->isStack());
      void* fptr = IS_STRING_TYPE(inType) ?
          (doDecRef ? (void*)str_to_bool : (void*)str0_to_bool) :
          (doDecRef ? (void*)arr_to_bool : (void*)arr0_to_bool);
      if (boxedForm) {
        EMIT_CALL(a, fptr, DEREF(inLoc));
      } else {
        EMIT_CALL(a, fptr, V(inLoc));
      }
      if (!IS_STRING_TYPE(inType)) {
        recordReentrantCall(i);
      }
      if (instrNeg) {
        a.    xor_imm32_reg32(1, rax);
      }
      m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                    RegInfo::DIRTY);
    } break;
    default: {
      ASSERT(false);
    } break;
  }
}

void
TranslatorX64::analyzeBranchOp(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = Supported;
}

// Helper for decoding dests of branch-like instructions at the end of
// a basic block.
static void branchDests(const Tracelet& t,
                        const NormalizedInstruction& i,
                        SrcKey* outTaken, SrcKey* outNotTaken,
                        int immIdx = 0) {
  *outNotTaken = nextSrcKey(t, i);
  int dest = i.imm[immIdx].u_BA;
  *outTaken = SrcKey(curFunc(), i.offset() + dest);
}

void TranslatorX64::branchWithFlagsSet(const Tracelet& t,
                                       const NormalizedInstruction& i,
                                       ConditionCode cc) {
  ASSERT(i.op() == OpJmpNZ || i.op() == OpJmpZ);
  // not_taken
  SrcKey taken, notTaken;
  branchDests(t, i, &taken, &notTaken);
  TRACE(3, "branchWithFlagsSet %d %d cc%02x jmp%sz\n",
        taken.offset(), notTaken.offset(), cc,
        i.isJmpNZ() ? "n" : "");
  emitCondJmp(taken, notTaken, cc);
}

void TranslatorX64::fuseBranchAfterStaticBool(const Tracelet& t,
                                              const NormalizedInstruction& i,
                                              bool resultIsTrue) {
  ASSERT(i.breaksBB);
  ASSERT(i.next);
  NormalizedInstruction &nexti = *i.next;
  fuseBranchSync(t, i);
  bool isTaken = (resultIsTrue == nexti.isJmpNZ());
  SrcKey taken, notTaken;
  branchDests(t, nexti, &taken, &notTaken);
  if (isTaken) {
    emitBindJmp(taken);
  } else {
    emitBindJmp(notTaken);
  }
}

void TranslatorX64::fuseBranchSync(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  // Don't bother sync'ing the output of this instruction.
  m_regMap.scrubStackEntries(i.outStack->location.offset);
  syncOutputs(t);
}

void TranslatorX64::fuseBranchAfterBool(const Tracelet& t,
                                        const NormalizedInstruction& i,
                                        ConditionCode cc) {
  ASSERT(i.breaksBB);
  ASSERT(i.next);
  NormalizedInstruction &nexti = *i.next;
  if (!i.next->isJmpNZ()) cc = ccNegate(cc);
  branchWithFlagsSet(t, nexti, cc);
}

void
TranslatorX64::translateBranchOp(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  bool isZ = !i.isJmpNZ();
  ASSERT(i.inputs.size()  == 1);
  ASSERT(!i.outStack && !i.outLocal);
  m_regMap.allocOutputRegs(i);
  const DynLocation& in = *i.inputs[0];
  const RuntimeType& rtt = in.rtt;
  const Location& inLoc = in.location;
  DataType inputType = rtt.outerType();
  PhysReg src = getReg(inLoc);
  /*
   * Careful. We're operating with intimate knowledge of the
   * constraints of the register allocator from here out.
   */
  if (rtt.isString() || inputType == KindOfArray) {
    // str_to_bool and arr_to_bool will decRef for us
    void* fptr = IS_STRING_TYPE(inputType) ? (void*)str_to_bool :
                                               (void*)arr_to_bool;
    EMIT_CALL(a, fptr, V(inLoc));
    src = rax;
    ScratchReg sr(m_regMap, rax);
    syncOutputs(t);
  } else if (inputType != KindOfUninit &&
             inputType != KindOfNull &&
             inputType != KindOfBoolean &&
             !rtt.isInt()) {
    // input might be in-flight
    m_regMap.cleanRegs(RegSet(m_regMap.getReg(inLoc)));
    // Cast to a bool.
    if (false) {
      TypedValue *tv = NULL;
      int64 ret = tv_to_bool(tv);
      if (ret) {
        printf("zoot");
      }
    }
    TRACE(2, Trace::prettyNode("tv_to_bool", inLoc) + string("\n"));
    // tv_to_bool will decRef for us if appropriate
    EMIT_CALL(a, tv_to_bool, A(inLoc));
    recordReentrantCall(i);
    src = rax;
    ScratchReg sr(m_regMap, rax);
    syncOutputs(t);
  } else {
    syncOutputs(t);
  }

  // not_taken
  SrcKey taken, notTaken;
  branchDests(t, i, &taken, &notTaken);

  // Since null always evaluates to false, we can emit an
  // unconditional jump. OpJmpNZ will never take the branch
  // while OpJmpZ will always take the branch.
  if (IS_NULL_TYPE(inputType)) {
    TRACE(1, "branch on Null -> always Z\n");
    emitBindJmp(isZ ? taken : notTaken);
    return;
  }
  a.    test_reg64_reg64(src, src);
  branchWithFlagsSet(t, i, isZ ? CC_Z : CC_NZ);
}

void
TranslatorX64::analyzeCGetL(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  const RuntimeType& type = i.inputs[0]->rtt;
  i.m_txFlags = type.isUninit() ? Supported : Native;
}

static const StringData* local_name(const Location& l) {
  ASSERT(l.isLocal());
  const StringData* ret = curFunc()->localNames()[l.offset];
  ASSERT(ret->isStatic());
  return ret;
}

static void raiseUndefVariable(StringData* nm) {
  raise_notice(Strings::UNDEFINED_VARIABLE, nm->data());
  // FIXME: do we need to decref the string if an exception is
  // propagating?
  if (nm->decRefCount() == 0) { nm->release(); }
}

void
TranslatorX64::translateCGetL(const Tracelet& t,
                              const NormalizedInstruction& i) {
  const DEBUG_ONLY Opcode op = i.op();
  ASSERT(op == OpFPassL || OpCGetL);
  const vector<DynLocation*>& inputs = i.inputs;
  ASSERT(inputs.size() == 1);
  ASSERT(inputs[0]->isLocal());
  DataType outType = i.inputs[0]->valueType();
  ASSERT(outType != KindOfInvalid);

  // Check for use of an undefined local.
  if (inputs[0]->rtt.isUninit()) {
    ASSERT(!i.outStack || i.outStack->outerType() == KindOfNull);
    outType = KindOfNull;
    ASSERT(inputs[0]->location.offset < curFunc()->numLocals());
    const StringData* name = local_name(inputs[0]->location);
    EMIT_CALL(a, raiseUndefVariable, IMM((uintptr_t)name));
    recordReentrantCall(i);
    if (i.outStack) {
      m_regMap.allocOutputRegs(i);
    }
    return;
  }

  /*
   * we can merge a CGetL with a following InstanceOfD
   * to avoid the incRef/decRef on the result (see
   * analyzeSecondPass).
   *
   * outStack will be clear in that case.
   */
  if (!i.outStack) return;
  ASSERT(outType == i.outStack->outerType());
  m_regMap.allocOutputRegs(i);
  if (IS_NULL_TYPE(outType)) return;
  PhysReg dest = getReg(i.outStack->location);

  if (i.manuallyAllocInputs && !m_regMap.hasReg(inputs[0]->location)) {
    fill(inputs[0]->location, dest);
  } else {
    PhysReg localReg = getReg(inputs[0]->location);
    emitMovRegReg(localReg, dest);
  }
  if (inputs[0]->isVariant()) {
    emitDeref(a, dest, dest);
  }
  ASSERT(outType != KindOfStaticString);
  emitIncRef(dest, outType);
}

void
TranslatorX64::analyzeCGetL2(Tracelet& t,
                             NormalizedInstruction& ni) {
  const int locIdx = 1;
  ASSERT(ni.inputs.size() == 2);
  ni.m_txFlags = ni.inputs[locIdx]->rtt.isUninit() ? Supported : Native;
}

void
TranslatorX64::translateCGetL2(const Tracelet& t,
                               const NormalizedInstruction& ni) {
  const int stackIdx = 0;
  const int locIdx   = 1;

  // Note: even if it's an undefined local we need to move a few
  // values around to have outputs end up in the right place.
  const bool undefinedLocal = ni.inputs[locIdx]->rtt.isUninit();

  if (undefinedLocal) {
    ASSERT(ni.outStack->valueType() == KindOfNull);
    ASSERT(ni.inputs[locIdx]->location.offset < curFunc()->numLocals());
    const StringData* name = local_name(ni.inputs[locIdx]->location);

    EMIT_CALL(a, raiseUndefVariable, IMM((uintptr_t)name));
    recordReentrantCall(ni);

    m_regMap.allocInputRegs(ni);
  }

  m_regMap.allocOutputRegs(ni);
  const PhysReg stackIn  = getReg(ni.inputs[stackIdx]->location);
  const PhysReg localIn  = getReg(ni.inputs[locIdx]->location);
  const PhysReg stackOut = getReg(ni.outStack2->location);
  ASSERT(ni.inputs[stackIdx]->location.isStack());
  ASSERT(ni.inputs[locIdx]->location.isLocal());

  /*
   * These registers overlap a bit, so we can swap a few bindings to
   * avoid a move.
   */
  ASSERT(stackIn == getReg(ni.outStack->location) && localIn != stackOut);
  m_regMap.swapRegisters(stackIn, stackOut);
  const PhysReg cellOut = getReg(ni.outStack->location);
  ASSERT(cellOut != stackIn);
  if (ni.inputs[locIdx]->isVariant()) {
    emitDeref(a, localIn, cellOut);
  } else if (!undefinedLocal) {
    emitMovRegReg(localIn, cellOut);
  }
  emitIncRef(cellOut, ni.inputs[locIdx]->valueType());
}

void
TranslatorX64::analyzeVGetL(Tracelet& t,
                            NormalizedInstruction& i) {
  i.m_txFlags = Native;
}

void
TranslatorX64::translateVGetL(const Tracelet& t,
                              const NormalizedInstruction& i) {
  const DEBUG_ONLY Opcode op = i.op();
  ASSERT(op == OpVGetL || op == OpFPassL);
  const vector<DynLocation*>& inputs = i.inputs;
  ASSERT(inputs.size() == 1);
  ASSERT(i.outStack);
  ASSERT(inputs[0]->isLocal());
  ASSERT(i.outStack->rtt.outerType() == KindOfRef);

  PhysReg localReg = getReg(inputs[0]->location);
  PhysReg dest;
  if (inputs[0]->rtt.outerType() != KindOfRef) {
    emitBox(inputs[0]->rtt.outerType(), localReg);
    m_regMap.bind(rax, inputs[0]->location, KindOfRef,
                  RegInfo::DIRTY);
    m_regMap.allocOutputRegs(i);
    dest = getReg(i.outStack->location);
    emitMovRegReg(rax, dest);
  } else {
    m_regMap.allocOutputRegs(i);
    dest = getReg(i.outStack->location);
    emitMovRegReg(localReg, dest);
  }
  emitIncRef(dest, KindOfRef);
}

void
TranslatorX64::analyzeAssignToLocalOp(Tracelet& t,
                                      NormalizedInstruction& ni) {
  const int locIdx = 1;
  ni.m_txFlags = planHingesOnRefcounting(ni.inputs[locIdx]->outerType());
}

void
TranslatorX64::translateAssignToLocalOp(const Tracelet& t,
                                        const NormalizedInstruction& ni) {
  const int rhsIdx  = 0;
  const int locIdx  = 1;
  const Opcode op = ni.op();
  ASSERT(op == OpSetL || op == OpBindL);
  ASSERT(ni.inputs.size() == 2);
  ASSERT((op == OpBindL) ==
         (ni.inputs[rhsIdx]->outerType() == KindOfRef));

  ASSERT(!ni.outStack || ni.inputs[locIdx]->location != ni.outStack->location);
  ASSERT(ni.outLocal);
  ASSERT(ni.inputs[locIdx]->location == ni.outLocal->location);
  ASSERT(ni.inputs[rhsIdx]->isStack());

  m_regMap.allocOutputRegs(ni);
  const PhysReg rhsReg        = getReg(ni.inputs[rhsIdx]->location);
  const PhysReg localReg      = getReg(ni.outLocal->location);
  const DataType oldLocalType = ni.inputs[locIdx]->outerType();
  const DataType rhsType      = ni.inputs[rhsIdx]->outerType();
  ASSERT(localReg != rhsReg);

  LazyScratchReg oldLocalReg(m_regMap);
  DataType decRefType;

  // For SetL, when the local is boxed, we need to change the
  // type/value of the inner cell.  If we're doing BindL, we don't
  // want to affect the old inner cell in any case (except to decref
  // it).
  const bool affectInnerCell = op == OpSetL &&
                               oldLocalType == KindOfRef;
  if (affectInnerCell) {
    ASSERT(rhsType != KindOfRef);

    oldLocalReg.alloc();
    emitDeref(a, localReg, *oldLocalReg);
    emitTypedValueStore(a, rhsType, rhsReg, 0, localReg);
    decRefType = ni.inputs[locIdx]->rtt.innerType();
  } else {
    /*
     * Instead of emitting a mov, just swap the locations these two
     * registers are mapped to.
     *
     * TODO: this might not be the best idea now that the register
     * allocator has some awareness about what is a local.  (Maybe we
     * should just xchg_reg64_reg64.)
     */
    m_regMap.swapRegisters(rhsReg, localReg);
    decRefType = oldLocalType;
  }

  // If we're giving stack output, it's important to incref before
  // calling a possible destructor, since the destructor could have
  // access to the local if it is a var.
  if (ni.outStack) {
    emitIncRef(rhsReg, rhsType);
  } else {
    SKTRACE(3, ni.source, "hoisting Pop* into current instr\n");
  }

  emitDecRef(ni, oldLocalReg.isAllocated() ? *oldLocalReg : localReg,
    decRefType);

  if (ni.outStack && !IS_NULL_TYPE(ni.outStack->outerType())) {
    PhysReg stackReg = getReg(ni.outStack->location);
    emitMovRegReg(rhsReg, stackReg);
  }
}

static void
planPop(NormalizedInstruction& i) {
  if (i.prev && i.prev->outputPredicted) {
    i.prev->outputPredicted = false;
    i.inputs[0]->rtt = RuntimeType(KindOfInvalid);
  }
  DataType type = i.inputs[0]->outerType();
  i.m_txFlags =
    (type == KindOfInvalid || IS_REFCOUNTED_TYPE(type)) ? Supported : Native;
  i.manuallyAllocInputs = true;
}

void TranslatorX64::analyzePopC(Tracelet& t, NormalizedInstruction& i) {
  planPop(i);
}

void TranslatorX64::analyzePopV(Tracelet& t, NormalizedInstruction& i) {
  planPop(i);
}

void TranslatorX64::analyzePopR(Tracelet& t, NormalizedInstruction& i) {
  planPop(i);
}

void
TranslatorX64::translatePopC(const Tracelet& t,
                             const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(!i.outStack && !i.outLocal);
  if (i.inputs[0]->rtt.isVagueValue()) {
    PhysReg base;
    int disp;
    locToRegDisp(i.inputs[0]->location, &base, &disp);
    emitDecRefGeneric(i, base, disp);
  } else {
    emitDecRefInput(a, i, 0);
  }
}

void
TranslatorX64::translatePopV(const Tracelet& t,
                             const NormalizedInstruction& i) {
  ASSERT(i.inputs[0]->rtt.isVagueValue() ||
         i.inputs[0]->isVariant());
  translatePopC(t, i);
}

void
TranslatorX64::translatePopR(const Tracelet& t,
                             const NormalizedInstruction& i) {
  translatePopC(t, i);
}

void
TranslatorX64::translateUnboxR(const Tracelet& t,
                               const NormalizedInstruction& i) {
  // If the value on the top of a stack is a var, unbox it and
  // leave it on the top of the stack.
  if (i.inputs[0]->isVariant()) {
    emitUnboxTopOfStack(i);
  }
}

void
TranslatorX64::translateNull(const Tracelet& t,
                             const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outLocal);
  if (i.outStack) {
    ASSERT(i.outStack->outerType() == KindOfNull);

    // We have to mark the output register as dirty to ensure that
    // the type gets spilled at the end of the tracelet
    m_regMap.allocOutputRegs(i);
  }
  /* nop */
}

void
TranslatorX64::translateTrue(const Tracelet& t,
                             const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outLocal);
  if (i.outStack) {
    m_regMap.allocOutputRegs(i);
    PhysReg rdest = getReg(i.outStack->location);
    emitImmReg(a, 1, rdest);
  }
}

void
TranslatorX64::translateFalse(const Tracelet& t,
                              const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outLocal);
  if (i.outStack) {
    m_regMap.allocOutputRegs(i);
    PhysReg dest = getReg(i.outStack->location);
    emitImmReg(a, false, dest);
  }
}

void
TranslatorX64::translateInt(const Tracelet& t,
                            const NormalizedInstruction& i) {
  ASSERT(i.inputs.size()  == 0);
  ASSERT(!i.outLocal);
  if (i.outStack) {
    ASSERT(i.outStack->isInt());
    m_regMap.allocOutputRegs(i);
    PhysReg dest = getReg(i.outStack->location);
    uint64_t srcImm = i.imm[0].u_I64A;
    emitImmReg(a, srcImm, dest);
  }
}

void
TranslatorX64::translateString(const Tracelet& t,
                               const NormalizedInstruction& i) {
  ASSERT(i.inputs.size()  == 0);
  ASSERT(!i.outLocal);
  if (!i.outStack) return;
  ASSERT(Translator::typeIsString(i.outStack->outerType()));
  m_regMap.allocOutputRegs(i);
  PhysReg dest = getReg(i.outStack->location);
  uint64_t srcImm = (uintptr_t)curUnit()->lookupLitstrId(i.imm[0].u_SA);
  // XXX: can simplify the lookup here by just fishing it out of the
  // output's valueString().
  // We are guaranteed that the string is static, so we do not need to
  // increment the refcount
  ASSERT(((StringData*)srcImm)->isStatic());
  SKTRACE(2, i.source, "Litstr %d -> %p \"%s\"\n",
      i.imm[0].u_SA, (StringData*)srcImm,
      Util::escapeStringForCPP(((StringData*)srcImm)->data()).c_str());
  emitImmReg(a, srcImm, dest);
}

void
TranslatorX64::translateArray(const Tracelet& t,
                              const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outLocal);
  if (i.outStack) {
    ASSERT(i.outStack->outerType() == KindOfArray);
    m_regMap.allocOutputRegs(i);
    ArrayData* ad = curUnit()->lookupArrayId(i.imm[0].u_AA);
    PhysReg r = getReg(i.outStack->location);
    emitImmReg(a, uint64(ad), r);
    // We are guaranteed that the array is static, so we do not need to
    // increment the refcount
    ASSERT(ad->isStatic());
  }
}

void
TranslatorX64::translateNewArray(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.outStack->outerType() == KindOfArray);
  m_regMap.allocOutputRegs(i);
  PhysReg r = getReg(i.outStack->location);
  emitImmReg(a, uint64(HphpArray::GetStaticEmptyArray()), r);
  // We are guaranteed that the new array is static, so we do not need to
  // increment the refcount
  ASSERT(HphpArray::GetStaticEmptyArray()->isStatic());
}

void
TranslatorX64::analyzeNop(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = Native;
}

void
TranslatorX64::translateNop(const Tracelet& t,
                            const NormalizedInstruction& i) {
}

void
TranslatorX64::analyzeAddElemC(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(i.inputs[2]->outerType() == KindOfArray &&
                              (i.inputs[1]->isInt() ||
                               i.inputs[1]->isString()));
}

void
TranslatorX64::translateAddElemC(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.inputs.size() >= 3);
  const DynLocation& arr = *i.inputs[2];
  const DynLocation& key = *i.inputs[1];
  const DynLocation& val = *i.inputs[0];
  ASSERT(!arr.isVariant()); // not handling variants.
  ASSERT(!key.isVariant());
  ASSERT(!val.isVariant());

  const Location& arrLoc = arr.location;
  const Location& keyLoc = key.location;
  const Location& valLoc = val.location;

  ASSERT(arrLoc.isStack());
  ASSERT(keyLoc.isStack());
  ASSERT(arrLoc.isStack());

  // If either the key or the rhs is not Int64, we will need to pass the
  // rhs by address, so we need to sync it back to memory
  if (!key.rtt.isInt() || !val.rtt.isInt()) {
    m_regMap.cleanRegs(RegSet(m_regMap.getReg(valLoc)));
  }

  // The array_setm helpers will decRef any old value that is
  // overwritten if appropriate. If copy-on-write occurs, it will also
  // incRef the new array and decRef the old array for us. Finally,
  // some of the array_setm helpers will decRef the key if it is a
  // string (for cases where the key is not a local), while others do
  // not (for cases where the key is a local).
  void* fptr;
  if (key.rtt.isInt() && val.rtt.isInt()) {
    if (false) { // type-check
      TypedValue* cell = NULL;
      ArrayData* arr = NULL;
      ArrayData* ret = array_setm_ik1_iv(cell, arr, 12, 3);
      printf("%p", ret); // use ret
    }
    // If the rhs is Int64, we can use a specialized helper
    EMIT_CALL(a, array_setm_ik1_iv,
               IMM(0),
               V(arrLoc),
               V(keyLoc),
               V(valLoc));
    recordReentrantCall(i);
  } else if (key.rtt.isInt() || key.rtt.isString()) {
    if (false) { // type-check
      TypedValue* cell = NULL;
      TypedValue* rhs = NULL;
      StringData* strkey = NULL;
      ArrayData* arr = NULL;
      ArrayData* ret;
      ret = array_setm_ik1_v0(cell, arr, 12, rhs);
      printf("%p", ret); // use ret
      ret = array_setm_sk1_v0(cell, arr, strkey, rhs);
      printf("%p", ret); // use ret
    }
    // Otherwise, we pass the rhs by address
    fptr = key.rtt.isString() ? (void*)array_setm_sk1_v0 :
      (void*)array_setm_ik1_v0;
    EMIT_CALL(a, fptr,
               IMM(0),
               V(arrLoc),
               V(keyLoc),
               A(valLoc));
    recordReentrantCall(i);
  } else {
    ASSERT(false);
  }
  // The array value may have changed, so we need to invalidate any
  // register we have associated with arrLoc
  m_regMap.invalidate(arrLoc);
  // The array_setm helper returns the up-to-date array pointer in rax.
  // Therefore, we can bind rax to arrLoc and mark it as dirty.
  m_regMap.bind(rax, arrLoc, KindOfArray, RegInfo::DIRTY);
}

void
TranslatorX64::analyzeAddNewElemC(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  i.m_txFlags = supportedPlan(i.inputs[1]->outerType() == KindOfArray);
}

void
TranslatorX64::translateAddNewElemC(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.inputs[0]->outerType() != KindOfRef);
  ASSERT(i.inputs[1]->outerType() != KindOfRef);
  ASSERT(i.inputs[0]->isStack());
  ASSERT(i.inputs[1]->isStack());

  Location arrLoc = i.inputs[1]->location;
  Location valLoc = i.inputs[0]->location;

  // We pass the rhs by address, so we need to sync it back to memory
  m_regMap.cleanRegs(RegSet(m_regMap.getReg(valLoc)));

  // The array_setm helpers will decRef any old value that is
  // overwritten if appropriate. If copy-on-write occurs, it will also
  // incRef the new array and decRef the old array for us. Finally,
  // some of the array_setm helpers will decRef the key if it is a
  // string (for cases where the key is not a local), while others do
  // not (for cases where the key is a local).
  if (false) { // type-check
    TypedValue* cell = NULL;
    TypedValue* rhs = NULL;
    ArrayData* arr = NULL;
    ArrayData* ret;
    ret = array_setm_wk1_v0(cell, arr, rhs);
    printf("%p", ret); // use ret
  }
  EMIT_CALL(a, array_setm_wk1_v0,
             IMM(0),
             V(arrLoc),
             A(valLoc));
  recordReentrantCall(i);
  // The array value may have changed, so we need to invalidate any
  // register we have associated with arrLoc
  m_regMap.invalidate(arrLoc);
  // The array_setm helper returns the up-to-date array pointer in rax.
  // Therefore, we can bind rax to arrLoc and mark it as dirty.
  m_regMap.bind(rax, arrLoc, KindOfArray, RegInfo::DIRTY);
}

static void undefCns(const StringData* nm) {
  VMRegAnchor _;
  TypedValue *cns = g_vmContext->getCns(const_cast<StringData*>(nm));
  if (!cns) {
    raise_notice(Strings::UNDEFINED_CONSTANT, nm->data(), nm->data());
    g_vmContext->getStack().pushStringNoRc(const_cast<StringData*>(nm));
  } else {
    Cell* c1 = g_vmContext->getStack().allocC();
    TV_READ_CELL(cns, c1);
  }
}

void TranslatorX64::emitSideExit(Asm& a, const NormalizedInstruction& i,
                                 bool next) {
  const NormalizedInstruction& dest = next ? *i.next : i;
  SKTRACE(3, i.source, "sideexit check %p\n", a.code.frontier);
  m_regMap.scrubStackEntries(dest.stackOff);
  m_regMap.cleanAll();
  emitRB(a, RBTypeSideExit, i.source);
  int stackDisp = dest.stackOff;
  if (stackDisp != 0) {
    SKTRACE(3, i.source, "stack bump %d => %x\n", stackDisp,
            -cellsToBytes(stackDisp));
    a.   add_imm32_reg64(-cellsToBytes(stackDisp), rVmSp);
  }
  emitBindJmp(a, dest.source, REQ_BIND_SIDE_EXIT);
}

void
TranslatorX64::translateCns(const Tracelet& t,
                            const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(i.outStack && !i.outLocal);

  // OK to burn "name" into TC: it was merged into the static string
  // table, so as long as this code is reachable, so shoud the string
  // be.
  DataType outType = i.outStack->valueType();
  StringData* name = curUnit()->lookupLitstrId(i.imm[0].u_SA);
  const TypedValue* tv = g_vmContext->getCns(name, true, false);
  bool checkDefined = false;
  if (outType != KindOfInvalid && tv == NULL &&
      !RuntimeOption::RepoAuthoritative) {
    PreConstDepMap::accessor acc;
    tv = findUniquePreConst(acc, name);
    if (tv != NULL) {
      checkDefined = true;
      acc->second.srcKeys.insert(t.m_sk);
      Stats::emitInc(a, Stats::Tx64_CnsFast);
    } else {
      // We had a unique value while analyzing but don't anymore. This
      // should be rare so just punt to keep things simple.
      punt();
    }
  }
  using namespace TargetCache;
  if (tv && tvIsStatic(tv)) {
    m_regMap.allocOutputRegs(i);
    if (checkDefined) {
      size_t bit = allocCnsBit(name);
      uint32 mask;
      CacheHandle ch = bitOffToHandleAndMask(bit, mask);
      // The 'test' instruction takes a signed immediate and the mask is
      // unsigned, but everything works out okay because the immediate is
      // the same size as the other operand. However, we have to sign-extend
      // the mask to 64 bits to make the assembler happy.
      int64_t imm = (int64_t)(int32)mask;
      a.test_imm32_disp_reg32(imm, ch, rVmTl);
      {
        // If we get to the optimistic translation and the constant
        // isn't defined, our tracelet is ruined because the type may
        // not be what we expect. If we were expecting KindOfString we
        // could theoretically keep going here since that's the type
        // of an undefined constant expression, but it should be rare
        // enough that it's not worth the complexity.
        UnlikelyIfBlock<CC_Z> ifZero(a, astubs);
        Stats::emitInc(astubs, Stats::Tx64_CnsFast, -1);
        emitSideExit(astubs, i, false);
      }
    }
    // Its type and value are known at compile-time.
    ASSERT(tv->m_type == outType ||
           (IS_STRING_TYPE(tv->m_type) && IS_STRING_TYPE(outType)));
    PhysReg r = getReg(i.outStack->location);
    a.   mov_imm64_reg(tv->m_data.num, r);
    emitIncRef(r, tv->m_type);
    return;
  }

  Stats::emitInc(a, Stats::Tx64_CnsSlow);
  CacheHandle ch = allocConstant(name);
  TRACE(2, "Cns: %s -> ch %ld\n", name->data(), ch);
  // Load the constant out of the thread-private tl_targetCaches.
  ScratchReg cns(m_regMap);
  a.    lea_reg64_disp_reg64(rVmTl, ch, *cns);
  a.    cmp_imm32_disp_reg32(0, TVOFF(m_type), *cns);
  DiamondReturn astubsRet;
  int stackDest = 0 - int(sizeof(Cell)); // popped - pushed
  {
    // It's tempting to dedup these, but not obvious we really can;
    // at least stackDest and tmp are specific to the translation
    // context.
    UnlikelyIfBlock<CC_Z> ifb(a, astubs, &astubsRet);
    EMIT_CALL(astubs, undefCns, IMM((uintptr_t)name));
    recordReentrantStubCall(i);
    m_regMap.invalidate(i.outStack->location);
  }

  // Bitwise copy to output area.
  emitCopyToStack(a, i, *cns, stackDest);
  m_regMap.invalidate(i.outStack->location);
}

void
TranslatorX64::analyzeDefCns(Tracelet& t,
                             NormalizedInstruction& i) {
  StringData* name = curUnit()->lookupLitstrId(i.imm[0].u_SA);
  /* dont bother to translate if it names a builtin constant */
  i.m_txFlags = supportedPlan(!g_vmContext->getCns(name, true, false));
}

typedef void (*defCnsHelper_func_t)(TargetCache::CacheHandle ch, Variant *inout,
                                    StringData *name, size_t bit);
template<bool setBit>
static void defCnsHelper(TargetCache::CacheHandle ch, Variant *inout,
                         StringData *name, size_t bit) {
  using namespace TargetCache;
  TypedValue *tv = (TypedValue*)handleToPtr(ch);
  if (LIKELY(tv->m_type == KindOfUninit &&
             inout->isAllowedAsConstantValue())) {
    inout->setEvalScalar();
    if (LIKELY(g_vmContext->m_constants.nvInsert(name, (TypedValue*)inout))) {
      tvDup((TypedValue*)inout, tv);
      *inout = true;
      if (setBit) {
        DEBUG_ONLY bool alreadyDefined = testAndSetBit(bit);
        ASSERT(!alreadyDefined);
      }
      return;
    }
    tv = (TypedValue*)&false_varNR;
  }

  if (tv->m_type != KindOfUninit) {
    raise_warning(Strings::CONSTANT_ALREADY_DEFINED, name->data());
  } else {
    ASSERT(!inout->isAllowedAsConstantValue());
    raise_warning(Strings::CONSTANTS_MUST_BE_SCALAR);
  }
  *inout = false;
}

void
TranslatorX64::translateDefCns(const Tracelet& t,
                               const NormalizedInstruction& i) {
  StringData* name = curUnit()->lookupLitstrId(i.imm[0].u_SA);

  if (false) {
    TargetCache::CacheHandle ch;
    size_t bit = 0;
    Variant *inout = 0;
    StringData *name = 0;
    defCnsHelper<true>(ch, inout, name, bit);
    defCnsHelper<false>(ch, inout, name, bit);
  }

  using namespace TargetCache;
  CacheHandle ch = allocConstant(name);
  TRACE(2, "DefCns: %s -> ch %ld\n", name->data(), ch);

  m_regMap.cleanLoc(i.inputs[0]->location);
  if (RuntimeOption::RepoAuthoritative) {
    EMIT_CALL(a, (defCnsHelper_func_t)defCnsHelper<false>,
               IMM(ch), A(i.inputs[0]->location),
               IMM((uint64)name));
  } else {
    EMIT_CALL(a, (defCnsHelper_func_t)defCnsHelper<true>,
               IMM(ch), A(i.inputs[0]->location),
               IMM((uint64)name), IMM(allocCnsBit(name)));
  }
  recordReentrantCall(i);
  m_regMap.invalidate(i.outStack->location);
}

void
TranslatorX64::translateClsCnsD(const Tracelet& t,
                                const NormalizedInstruction& i) {
  using namespace TargetCache;
  const NamedEntityPair& namedEntityPair =
    curUnit()->lookupNamedEntityPairId(i.imm[1].u_SA);
  ASSERT(namedEntityPair.second);
  const StringData *clsName = namedEntityPair.first;
  ASSERT(clsName->isStatic());
  StringData* cnsName = curUnit()->lookupLitstrId(i.imm[0].u_SA);
  ASSERT(cnsName->isStatic());
  StringData* fullName = StringData::GetStaticString(
    Util::toLower(clsName->data()) + "::" + cnsName->data());

  Stats::emitInc(a, Stats::TgtCache_ClsCnsHit);
  CacheHandle ch = allocClassConstant(fullName);
  ScratchReg cns(m_regMap);
  a.lea_reg64_disp_reg64(rVmTl, ch, *cns);
  a.cmp_imm32_disp_reg32(0, TVOFF(m_type), *cns);
  {
    UnlikelyIfBlock<CC_Z> ifNull(a, astubs);

    if (false) { // typecheck
      TypedValue* tv = NULL;
      UNUSED TypedValue* ret =
        TargetCache::lookupClassConstant(tv, namedEntityPair.second,
                                         namedEntityPair.first, cnsName);
    }

    EMIT_CALL(astubs, TCA(TargetCache::lookupClassConstant),
              R(*cns),
              IMM(uintptr_t(namedEntityPair.second)),
              IMM(uintptr_t(namedEntityPair.first)),
              IMM(uintptr_t(cnsName)));
    recordReentrantStubCall(i);
    // DiamondGuard will restore cns's SCRATCH state but not its
    // contents. lookupClassConstant returns the value we want.
    emitMovRegReg(astubs, rax, *cns);
  }
  int stackDest = 0 - int(sizeof(Cell)); // 0 popped - 1 pushed
  emitCopyToStack(a, i, *cns, stackDest);
}

void
TranslatorX64::analyzeConcat(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  const RuntimeType& r = i.inputs[0]->rtt;
  const RuntimeType& l = i.inputs[1]->rtt;
  // The concat translation isn't reentrant; objects that override
  // __toString() can cause reentry.
  i.m_txFlags = simplePlan(r.valueType() != KindOfObject &&
                           l.valueType() != KindOfObject);
}

void
TranslatorX64::translateConcat(const Tracelet& t,
                               const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  const DynLocation& r = *i.inputs[0];
  const DynLocation& l = *i.inputs[1];
  // We have specialized helpers for concatenating two strings, a
  // string and an int, and an int an a string.
  void* fptr = NULL;
  if (l.rtt.isString() && r.rtt.isString()) {
    fptr = (void*)concat_ss;
  } else if (l.rtt.isString() && r.rtt.isInt()) {
    fptr = (void*)concat_si;
  } else if (l.rtt.isInt() && r.rtt.isString()) {
    fptr = (void*)concat_is;
  }
  if (fptr) {
    // If we have a specialized helper, use it
    if (false) { // type check
      StringData* v1 = NULL;
      StringData* v2 = NULL;
      StringData* retval = concat_ss(v1, v2);
      printf("%p", retval); // use retval
    }

    // The concat helper will decRef the inputs and incRef the output
    // for us if appropriate
    EMIT_CALL(a, fptr,
               V(l.location),
               V(r.location));
    ASSERT(i.outStack->rtt.isString());
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);

  } else {
    // Otherwise, use the generic concat helper
    if (false) { // type check
      uint64_t v1, v2;
      DataType t1, t2;
      StringData *retval = concat(t1, v1, t2, v2);
      printf("%p", retval); // use retval
    }
    // concat will decRef the two inputs and incRef the output
    // for us if appropriate
    EMIT_CALL(a, concat,
               IMM(l.valueType()), V(l.location),
               IMM(r.valueType()), V(r.location));
    ASSERT(i.outStack->isString());
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
  }
}

static TXFlags
planInstrAdd_Int(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  return nativePlan(i.inputs[0]->isInt() && i.inputs[1]->isInt());
}

static TXFlags
planInstrAdd_Array(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  return supportedPlan(i.inputs[0]->valueType() == KindOfArray &&
                       i.inputs[1]->valueType() == KindOfArray);
}

void
TranslatorX64::analyzeAdd(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = TXFlags(planInstrAdd_Int(i) | planInstrAdd_Array(i));
}

void
TranslatorX64::translateAdd(const Tracelet& t,
                            const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);

  if (planInstrAdd_Array(i)) {
    // Handle adding two arrays
    ASSERT(i.outStack->outerType() == KindOfArray);
    if (false) { // type check
      ArrayData* v = NULL;
      v = array_add(v, v);
    }
    // The array_add helper will decRef the inputs and incRef the output
    // for us if appropriate
    EMIT_CALL(a, array_add,
               V(i.inputs[1]->location),
               V(i.inputs[0]->location));
    recordReentrantCall(i);
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
    return;
  }

  ASSERT(planInstrAdd_Int(i));
  binaryArithCell(i, OpAdd, *i.inputs[0], *i.outStack);
}

void
TranslatorX64::analyzeXor(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = nativePlan((i.inputs[0]->outerType() == KindOfBoolean ||
                            i.inputs[0]->isInt()) &&
                           (i.inputs[1]->outerType() == KindOfBoolean ||
                            i.inputs[1]->isInt()));
}

static inline void
emitIntToCCBool(X64Assembler &a, PhysReg srcdest, PhysReg scratch,
                int CC) {
  /*
   *    test    %srcdest, %srcdest
   *    set<CC> %scratchL
   *    movzbq  %scratchL, %srcdest
   */
  a.   test_reg64_reg64(srcdest, srcdest);
  a.   setcc           (CC, scratch);
  a.   mov_reg8_reg64_unsigned(scratch, srcdest);
}

static inline void
emitIntToBool(X64Assembler &a, PhysReg srcdest, PhysReg scratch) {
  emitIntToCCBool(a, srcdest, scratch, CC_NZ);
}

static inline void
emitIntToNegBool(X64Assembler &a, PhysReg srcdest, PhysReg scratch) {
  emitIntToCCBool(a, srcdest, scratch, CC_Z);
}

void
TranslatorX64::translateXor(const Tracelet& t,
                            const NormalizedInstruction& i) {
  PhysReg src, srcdest;
  getBinaryStackInputs(m_regMap, i, src, srcdest);
  m_regMap.allocOutputRegs(i);
  ScratchReg scr(m_regMap);
  if (i.inputs[0]->isInt()) {
    emitIntToBool(a, src, *scr);
  }
  if (i.inputs[1]->isInt()) {
    emitIntToBool(a, srcdest, *scr);
  }
  a.    xor_reg64_reg64(src, srcdest);
}

void
TranslatorX64::analyzeNot(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  i.m_txFlags = nativePlan(i.inputs[0]->isInt() ||
                           i.inputs[0]->outerType() == KindOfBoolean);
}

void
TranslatorX64::translateNot(const Tracelet& t,
                            const NormalizedInstruction& i) {
  ASSERT(i.isNative());
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(!i.inputs[0]->isVariant());
  m_regMap.allocOutputRegs(i);
  PhysReg srcdest = m_regMap.getReg(i.outStack->location);
  ScratchReg scr(m_regMap);
  emitIntToNegBool(a, srcdest, *scr);
}

void
TranslatorX64::analyzeBitNot(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = nativePlan(i.inputs[0]->isInt());
}

void
TranslatorX64::translateBitNot(const Tracelet& t,
                                    const NormalizedInstruction& i) {
  ASSERT(i.outStack && !i.outLocal);
  m_regMap.allocOutputRegs(i);
  PhysReg srcdest = m_regMap.getReg(i.outStack->location);
  a.   not_reg64(srcdest);
}

void
TranslatorX64::analyzeCastInt(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = nativePlan(i.inputs[0]->isInt());
}

void
TranslatorX64::translateCastInt(const Tracelet& t,
                                const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);

  /* nop */
}

void
TranslatorX64::analyzeCastString(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags =
    i.inputs[0]->isArray() || i.inputs[0]->isObject() ? Supported :
    i.inputs[0]->isInt() ? Simple :
    Native;
  i.funcd = NULL;
}

static void toStringError(StringData *cls) {
  raise_error("Method __toString() must return a string value");
}

static const StringData* stringDataFromInt(int64 n) {
  StringData* s = buildStringData(n);
  s->incRefCount();
  return s;
}

static const StringData* stringDataFromDouble(int64 n) {
  StringData* s = buildStringData(*(double*)&n);
  s->incRefCount();
  return s;
}

void TranslatorX64::toStringHelper(ObjectData *obj) {
  // caller must set r15 to the new ActRec
  register ActRec *ar asm("r15");
  register ActRec *rbp asm("rbp");

  const Class* cls = obj->getVMClass();
  const Func* toString = cls->getToString();
  if (!toString) {
    // the unwinder will restore rVmSp to
    // &ar->m_r, so we'd better make sure its
    // got a valid TypedValue there.
    TV_WRITE_UNINIT(&ar->m_r);
    std::string msg = cls->preClass()->name()->data();
    msg += "::__toString() was not defined";
    throw BadTypeConversionException(msg.c_str());
  }
  // ar->m_savedRbp set by caller
  ar->m_savedRip = rbp->m_savedRip;
  ar->m_func = toString;
  // ar->m_soff set by caller
  ar->initNumArgs(0);
  ar->setThis(obj);
  ar->setVarEnv(0);
  // point our return address at the prolog to execute
  rbp->m_savedRip = (uint64_t)toString->getPrologue(0);
}

void
TranslatorX64::translateCastString(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);

  if (i.inputs[0]->isNull()) {
    m_regMap.allocOutputRegs(i);
    PhysReg dest = m_regMap.getReg(i.outStack->location);
    a.   mov_imm64_reg((uint64)empty_string.get(), dest);
  } else if (i.inputs[0]->isBoolean()) {
    static StringData* s_1 = StringData::GetStaticString("1");
    m_regMap.allocOutputRegs(i);
    PhysReg dest = m_regMap.getReg(i.outStack->location);
    a.   cmp_imm32_reg64(0, dest);
    a.   mov_imm64_reg((uint64)empty_string.get(), dest);
    ScratchReg scratch(m_regMap);
    a.   mov_imm64_reg((intptr_t)s_1, *scratch);
    a.   cmov_reg64_reg64(CC_NZ, *scratch, dest);
  } else if (i.inputs[0]->isInt()) {
    EMIT_CALL(a, stringDataFromInt, V(i.inputs[0]->location));
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
  } else if (i.inputs[0]->isDouble()) {
    EMIT_CALL(a, stringDataFromDouble, V(i.inputs[0]->location));
    m_regMap.bind(rax, i.outStack->location, i.outStack->outerType(),
                  RegInfo::DIRTY);
  } else if (i.inputs[0]->isString()) {
    // nop
  } else if (i.inputs[0]->isArray()) {
    static StringData* s_array = StringData::GetStaticString("Array");
    m_regMap.allocOutputRegs(i);
    PhysReg dest = m_regMap.getReg(i.outStack->location);
    emitDecRef(i, dest, KindOfArray);
    a.   mov_imm64_reg((uint64)s_array, dest);
  } else if (i.inputs[0]->isObject()) {
    m_regMap.scrubStackEntries(i.stackOff - 1);
    m_regMap.cleanAll();
    int delta = i.stackOff + kNumActRecCells - 1;
    if (delta) {
      a. add_imm64_reg64(-cellsToBytes(delta), rVmSp);
    }
    a.   store_reg64_disp_reg64(rVmFp, AROFF(m_savedRbp), rVmSp);
    a.   store_imm32_disp_reg(nextSrcKey(t, i).offset() - curFunc()->base(),
                              AROFF(m_soff), rVmSp);
    PhysReg obj = m_regMap.getReg(i.inputs[0]->location);
    if (obj != argNumToRegName[0]) {
      a. mov_reg64_reg64(obj,  argNumToRegName[0]);
    }
    m_regMap.smashRegs(kAllRegs);
    a.   mov_reg64_reg64(rVmSp, r15);
    EMIT_CALL(a, TCA(toStringHelper));
    recordReentrantCall(i);
    if (i.stackOff != 0) {
      a. add_imm64_reg64(cellsToBytes(i.stackOff), rVmSp);
    }
    PhysReg base;
    int disp;
    locToRegDisp(i.outStack->location, &base, &disp);
    ScratchReg scratch(m_regMap);
    emitStringCheck(a, base, disp + TVOFF(m_type), *scratch);
    {
      UnlikelyIfBlock<CC_NZ> ifNotString(a, astubs);
      EMIT_CALL(astubs, toStringError, IMM(0));
      recordReentrantStubCall(i);
    }
  } else {
    NOT_REACHED();
  }
}

void
TranslatorX64::analyzePrint(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  const RuntimeType& rtt = i.inputs[0]->rtt;
  DataType type = rtt.outerType();
  i.m_txFlags = simplePlan(
    type == KindOfUninit ||
    type == KindOfNull ||
    type == KindOfBoolean ||
    rtt.isInt() ||
    rtt.isString());
}

void
TranslatorX64::translatePrint(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  const vector<DynLocation*>& inputs  = i.inputs;
  ASSERT(inputs.size()   == 1);
  ASSERT(!i.outLocal);
  ASSERT(!i.outStack || i.outStack->isInt());
  Location  loc = inputs[0]->location;
  DataType type = inputs[0]->outerType();
  switch (type) {
    STRINGCASE():       EMIT_CALL(a, print_string,  V(loc)); break;
    case KindOfInt64:   EMIT_CALL(a, print_int,     V(loc)); break;
    case KindOfBoolean: EMIT_CALL(a, print_boolean, V(loc)); break;
    NULLCASE():         /* do nothing */                   break;
    default: {
      // Translation is only supported for Null, Boolean, Int, and String
      ASSERT(false);
      break;
    }
  }
  m_regMap.allocOutputRegs(i);
  if (i.outStack) {
    PhysReg outReg = getReg(i.outStack->location);
    emitImmReg(a, 1, outReg);
  }
}

void
TranslatorX64::translateJmp(const Tracelet& t,
                            const NormalizedInstruction& i) {
  m_regMap.allocOutputRegs(i);
  syncOutputs(t);

  // Check the surprise page on all backwards jumps
  if (i.imm[0].u_BA < 0) {
    if (trustSigSegv) {
      const uint64_t stackMask =
        ~(cellsToBytes(RuntimeOption::EvalVMStackElms) - 1);
      a.mov_reg64_reg64(rVmSp, rScratch);
      a.and_imm64_reg64(stackMask, rScratch);
      TCA surpriseLoad = a.code.frontier;
      a.load_reg64_disp_reg64(rScratch, 0, rScratch);

      if (!m_segvStubs.insert(SignalStubMap::value_type(surpriseLoad,
                                                        astubs.code.frontier)))
        NOT_REACHED();
      /*
       * Note that it is safe not to register unwind information here,
       * because we just called syncOutputs so all registers are
       * already clean.
       */
      astubs.call((TCA)&EventHook::CheckSurprise);
      recordStubCall(i);
      astubs.jmp(a.code.frontier);
    } else {
      emitTestSurpriseFlags();
      {
        UnlikelyIfBlock<CC_NZ> ifSurprise(a, astubs);
        astubs.call((TCA)&EventHook::CheckSurprise);
        recordStubCall(i);
      }
    }
  }
  SrcKey sk(curFunc(), i.offset() + i.imm[0].u_BA);
  emitBindJmp(sk);
}

void
TranslatorX64::analyzeSwitch(Tracelet& t,
                             NormalizedInstruction& i) {
  RuntimeType& rtt = i.inputs[0]->rtt;
  ASSERT(rtt.outerType() != KindOfRef);
  switch (rtt.outerType()) {
    NULLCASE():
    case KindOfBoolean:
    case KindOfInt64:
      i.m_txFlags = Native;
      break;

    case KindOfDouble:
      i.m_txFlags = Simple;
      break;

    STRINGCASE():
    case KindOfObject:
    case KindOfArray:
      i.m_txFlags = Supported;
      break;

    default:
      not_reached();
  }
}

template <typename T>
static int64 switchBoundsCheck(T v, int64 base, int64 nTargets) {
  // I'm relying on gcc to be smart enough to optimize away the next
  // two lines when T is int64.
  if (int64(v) == v) {
    int64 ival = v;
    if (ival >= base && ival < (base + nTargets)) {
      return ival - base;
    }
  }
  return nTargets + 1;
}

static int64 switchDoubleHelper(int64 val, int64 base, int64 nTargets) {
  union {
    int64 intbits;
    double dblval;
  } u;
  u.intbits = val;
  return switchBoundsCheck(u.dblval, base, nTargets);
}

static int64 switchStringHelper(StringData* s, int64 base, int64 nTargets) {
  int64 ival;
  double dval;
  switch (s->isNumericWithVal(ival, dval, 1)) {
    case KindOfNull:
      ival = switchBoundsCheck(0, base, nTargets);
      break;

    case KindOfDouble:
      ival = switchBoundsCheck(dval, base, nTargets);
      break;

    case KindOfInt64:
      ival = switchBoundsCheck(ival, base, nTargets);
      break;

    default:
      not_reached();
  }
  if (s->decRefCount() == 0) {
    s->release();
  }
  return ival;
}

static int64 switchObjHelper(ObjectData* o, int64 base, int64 nTargets) {
  int64 ival = o->o_toInt64();
  if (o->decRefCount() == 0) {
    o->release();
  }
  return switchBoundsCheck(ival, base, nTargets);
}

void
TranslatorX64::translateSwitch(const Tracelet& t,
                               const NormalizedInstruction& i) {
  int64 base = i.imm[1].u_I64A;
  bool bounded = i.imm[2].u_IVA;
  const ImmVector& iv = i.immVec;
  int nTargets = bounded ? iv.size() - 2 : iv.size();
  int jmptabSize = nTargets;
  ASSERT(nTargets > 0);
  PhysReg valReg = getReg(i.inputs[0]->location);
  DataType inType = i.inputs[0]->outerType();
  ASSERT(IMPLIES(inType != KindOfInt64, bounded));
  ASSERT(IMPLIES(bounded, iv.size() > 2));
  syncOutputs(t); // this will mark valReg as FREE but it still has
                  // its old value

  SrcKey defaultSk(curFunc(), i.offset() + iv.vec32()[iv.size() - 1]);
  SrcKey zeroSk(curFunc(), 0);
  if (0 >= base && 0 < (base + nTargets)) {
    zeroSk.m_offset = i.offset() + iv.vec32()[0 - base];
  } else {
    zeroSk.m_offset = defaultSk.m_offset;
  }

  switch (i.inputs[0]->outerType()) {
    NULLCASE(): {
      emitBindJmp(zeroSk);
      return;
    }

    case KindOfBoolean: {
      SrcKey nonzeroSk(curFunc(), i.offset() + iv.vec32()[iv.size() - 2]);
      a.test_reg64_reg64(valReg, valReg);
      emitCondJmp(nonzeroSk, zeroSk, CC_NZ);
      return;
    }

    case KindOfInt64:
      // No special treatment needed
      break;

    case KindOfDouble:
    STRINGCASE():
    case KindOfObject: {
      // switch(Double|String|Obj)Helper do bounds-checking for us, so
      // we need to make sure the default case is in the jump table,
      // and don't emit our own bounds-checking code
      jmptabSize = iv.size();
      bounded = false;
      if (false) {
        StringData* s = NULL;
        ObjectData* o = NULL;
        switchDoubleHelper(0.0, 0, 0);
        switchStringHelper(s, 0, 0);
        switchObjHelper(o, 0, 0);
      }
      EMIT_CALL(a,
                 inType == KindOfDouble ? (TCA)switchDoubleHelper :
                 (IS_STRING_TYPE(inType) ? (TCA)switchStringHelper :
                  (TCA)switchObjHelper),
                 R(valReg), IMM(base), IMM(nTargets));
      recordCall(i);
      valReg = rax;
      break;
    }

    case KindOfArray:
      emitDecRef(a, i, valReg, KindOfArray);
      emitBindJmp(defaultSk);
      return;

    default:
      not_reached();
  }

  if (bounded) {
    if (base) {
      a.sub_imm64_reg64(base, valReg);
    }
    a.cmp_imm64_reg64(nTargets, valReg);
    prepareForSmash(a, kJmpccLen);
    TCA defaultStub =
      emitServiceReq(REQ_BIND_JMPCC_SECOND, 3,
                     a.code.frontier, defaultSk.m_offset, CC_AE);
    // Unsigned comparison: check for < 0 and >= nTargets at the same time
    a.jae(defaultStub);
  }

  TCA* jmptab = m_globalData.alloc<TCA>(sizeof(TCA), jmptabSize);
  TCA afterLea = a.code.frontier + kLeaRipLen;
  ptrdiff_t diff = (TCA)jmptab - afterLea;
  ASSERT(deltaFits(diff, sz::dword));
  a.lea_rip_disp_reg64(diff, rScratch);
  ASSERT(a.code.frontier == afterLea);
  a.jmp_reg64_index_displ(rScratch, valReg, 0);

  for (int idx = 0; idx < jmptabSize; ++idx) {
    SrcKey sk(curFunc(), i.offset() + iv.vec32()[idx]);
    jmptab[idx] = emitServiceReq(false, REQ_BIND_ADDR, 2ull,
                                 &jmptab[idx], uint64_t(sk.offset()));
  }
}

void
TranslatorX64::analyzeRetC(Tracelet& t,
                           NormalizedInstruction& i) {
  i.manuallyAllocInputs = true;
  i.m_txFlags = Supported;
}

void
TranslatorX64::analyzeRetV(Tracelet& t,
                           NormalizedInstruction& i) {
  analyzeRetC(t, i);
}

// translateRetC --
//
//   Return to caller with the current activation record replaced with the
//   top-of-stack return value. Call with outputs sync'ed, so the code
//   we're emmitting runs "in between" basic blocks.
void
TranslatorX64::translateRetC(const Tracelet& t,
                             const NormalizedInstruction& i) {
  /**
   * This method chooses one of two ways to generate machine code for RetC
   * depending on whether we are generating a specialized return (where we
   * free the locals inline when possible) or a generic return (where we call
   * a helper function to free locals).
   *
   * For the specialized return, we emit the following flow:
   *
   *   Check if varenv is NULL
   *   If it's not NULL, branch to label 2
   *   Free each local variable
   * 1:
   *   Teleport the return value to appropriate memory location
   *   Restore the old values for rVmFp and rVmSp, and
   *   unconditionally transfer control back to the caller
   * 2:
   *   Call the frame_free_locals helper
   *   Jump to label 1
   *
   * For a generic return, we emit the following flow:
   *
   *   Call the frame_free_locals helper
   *   Teleport the return value to appropriate memory location
   *   Restore the old values for rVmFp and rVmSp, and
   *   unconditionally transfer control back to the caller
   */

  int adjust = t.m_stackChange;
  if (i.skipSync) {
    /*
     * getting here means there was nothing to do between
     * a previous reqXXX and this ret. Any spill code we generate
     * here would be broken (because the rbx is wrong), so
     * verify that we dont generate anything...
     */
    TCA s DEBUG_ONLY = a.code.frontier;
    syncOutputs(0);
    ASSERT(s == a.code.frontier);
    adjust = 0;
  } else {
    /*
     * no need to syncOutputs here... we're going to update
     * rbx at the end of this function anyway, and we may want
     * to use enregistered locals on the fast path below
     */
    m_regMap.scrubStackEntries(t.m_stackChange);
    m_regMap.cleanAll();
  }

  bool noThis = !curFunc()->isPseudoMain() &&
                (!curFunc()->isMethod() || curFunc()->isStatic());
  bool mayUseVV = (curFunc()->attrs() & AttrMayUseVV);

  TypedValue tv;
  TV_WRITE_UNINIT(&tv);
  tv.m_data.num = 0; // to keep the compiler happy
  if (i.grouped) {
    /*
     * We suppressed the write of the (literal) return value
     * to the stack. Figure out what it was.
     */
    NormalizedInstruction* prev = i.prev;
    ASSERT(!prev->outStack);
    switch (prev->op()) {
      case OpNull:
        tv.m_type = KindOfNull;
        break;
      case OpTrue:
      case OpFalse:
        tv.m_type = KindOfBoolean;
        tv.m_data.num = prev->op() == OpTrue;
        break;
      case OpInt:
        tv.m_type = KindOfInt64;
        tv.m_data.num = prev->imm[0].u_I64A;
        break;
      case OpDouble:
        tv.m_type = KindOfDouble;
        tv.m_data.dbl = prev->imm[0].u_DA;
        break;
      case OpString:
        tv.m_type = BitwiseKindOfString;
        tv.m_data.pstr = curUnit()->lookupLitstrId(prev->imm[0].u_SA);
        break;
      case OpArray:
        tv.m_type = KindOfArray;
        tv.m_data.parr = curUnit()->lookupArrayId(prev->imm[0].u_AA);
        break;
      default:
        assert(false);
    }
  }
  /*
   * figure out where to put the return value, and where to get it from
   */
  const Func *callee = curFunc();
  ASSERT(callee);
  int nLocalCells =
    callee == NULL ? 0 : // This happens for returns from pseudo-main.
    callee->numSlotsInFrame();
  int retvalSrcBase  = cellsToBytes(-adjust); // 0(rVmSp) is retval

  /*
   * The (1 + nLocalCells) skips 1 slot for the return value.
   */
  TRACE(2, "nLocalCells: %d\n", nLocalCells);
  int retvalDestDisp =
    cellsToBytes(1 + nLocalCells - adjust) + AROFF(m_r);

  if (freeLocalsInline()) {
    // Emit specialized code inline to clean up the locals
    ASSERT(curFunc()->numLocals() == (int)i.inputs.size());

    boost::scoped_ptr<DiamondReturn> dr;
    if (mayUseVV) {
      dr.reset(new DiamondReturn);
      ScratchReg scratch(m_regMap);
      a. load_reg64_disp_reg64(rVmFp, AROFF(m_varEnv), *scratch);
      a. test_reg64_reg64(*scratch, *scratch);
      {
        UnlikelyIfBlock<CC_NZ> varEnvCheck(a, astubs, dr.get());
        if (i.grouped) {
          emitStoreImm(astubs, tv.m_type,
                       rVmSp, retvalSrcBase + TVOFF(m_type), sz::dword);
          if (tv.m_type != KindOfNull) {
            emitStoreImm(astubs, tv.m_data.num,
                         rVmSp, retvalSrcBase, sz::qword);
          }
        }
        emitFrameRelease(astubs, i, noThis);
      }
    }

    for (unsigned int k = 0; k < i.inputs.size(); ++k) {
      // RetC's inputs should all be locals
      ASSERT(i.inputs[k]->location.space == Location::Local);
      DataType t = i.inputs[k]->outerType();
      if (IS_REFCOUNTED_TYPE(t)) {
        PhysReg reg = m_regMap.allocReg(i.inputs[k]->location, t,
                                        RegInfo::CLEAN);
        emitDecRef(i, reg, t);
      }
    }

    // If this is a instance method called on an object or if it is a
    // pseudomain, we need to decRef $this (if there is one)
    if (curFunc()->isMethod() && !curFunc()->isStatic()) {
      // This assert is weaker than it looks; it only checks the invocation
      // we happen to be translating for. The runtime "assert" is the
      // unconditional dereference of m_this we emit; if the frame has
      // neither this nor a class, then m_this will be null and we'll
      // SEGV.
      ASSERT(curFrame()->hasThis() || curFrame()->hasClass());
      // m_this and m_cls share a slot in the ActRec, so we check the
      // lowest bit (0 -> m_this, 1 -> m_cls)
      a.      load_reg64_disp_reg64(rVmFp, AROFF(m_this), rdi);
      if (i.guardedThis) {
        emitDecRef(i, rdi, KindOfObject);
      } else {
        a.      test_imm32_reg64(1, rdi);
        {
          JccBlock<CC_NZ> ifZero(a);
          emitDecRef(i, rdi, KindOfObject); // this. decref it.
        }
      }
    } else if (curFunc()->isPseudoMain()) {
      a.      load_reg64_disp_reg64(rVmFp, AROFF(m_this), rdi);
      a.      shr_imm32_reg64(1, rdi); // sets c (from bit 0) and z
      {
        // tests for Not Zero and Not Carry
        UnlikelyIfBlock<CC_NBE> ifRealThis(a, astubs);
        astubs.    shl_imm32_reg64(1, rdi);
        emitDecRef(astubs, i, rdi, KindOfObject);
      }
    }

    m_regMap.smashRegs(kAllRegs);
    emitTestSurpriseFlags();
    {
      UnlikelyIfBlock<CC_NZ> ifTracer(a, astubs);
      if (i.grouped) {
        emitStoreImm(astubs, tv.m_type,
                     rVmSp, retvalSrcBase + TVOFF(m_type), sz::dword);
        if (tv.m_type != KindOfNull) {
          emitStoreImm(astubs, tv.m_data.num,
                       rVmSp, retvalSrcBase, sz::qword);
        }
      }
      astubs.mov_reg64_reg64(rVmFp, argNumToRegName[0]);
      emitCall(astubs, (TCA)&EventHook::FunctionExit, true);
      recordReentrantStubCall(i);
    }
  } else {
    if (i.grouped) {
      /*
       * what a pain: EventHook::onFunctionExit needs access
       * to the return value - so we'd better not have suppressed
       * writing it to the stack...
       */
      not_reached();
    }
    m_regMap.smashRegs(kAllRegs);
    // If we are doing the generic return flow, we emit a call to
    // frame_free_locals here
    ASSERT(i.inputs.size() == 0);
    emitFrameRelease(a, i, noThis);
  }

  const PhysReg rRetAddr = rax;
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_savedRip), rRetAddr);
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_savedRbp), rVmFp);

  /*
   * Having gotten everything we care about out of the current frame
   * pointer, smash the return address type and value over it. We don't
   * care about reference counts: as long as this runs to completion, we're
   * refcount-neutral.
   */
  if (i.grouped) {
    emitStoreImm(a, tv.m_type,
                 rVmSp, retvalDestDisp + TVOFF(m_type), sz::dword);
    if (tv.m_type != KindOfNull) {
      emitStoreImm(a, tv.m_data.num,
                   rVmSp, retvalDestDisp, sz::qword);
    }
  } else {
    ASSERT(sizeof(Cell) == 16);
    a.   load_reg64_disp_reg64 (rVmSp,    retvalSrcBase,      rScratch);
    a.   store_reg64_disp_reg64(rScratch, retvalDestDisp,     rVmSp);
    a.   load_reg64_disp_reg64 (rVmSp,    retvalSrcBase + 8,  rScratch);
    a.   store_reg64_disp_reg64(rScratch, retvalDestDisp + 8, rVmSp);
  }
  /*
   * Now update the principle hardware registers.
   *
   * Stack pointer has to skip over all the locals as well as the
   * activation record.
   */
  a.   add_imm64_reg64(sizeof(ActRec) +
                       cellsToBytes(nLocalCells - adjust), rVmSp);
  emitRB(a, RBTypeFuncExit, curFunc()->fullName()->data(), RegSet(rRetAddr));
  a.   jmp_reg        (rRetAddr);
  translator_not_reached(a);
}

void
TranslatorX64::translateRetV(const Tracelet& t,
                             const NormalizedInstruction& i) {
  translateRetC(t, i);
}

void TranslatorX64::emitNativeImpl(const Func* func,
                                   bool emitSavedRIPReturn) {
  BuiltinFunction builtinFuncPtr = func->builtinFuncPtr();
  if (false) { // typecheck
    ActRec* ar = NULL;
    builtinFuncPtr(ar);
  }

  TRACE(2, "calling builtin preClass %p func %p\n", func->preClass(),
    builtinFuncPtr);
  // Call the native implementation. This will free the locals for us in the
  // normal case. In the case where an exception is thrown, the VM unwinder
  // will handle it for us.
  a.   mov_reg64_reg64(rVmFp, argNumToRegName[0]);
  emitCall(a, (TCA)builtinFuncPtr, false /* smash regs */);

  /*
   * We're sometimes calling this while curFunc() isn't really the
   * builtin---make sure to properly record the sync point as if we
   * are inside the builtin.
   *
   * The assumption here is that for builtins, the generated func
   * contains only a single opcode (NativeImpl), and there are no
   * non-argument locals.
   */
  ASSERT(func->numIterators() == 0 && func->isBuiltin());
  ASSERT(func->numLocals() == func->numParams());
  ASSERT(*func->getEntry() == OpNativeImpl);
  ASSERT(instrLen(func->getEntry()) == func->past() - func->base());
  Offset pcOffset = 0;  // NativeImpl is the only instruction in the func
  Offset stackOff = func->numLocals(); // Builtin stubs have no
                                       // non-arg locals
  recordSyncPoint(a, pcOffset, stackOff);

  const PhysReg rRetAddr = rax;
  RegSet saveDuringEmitRB;
  if (emitSavedRIPReturn) {
    // Get the return address from the ActRec
    a.   load_reg64_disp_reg64(rVmFp, AROFF(m_savedRip), rRetAddr);
    saveDuringEmitRB |= RegSet(rRetAddr);
  }
  // The native implementation put the return value in the right place for
  // us already; all we have left to do is update the frame pointer and the
  // stack pointer
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_savedRbp), rVmFp);
  int nLocalCells = func->numSlotsInFrame();
  a.   add_imm64_reg64(sizeof(ActRec) + cellsToBytes(nLocalCells-1), rVmSp);
  emitRB(a, RBTypeFuncExit, func->fullName()->data(), saveDuringEmitRB);
  if (emitSavedRIPReturn) {
    a.   jmp_reg        (rRetAddr);
    translator_not_reached(a);
  }
}

void
TranslatorX64::translateNativeImpl(const Tracelet& t,
                                   const NormalizedInstruction& ni) {
  syncOutputs(t);
  ASSERT(ni.stackOff == 0); // Assumed in emitNativeImpl
  emitNativeImpl(curFunc(), true);
}

// Warning: smashes rsi and rdi, and can't handle unclean registers.
// Used between functions.
void
TranslatorX64::emitFrameRelease(X64Assembler& a,
                                const NormalizedInstruction& i,
                                bool noThis /*= false*/) {
  if (false) { // typecheck
    frame_free_locals(curFrame(), 0);
  }
  a.     mov_reg64_reg64(rVmFp, argNumToRegName[0]);
  int numLocals = curFunc()->numLocals();
  emitImmReg(a, numLocals, argNumToRegName[1]);
  if (noThis) {
    emitCall(a, (TCA)frame_free_locals_no_this);
  } else {
    emitCall(a, (TCA)frame_free_locals);
  }
  recordReentrantCall(a, i);
}

// emitClsLocalIndex --
// emitStringToClass --
// emitStringToKnownClass --
// emitObjToClass --
// emitClsAndPals --
//   Helpers for AGetC/AGetL.

const int kEmitClsLocalIdx = 0;

void
TranslatorX64::emitStringToKnownClass(const NormalizedInstruction& i,
                                      const StringData* clsName) {
  using namespace TargetCache;
  ASSERT(clsName);
  Stats::emitInc(a, Stats::TgtCache_KnownClsHit);
  CacheHandle ch = allocKnownClass(clsName);
  ScratchReg cls(m_regMap);
  a.          load_reg64_disp_reg64(rVmTl, ch, *cls);
  a.          test_reg64_reg64(*cls, *cls);
  {
    UnlikelyIfBlock<CC_Z> ifNull(a, astubs);
    ScratchReg clsPtr(m_regMap);
    astubs.   lea_reg64_disp_reg64(rVmTl, ch, *clsPtr);
    if (false) { // typecheck
      Class** cache = NULL;
      UNUSED Class* ret =
        TargetCache::lookupKnownClass<false>(cache, clsName, true);
    }
    // We're only passing two arguments to lookupKnownClass because
    // the third is ignored in the checkOnly == false case
    EMIT_CALL(astubs, ((TargetCache::lookupKnownClass_func_t)
                         TargetCache::lookupKnownClass<false>),
               R(*clsPtr), IMM((uintptr_t)clsName));
    recordReentrantStubCall(i);
    // UnlikelyIfBlock will restore cls's SCRATCH state but not its
    // contents. lookupKnownClass returns the value we want.
    emitMovRegReg(astubs, rax, *cls);
  }
  m_regMap.bindScratch(cls, i.outStack->location, KindOfClass, RegInfo::DIRTY);
}

void
TranslatorX64::emitStringToClass(const NormalizedInstruction& i) {
  using namespace TargetCache;
  if (!i.inputs[kEmitClsLocalIdx]->rtt.valueString()) {
    // Handle the case where we don't know the name of the class
    // at translation time
    const Location& in = i.inputs[kEmitClsLocalIdx]->location;
    const Location& out = i.outStack->location;
    CacheHandle ch = ClassCache::alloc();
    if (false) {
      StringData *name = NULL;
      const UNUSED Class* cls = ClassCache::lookup(ch, name);
    }
    TRACE(1, "ClassCache @ %d\n", int(ch));
    if (i.inputs[kEmitClsLocalIdx]->rtt.isVariant()) {
        EMIT_CALL(a, ClassCache::lookup,
                   IMM(ch),
                   DEREF(in));
    } else {
        EMIT_CALL(a, ClassCache::lookup,
                   IMM(ch),
                   V(in));
    }
    recordReentrantCall(i);
    m_regMap.bind(rax, out, KindOfClass, RegInfo::DIRTY);
    return;
  }
  // We know the name of the class at translation time; use the
  // target cache associated with the name of the class
  const StringData* clsName = i.inputs[kEmitClsLocalIdx]->rtt.valueString();
  emitStringToKnownClass(i, clsName);
}

void
TranslatorX64::emitObjToClass(const NormalizedInstruction& i) {
  m_regMap.allocOutputRegs(i);
  const Location& in = i.inputs[kEmitClsLocalIdx]->location;
  const Location& out = i.outStack->location;
  PhysReg src = getReg(in);
  ScratchReg tmp(m_regMap);
  if (i.inputs[kEmitClsLocalIdx]->rtt.isVariant()) {
    emitDeref(a, src, *tmp);
    src = *tmp;
  }
  ASSERT(i.outStack->valueType() == KindOfClass);
  a.   load_reg64_disp_reg64(src, ObjectData::getVMClassOffset(), getReg(out));
}

void
TranslatorX64::emitClsAndPals(const NormalizedInstruction& ni) {
  if (ni.inputs[kEmitClsLocalIdx]->isString()) {
    emitStringToClass(ni);
  } else {
    emitObjToClass(ni);
  }
}

void
TranslatorX64::analyzeAGetC(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.outStack->valueType() == KindOfClass);
  const RuntimeType& rtt = i.inputs[0]->rtt;
  ASSERT(!rtt.isVariant());
  i.m_txFlags = supportedPlan(rtt.isString() ||
                              rtt.valueType() == KindOfObject);
}

void TranslatorX64::translateAGetC(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  emitClsAndPals(i);
}

void TranslatorX64::analyzeAGetL(Tracelet& t,
                                 NormalizedInstruction& ni) {
  ASSERT(ni.inputs.size() == 1);
  ASSERT(ni.inputs[0]->isLocal());
  const RuntimeType& rtt = ni.inputs[0]->rtt;
  ni.m_txFlags = supportedPlan(rtt.isString() ||
                               rtt.valueType() == KindOfObject);
}

void TranslatorX64::translateAGetL(const Tracelet& t,
                                   const NormalizedInstruction& ni) {
  emitClsAndPals(ni);
}

void TranslatorX64::translateSelf(const Tracelet& t,
                                  const NormalizedInstruction& i) {

  m_regMap.allocOutputRegs(i);
  PhysReg tmp = getReg(i.outStack->location);
  Class* clss = curFunc()->cls();
  // if in repo authorative mode & the class is unique, then we can
  // just burn the class ref into an immediate
  if (RuntimeOption::RepoAuthoritative &&
     (clss->preClass()->attrs() & AttrUnique)) {
    // tmp = clss
    a.mov_imm64_reg((int64_t)clss,tmp);
  } else {
    // tmp = rfp->m_func
    // tmp = tmp->m_cls
    a.load_reg64_disp_reg64(rVmFp,offsetof(ActRec,m_func),tmp);
    a.load_reg64_disp_reg64(tmp, Func::clsOff(), tmp);
    // Note: After Mark's checkin, we can simplify further to
    if (false) {
      a.load_reg64_disp_reg64(rVmTl,
                              TargetCache::allocKnownClass(clss->name()),
                              tmp);
    }
  }
}

void TranslatorX64::translateParent(const Tracelet& t,
                                    const NormalizedInstruction& i) {
  Class* clss = curFunc()->cls()->parent();
  // if in repo authorative mode & the class is unique, then we can
  // just burn the class ref into an immediate
  if (RuntimeOption::RepoAuthoritative &&
     (clss->preClass()->attrs() & AttrUnique)) {
    // tmp = clss
    m_regMap.allocOutputRegs(i);
    PhysReg tmp = getReg(i.outStack->location);
    a.mov_imm64_reg((int64_t)clss,tmp);
  } else {
    // Note: After Mark's checkin, we can simplify further like
    // translateSelf above
    emitStringToKnownClass(i, clss->name());
  }
}

void TranslatorX64::analyzeSelf(Tracelet& t,NormalizedInstruction& i) {
  Class* clss = curClass();
  if (clss == NULL) {
    i.m_txFlags = Interp;
    return;
  }
  i.m_txFlags = Supported;
}

void TranslatorX64::analyzeParent(Tracelet& t,NormalizedInstruction& i) {
  Class* clss = curClass();
  if (clss == NULL) {
    i.m_txFlags = Interp;
    return;
  }
  if (clss->parent() == NULL) {
    // clss has no parent; interpret to throw fatal
    i.m_txFlags = Interp;
    return;
  }
  i.m_txFlags = Supported;
}

void TranslatorX64::translateDup(const Tracelet& t,
                                 const NormalizedInstruction& ni) {
  ASSERT(ni.inputs.size() == 1);
  ASSERT(ni.outStack);
  ASSERT(!ni.inputs[0]->rtt.isVariant());
  m_regMap.allocOutputRegs(ni);
  PhysReg outR = getReg(ni.outStack->location);
  emitMovRegReg(a, getReg(ni.inputs[0]->location), outR);
  emitIncRef(outR, ni.inputs[0]->outerType());
}

typedef std::map<int, int> ParamMap;
/*
 * mapGenParams determines if every named local in origFunc has a
 * corresponding named local in genFunc. If this step succeeds and
 * there's no VarEnv at runtime, the continuation's variables can be
 * filled completely inline in the TC (assuming there aren't too
 * many).
 */
static bool mapGenParams(ParamMap& map,
                         const Func* origFunc, const Func* genFunc) {
  const StringData* const* varNames = origFunc->localNames();
  for (Id i = 0; i < origFunc->numNamedLocals(); ++i) {
    Id id = genFunc->lookupVarId(varNames[i]);
    if (id != kInvalidId) {
      map[i] = id;
    } else {
      return false;
    }
  }
  return true;
}

void TranslatorX64::emitCallFillCont(X64Assembler& a,
                                     const Func* orig,
                                     const Func* gen) {
  if (false) {
    ActRec* fp = NULL;
    c_GenericContinuation *cont = NULL;
    cont =
      VMExecutionContext::fillContinuationVars(fp, orig, gen, cont);
  }
  EMIT_CALL(a,
             VMExecutionContext::fillContinuationVars,
             R(rVmFp),
             IMM((intptr_t)orig),
             IMM((intptr_t)gen),
             R(rax));
}

void TranslatorX64::translateCreateCont(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  bool getArgs = i.imm[0].u_IVA;
  const StringData* genName = curUnit()->lookupLitstrId(i.imm[1].u_SA);
  const StringData* className = curUnit()->lookupLitstrId(i.imm[2].u_SA);
  const Func* origFunc = curFunc();
  Class* genClass = Unit::lookupClass(className);
  ASSERT(genClass);
  const Func* genFunc = origFunc->getGeneratorBody(genName);

  if (false) {
    ActRec* fp = NULL;
    UNUSED c_GenericContinuation* cont =
      VMExecutionContext::createContinuation(fp, getArgs, origFunc, genClass,
                                             genFunc);
  }

  // Even callee-saved regs need to be clean, because
  // createContinuation will read all locals.
  m_regMap.cleanAll();
  EMIT_CALL(a,
             VMExecutionContext::createContinuation,
             R(rVmFp),
             IMM(getArgs),
             IMM((intptr_t)origFunc),
             IMM((intptr_t)genClass),
             IMM((intptr_t)genFunc));
  ScratchReg holdRax(m_regMap, rax);

  int origLocals = origFunc->numNamedLocals();
  int genLocals = genFunc->numNamedLocals() - 1;
  ParamMap params;
  if (origLocals <= kMaxInlineContLocals &&
      mapGenParams(params, origFunc, genFunc)) {
    ScratchReg rScratch(m_regMap);
    a.  load_reg64_disp_reg64(rVmFp, AROFF(m_varEnv), *rScratch);
    a.  test_reg64_reg64(*rScratch, *rScratch);
    DiamondReturn astubsRet;
    {
      UnlikelyIfBlock<CC_NZ> ifVarEnv(a, astubs, &astubsRet);
      Stats::emitInc(astubs, Stats::Tx64_ContCreateSlow);
      emitCallFillCont(astubs, origFunc, genFunc);
    }
    // fillContinuationVars returned the continuation in rax and
    // DiamondGuard marked rax as scratch again, so it's safe to keep
    // using it
    Stats::emitInc(a, Stats::Tx64_ContCreateFast);
    static const StringData* thisStr = StringData::GetStaticString("this");
    Id thisId = kInvalidId;
    bool fillThis = origFunc->isNonClosureMethod() && !origFunc->isStatic() &&
      ((thisId = genFunc->lookupVarId(thisStr)) != kInvalidId) &&
      (origFunc->lookupVarId(thisStr) == kInvalidId);
    ScratchReg rDest(m_regMap);
    if (origLocals > 0 || fillThis) {
      a.lea_reg64_disp_reg64(rax,
                             c_GenericContinuation::localsOffset(),
                             *rDest);
    }
    for (int i = 0; i < origLocals; ++i) {
      ASSERT(mapContains(params, i));
      int destOff = cellsToBytes(genLocals - params[i]);
      emitCopyTo(a, rVmFp, localOffset(i), *rDest, destOff, *rScratch, rFlag);
      emitIncRefGenericRegSafe(*rDest, destOff, *rScratch);
    }

    // Deal with a potential $this local in the generator body
    if (fillThis) {
      ASSERT(thisId != kInvalidId);
      a.load_reg64_disp_reg64(rax,
                              offsetof(c_GenericContinuation, m_obj),
                              *rScratch);
      a.test_reg64_reg64(*rScratch, *rScratch);
      {
        JccBlock<CC_Z> ifObj(a);
        const int thisOff = cellsToBytes(genLocals - thisId);
        // We don't have to check for a static refcount since we
        // know it's an Object
        a.add_imm32_disp_reg32(1, TVOFF(_count), *rScratch);
        a.store_reg64_disp_reg64(*rScratch, thisOff + TVOFF(m_data), *rDest);
        a.store_imm32_disp_reg(KindOfObject, thisOff + TVOFF(m_type), *rDest);
      }
    }
  } else {
    Stats::emitInc(a, Stats::Tx64_ContCreateSlow);
    emitCallFillCont(a, origFunc, genFunc);
  }
  m_regMap.bindScratch(holdRax, i.outStack->location, KindOfObject,
                       RegInfo::DIRTY);
}

void TranslatorX64::emitCallUnpack(X64Assembler& a,
                                   const NormalizedInstruction& i,
                                   int nCopy) {
  const int contIdx = 0;

  if (false) {
    c_GenericContinuation* cont = NULL;
    TypedValue* dest = NULL;
    VMExecutionContext::unpackContinuation(cont, dest);
  }
  EMIT_CALL(a,
             VMExecutionContext::unpackContinuation,
             V(i.inputs[contIdx]->location),
             A(Location(Location::Local, nCopy)));
  recordCall(a, i);
}

void TranslatorX64::translateUnpackCont(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  const int contIdx = 0;
  ASSERT(curFrame()->m_varEnv == NULL);
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.inputs[contIdx]->location == Location(Location::Local, 0));
  ASSERT(i.outStack->outerType() == KindOfInt64);
  int nCopy = curFunc()->numNamedLocals() - 1;

  for (int loc = 1; loc <= nCopy; ++loc) {
    // We're at the beginning of the function. The only local in a
    // register should be local 0, our input
    ASSERT(!m_regMap.hasReg(Location(Location::Local, loc)));
  }
  if (nCopy > kMaxInlineContLocals) {
    Stats::emitInc(a, Stats::Tx64_ContUnpackSlow);
    emitCallUnpack(a, i, nCopy);
    m_regMap.bind(rax, i.outStack->location, KindOfInt64,
                  RegInfo::DIRTY);
    return;
  }

  PhysReg rCont = getReg(i.inputs[contIdx]->location);
  ScratchReg rLabel(m_regMap);
  {
    a.    test_imm32_disp_reg32(0x1,
                                offsetof(c_GenericContinuation, m_hasExtraVars),
                                rCont);
    DiamondReturn astubsRet;
    {
      UnlikelyIfBlock<CC_NZ> hasVars(a, astubs, &astubsRet);
      Stats::emitInc(astubs, Stats::Tx64_ContUnpackSlow);
      emitCallUnpack(astubs, i, nCopy);
      emitMovRegReg(astubs, rax, *rLabel);
    }
    Stats::emitInc(a, Stats::Tx64_ContUnpackFast);

    a.    load_reg64_disp_reg64(rCont,
                                offsetof(c_GenericContinuation, m_label),
                                *rLabel);
    ScratchReg rScratch(m_regMap);
    ScratchReg rSrc(m_regMap);
    ScratchReg rZero(m_regMap);
    if (nCopy > 0) {
      a.  lea_reg64_disp_reg64(rCont,
                               c_GenericContinuation::localsOffset(),
                               *rSrc);
      a.  xor_reg32_reg32(*rZero, *rZero);
    }
    for (int srcOff = 0, destOff = localOffset(nCopy);
         srcOff < (int)cellsToBytes(nCopy);
         srcOff += sizeof(Cell), destOff += sizeof(Cell)) {
      emitCopyTo(a, *rSrc, srcOff, rVmFp, destOff, *rScratch, *rZero);
      a.  store_reg32_disp_reg64(*rZero, srcOff + TVOFF(m_type), *rSrc);
    }
  }
  m_regMap.bindScratch(rLabel, i.outStack->location, KindOfInt64,
                       RegInfo::DIRTY);
}

void TranslatorX64::emitCallPack(X64Assembler& a,
                                 const NormalizedInstruction& i,
                                 int nCopy) {
  const int valIdx = 0;
  const int contIdx = 1;

  // packContinuation is going to read values directly from the stack
  // so we have to clean everything. We're about to do an EMIT_CALL so
  // everything will get cleaned anyway but we don't want to rely on
  // that.
  m_regMap.cleanAll();
  if (false) {
    c_GenericContinuation* cont = NULL;
    TypedValue* tv = NULL;
    ActRec* fp = NULL;
    int label = 0;
    VMExecutionContext::packContinuation(cont, fp, tv, label);
  }
  EMIT_CALL(a,
             VMExecutionContext::packContinuation,
             V(i.inputs[contIdx]->location),
             R(rVmFp),
             A(i.inputs[valIdx]->location),
             IMM(i.imm[0].u_IVA));
  recordCall(a, i);
  m_regMap.invalidateLocals(1, nCopy);
}

void TranslatorX64::translatePackCont(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  const int valIdx = 0;
  const int contIdx = 1;

  int nCopy = curFunc()->numNamedLocals() - 1;
  if (nCopy > kMaxInlineContLocals) {
    Stats::emitInc(a, Stats::Tx64_ContPackSlow);
    emitCallPack(a, i, nCopy);
    emitDecRefInput(a, i, valIdx);
    return;
  }

  ScratchReg rScratch(m_regMap);
  a.    load_reg64_disp_reg64(rVmFp, AROFF(m_varEnv), *rScratch);
  a.    test_reg64_reg64(*rScratch, *rScratch);
  DiamondReturn astubsRet;
  {
    // TODO: Task #1132976: We can probably prove that this is impossible in
    // most cases using information from hphpc
    UnlikelyIfBlock<CC_NZ> varEnv(a, astubs, &astubsRet);
    Stats::emitInc(astubs, Stats::Tx64_ContPackSlow);
    emitCallPack(astubs, i, nCopy);
    emitDecRefInput(astubs, i, valIdx);
  }
  Stats::emitInc(a, Stats::Tx64_ContPackFast);

  PhysReg rCont = getReg(i.inputs[contIdx]->location);
  ScratchReg rDest(m_regMap);
  ScratchReg rZero(m_regMap);
  if (nCopy > 0) {
    a.  lea_reg64_disp_reg64(rCont,
                             c_GenericContinuation::localsOffset(),
                             *rDest);
    a.  xor_reg32_reg32(*rZero, *rZero);
  }
  for (int idx = nCopy, destOff = 0, srcOff = localOffset(nCopy);
       idx > 0;
       --idx, destOff += sizeof(Cell), srcOff += sizeof(Cell)) {
    Location loc(Location::Local, idx);
    if (m_regMap.hasReg(loc)) {
      PhysReg reg = getReg(loc);
      spillTo(m_regMap.getInfo(reg)->m_type, reg, true, *rDest, destOff);
    } else {
      emitCopyTo(a, rVmFp, srcOff, *rDest, destOff, *rScratch, *rZero);
    }
    m_regMap.invalidate(loc);
    a.  store_reg32_disp_reg64(*rZero, srcOff + TVOFF(m_type), rVmFp);
  }

  int valueOff = offsetof(c_GenericContinuation, m_value);
  emitDecRefGeneric(i, rCont, valueOff);
  // We're moving our reference to the value from the stack to the
  // continuation object, so we don't have to incRef or decRef
  Location valLoc = i.inputs[valIdx]->location;
  spillTo(i.inputs[valIdx]->outerType(), getReg(valLoc), true, rCont, valueOff);
  emitImmReg(a, i.imm[0].u_IVA, *rScratch);
  a.    store_reg64_disp_reg64(*rScratch,
                               offsetof(c_GenericContinuation, m_label),
                               rCont);
}

static void continuationRaiseHelper(c_GenericContinuation* cont) {
  cont->t_raised();
  not_reached();
}

void TranslatorX64::emitContRaiseCheck(X64Assembler& a,
                                       const NormalizedInstruction& i) {
  const int contIdx = 0;
  ASSERT(i.inputs[contIdx]->location == Location(Location::Local, 0));
  PhysReg rCont = getReg(i.inputs[contIdx]->location);
  a.    test_imm32_disp_reg32(0x1,
                              offsetof(c_GenericContinuation, m_should_throw),
                              rCont);
  {
    UnlikelyIfBlock<CC_NZ> ifThrow(a, astubs);
    if (false) {
      c_GenericContinuation* c = NULL;
      continuationRaiseHelper(c);
    }
    EMIT_CALL(astubs,
               continuationRaiseHelper,
               R(rCont));
    recordReentrantStubCall(i);
    translator_not_reached(astubs);
  }
}

void TranslatorX64::translateContReceive(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  const int contIdx = 0;
  emitContRaiseCheck(a, i);
  ScratchReg rScratch(m_regMap);
  a.   lea_reg64_disp_reg64(getReg(i.inputs[contIdx]->location),
                            offsetof(c_GenericContinuation, m_received),
                            *rScratch);
  emitIncRefGeneric(*rScratch, 0);
  emitCopyToStack(a, i, *rScratch, -1 * (int)sizeof(Cell));
}

void TranslatorX64::translateContRaised(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  emitContRaiseCheck(a, i);
}

void TranslatorX64::translateContDone(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  const int contIdx = 0;
  a.    store_imm8_disp_reg(0x1,
                            offsetof(c_GenericContinuation, m_done),
                            getReg(i.inputs[contIdx]->location));
}

static void contPreNextThrowHelper(c_GenericContinuation* c) {
  c->preNext();
  not_reached();
}

void TranslatorX64::emitContPreNext(const NormalizedInstruction& i,
                                    ScratchReg& rCont) {
  const Offset doneOffset = offsetof(c_GenericContinuation, m_done);
  CT_ASSERT((doneOffset + 1) == offsetof(c_GenericContinuation, m_running));
  // Check m_done and m_running at the same time
  a.    test_imm32_disp_reg32(0x0101, doneOffset, *rCont);
  {
    UnlikelyIfBlock<CC_NZ> ifThrow(a, astubs);
    EMIT_CALL(astubs, contPreNextThrowHelper, R(*rCont));
    recordReentrantStubCall(i);
    translator_not_reached(astubs);
  }

  // ++m_index
  a.    add_imm64_disp_reg64(0x1, offsetof(c_GenericContinuation, m_index),
                             *rCont);
  // m_running = true
  a.    store_imm8_disp_reg(0x1, offsetof(c_GenericContinuation, m_running),
                            *rCont);

  // push Continuation on the stack
  m_regMap.bindScratch(rCont, i.outStack->location, KindOfObject,
                       RegInfo::DIRTY);
  emitIncRef(*rCont, KindOfObject);
}

void TranslatorX64::translateContNext(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  ScratchReg rCont(m_regMap);
  a.    load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);

  // m_received.setNull()
  const Offset receivedOff = offsetof(c_GenericContinuation, m_received);
  emitDecRefGeneric(i, *rCont, receivedOff);
  emitStoreImm(a, KindOfUninit, *rCont, receivedOff + TVOFF(m_type),
               sz::dword, &m_regMap);

  emitContPreNext(i, rCont);
}

static void contNextCheckThrowHelper(c_GenericContinuation* cont) {
  cont->nextCheck();
  not_reached();
}

void TranslatorX64::emitContNextCheck(const NormalizedInstruction& i,
                                      ScratchReg& rCont) {
  // if (m_index < 0)
  a.    cmp_imm64_disp_reg64(0, offsetof(c_GenericContinuation, m_index),
                             *rCont);
  {
    UnlikelyIfBlock<CC_L> whoops(a, astubs);
    EMIT_CALL(astubs, contNextCheckThrowHelper, *rCont);
    recordReentrantStubCall(i);
    translator_not_reached(astubs);
  }
}

template<bool raise>
void TranslatorX64::translateContSendImpl(const NormalizedInstruction& i) {
  const int valIdx = 0;
  ASSERT(i.inputs[valIdx]->location == Location(Location::Local, 0));

  ScratchReg rCont(m_regMap);
  a.    load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);
  emitContNextCheck(i, rCont);

  // m_received = value
  const Offset receivedOff = offsetof(c_GenericContinuation, m_received);
  emitDecRefGeneric(i, *rCont, receivedOff);
  PhysReg valReg = getReg(i.inputs[valIdx]->location);
  DataType valType = i.inputs[valIdx]->outerType();
  emitIncRef(valReg, valType);
  spillTo(valType, valReg, true, *rCont, receivedOff);

  // m_should_throw = true (maybe)
  if (raise) {
    a.  store_imm8_disp_reg(0x1,
                            offsetof(c_GenericContinuation, m_should_throw),
                            *rCont);
  }

  emitContPreNext(i, rCont);
}

void TranslatorX64::translateContSend(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  translateContSendImpl<false>(i);
}

void TranslatorX64::translateContRaise(const Tracelet& t,
                                       const NormalizedInstruction& i) {
  translateContSendImpl<true>(i);
}

void TranslatorX64::translateContValid(const Tracelet& t,
                                       const NormalizedInstruction& i) {
  ScratchReg rCont(m_regMap);
  a.    load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);

  m_regMap.allocOutputRegs(i);
  PhysReg validReg = getReg(i.outStack->location);
  // !m_done
  a.    loadzxb_reg64_disp_reg64(*rCont,
                                 offsetof(c_GenericContinuation, m_done),
                                 validReg);
  a.    xor_imm32_reg64(0x1, validReg);
}

void TranslatorX64::translateContCurrent(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  ScratchReg rCont(m_regMap);
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);
  emitContNextCheck(i, rCont);

  a.   lea_reg64_disp_reg64(*rCont, offsetof(c_GenericContinuation, m_value),
                            *rCont);
  emitIncRefGeneric(*rCont, 0);
  emitCopyToStack(a, i, *rCont, -1 * (int)sizeof(Cell));
}

void TranslatorX64::translateContStopped(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  ScratchReg rCont(m_regMap);
  a.    load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);
  a.    store_imm8_disp_reg(0x0,
                            offsetof(c_GenericContinuation, m_running),
                            *rCont);
}

void TranslatorX64::translateContHandle(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  // Always interpreted
  not_reached();
}

static void analyzeClassExistsImpl(NormalizedInstruction& i) {
  const int nameIdx = 1;
  const int autoIdx = 0;
  ASSERT(!i.inputs[nameIdx]->isVariant() && !i.inputs[autoIdx]->isVariant());
  i.m_txFlags = supportedPlan(i.inputs[nameIdx]->isString() &&
                              i.inputs[autoIdx]->isBoolean());
  i.fuseBranch = (i.m_txFlags & Supported) &&
    i.inputs[nameIdx]->rtt.valueString() &&
    i.inputs[autoIdx]->rtt.valueBoolean() != RuntimeType::UnknownBool;
}

void TranslatorX64::analyzeClassExists(Tracelet& t,
                                       NormalizedInstruction& i) {
  analyzeClassExistsImpl(i);
}

void TranslatorX64::analyzeInterfaceExists(Tracelet& t,
                                           NormalizedInstruction& i) {
  analyzeClassExistsImpl(i);
}

void TranslatorX64::analyzeTraitExists(Tracelet& t,
                                       NormalizedInstruction& i) {
  analyzeClassExistsImpl(i);
}

static int64 classExistsSlow(const StringData* name, bool autoload,
                             Attr typeAttr) {
  bool ret = Unit::classExists(name, autoload, typeAttr);
  // XXX: do we need to decref this during an exception?
  if (name->decRefCount() == 0) {
    const_cast<StringData*>(name)->release();
  }
  return ret;
}

void TranslatorX64::translateClassExistsImpl(const Tracelet& t,
                                             const NormalizedInstruction& i,
                                             Attr typeAttr) {
  const int nameIdx = 1;
  const int autoIdx = 0;
  const StringData* name = i.inputs[nameIdx]->rtt.valueString();
  ASSERT(IMPLIES(name, name->isStatic()));
  const int autoload = i.inputs[autoIdx]->rtt.valueBoolean();

  ScratchReg scratch(m_regMap);
  if (name != NULL && autoload != RuntimeType::UnknownBool) {
    ASSERT(i.fuseBranch);
    const Attr attrNotClass = Attr(AttrTrait | AttrInterface);
    const bool isClass = typeAttr == AttrNone;
    using namespace TargetCache;
    Stats::emitInc(a, Stats::Tx64_ClassExistsFast);
    CacheHandle ch = allocKnownClass(name);

    {
      DiamondReturn astubsRet;
      a.  load_reg64_disp_reg64(rVmTl, ch, *scratch);
      a.  test_reg64_reg64(*scratch, *scratch);
      if (autoload) {
        UnlikelyIfBlock<CC_Z> ifNull(a, astubs, &astubsRet);
        if (false) {
          Class** c = NULL;
          UNUSED Class* ret = lookupKnownClass<true>(c, name, false);
        }
        Stats::emitInc(astubs, Stats::TgtCache_ClassExistsMiss);
        // If the class exists after autoloading, the helper will
        // return the Class's flags. Otherwise, it will return a set
        // of flags such that our flag check at the join point below
        // will fail.
        EMIT_CALL(astubs, (lookupKnownClass_func_t)lookupKnownClass<true>,
                   RPLUS(rVmTl, ch),
                   IMM((uintptr_t)name),
                   IMM(isClass));
        recordReentrantStubCall(i);
        emitMovRegReg(astubs, rax, *scratch);
      } else {
        UnlikelyIfBlock<CC_Z> ifNull(a, astubs, &astubsRet);
        // This isn't really a traditional slow path, count as a hit
        Stats::emitInc(astubs, Stats::TgtCache_ClassExistsHit);
        // Provide flags so the check back in a fails
        emitImmReg(astubs, isClass ? attrNotClass : AttrNone, *scratch);
      }
      // If we don't take the slow/NULL path, load the Class's attrs
      // into *scratch to prepare for the flag check.
      Stats::emitInc(a, Stats::TgtCache_ClassExistsHit);
      a.  load_reg64_disp_reg64(*scratch, Class::preClassOff(),
                                *scratch);
      a.  load_reg64_disp_reg32(*scratch, PreClass::attrsOffset(),
                                *scratch);
    }

    if (i.changesPC) {
      fuseBranchSync(t, i);
    }
    a.    test_imm32_reg32(isClass ? attrNotClass : typeAttr, *scratch);
    ConditionCode cc = isClass ? CC_Z : CC_NZ;
    if (i.changesPC) {
      fuseBranchAfterBool(t, i, cc);
    } else {
      a.  setcc(cc, *scratch);
      a.  mov_reg8_reg64_unsigned(*scratch, *scratch);
      m_regMap.bindScratch(scratch, i.outStack->location, KindOfBoolean,
                           RegInfo::DIRTY);
    }
  } else {
    ASSERT(!i.fuseBranch);
    Stats::emitInc(a, Stats::Tx64_ClassExistsSlow);
    if (false) {
      UNUSED bool ret = false;
      ret = classExistsSlow(name, ret, typeAttr);
    }
    EMIT_CALL(a, classExistsSlow,
               V(i.inputs[nameIdx]->location),
               V(i.inputs[autoIdx]->location),
               IMM(typeAttr));
    recordReentrantCall(i);
    // Our helper decrefs the string
    m_regMap.bind(rax, i.outStack->location, KindOfBoolean, RegInfo::DIRTY);
  }
}

void TranslatorX64::translateClassExists(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  translateClassExistsImpl(t, i, AttrNone);
}

void TranslatorX64::translateInterfaceExists(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  translateClassExistsImpl(t, i, AttrInterface);
}

void TranslatorX64::translateTraitExists(const Tracelet& t,
                                         const NormalizedInstruction& i) {
  translateClassExistsImpl(t, i, AttrTrait);
}

// Helper function for static property access.  This function emits code
// which leaves a pointer to the static property for clsInput::$propInput in
// register scr. We destroy scr early on, yet do not consume inputs until
// later, so scr must not alias an input register.  This also handles
// the decref for the case where prop is not a static string.
void TranslatorX64::emitStaticPropInlineLookup(const NormalizedInstruction& i,
                                               const DynLocation& clsInput,
                                               const DynLocation& propInput,
                                               PhysReg scr) {
  const Class* cls = clsInput.rtt.valueClass();
  const StringData* propName = propInput.rtt.valueString();
  using namespace TargetCache;
  CacheHandle ch;

  ASSERT(cls && propName);
  // Use the uniquely known cls / prop to generate a single cache per prop
  const StringData* clsName = cls->preClass()->name();
  string sds(Util::toLower(clsName->data()) + ":" +
             string(propName->data(), propName->size()));
  StringData sd(sds.c_str(), sds.size(), AttachLiteral);
  ch = SPropCache::alloc(&sd);
  SKTRACE(1, i.source, "SPropInlineLookup %s %d\n", sd.data(), int(ch));

  Stats::emitInc(a, Stats::TgtCache_SPropHit);

  // For the simple case of statically known class and prop name, we inline
  // the target cache lookup, and outline the miss case.
  // Load the TV pointer out of the thread-private tl_targetCaches.
  BOOST_STATIC_ASSERT((offsetof(SPropCache, m_tv) == 0));
  a.   load_reg64_disp_reg64(rVmTl, ch, scr);
  a.   test_reg64_reg64(scr, scr);

  // Call the slow path.
  {
    UnlikelyIfBlock<CC_Z> shucks(a, astubs);

    // Precondition for this lookup - we don't need to pass the preClass,
    // as we only translate in class lookups.
    ASSERT(cls == arGetContextClass(curFrame()));
    if (false) { // typecheck
      StringData *data = NULL;
      SPropCache::lookup(ch, cls, data);
    }

    EMIT_CALL(astubs, (TCA)SPropCache::lookup,
               IMM(ch), V(clsInput.location), IMM(uint64_t(propName)));
    recordReentrantStubCall(i);
    emitMovRegReg(astubs, rax, scr);

    // We're consuming the name as input, but it is static, no decref needed
    ASSERT(propInput.rtt.valueString()->isStatic());
    // astubs.  jmp(a.code.frontier); -- implicit
  }
}

void TranslatorX64::analyzeCGetS(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  ASSERT(i.inputs[0]->valueType() == KindOfClass);
  ASSERT(i.outStack);
  const Class* cls = i.inputs[0]->rtt.valueClass();
  const StringData* propName = i.inputs[1]->rtt.valueString();
  i.m_txFlags = supportedPlan(cls && propName &&
                              (arGetContextClass(curFrame()) == cls));
}

void TranslatorX64::translateCGetS(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  ScratchReg sprop(m_regMap);
  emitStaticPropInlineLookup(i, *i.inputs[0], *i.inputs[1], *sprop);
  emitDerefIfVariant(a, *sprop);
  emitIncRefGeneric(*sprop, 0);
  // Finally copy the thing to the stack
  int stackDest = 2 * sizeof(Cell) - sizeof(Cell); // popped - pushed
  emitCopyToStack(a, i, *sprop, stackDest);
}

void TranslatorX64::analyzeSetS(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 3);
  ASSERT(i.inputs[1]->valueType() == KindOfClass);
  ASSERT(i.outStack);
  const Class* cls = i.inputs[1]->rtt.valueClass();
  const StringData* propName = i.inputs[2]->rtt.valueString();
  i.m_txFlags = supportedPlan(cls && propName &&
                              (arGetContextClass(curFrame()) == cls));
}

void TranslatorX64::translateSetS(const Tracelet& t,
                                  const NormalizedInstruction& i) {
  ScratchReg sprop(m_regMap);
  const RuntimeType& rhsType = i.inputs[0]->rtt;
  emitStaticPropInlineLookup(i, *i.inputs[1], *i.inputs[2], *sprop);

  ASSERT(m_regMap.getInfo(*sprop)->m_state == RegInfo::SCRATCH);
  ASSERT(!rhsType.isVariant());

  m_regMap.allocOutputRegs(i);
  PhysReg rhsReg = getReg(i.inputs[0]->location);
  PhysReg outReg = getReg(i.outStack->location);
  emitTvSet(i, rhsReg, rhsType.outerType(), *sprop);
  ASSERT(i.inputs[2]->location == i.outStack->location);
  emitMovRegReg(rhsReg, outReg);
}

void TranslatorX64::analyzeSetG(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  i.m_txFlags = supportedPlan(
    i.inputs[1]->isString() &&
    !i.inputs[0]->isVariant()
  );
  if (i.m_txFlags) i.manuallyAllocInputs = true;
}

void TranslatorX64::translateSetG(const Tracelet& t,
                                  const NormalizedInstruction& i) {
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.inputs.size() == 2);
  ASSERT(i.inputs[1]->isString());
  ASSERT(i.inputs[1]->location == i.outStack->location);

  const DataType type = i.inputs[0]->rtt.outerType();

  /*
   * Grab the global from the target cache; rax will get a pointer to
   * the TypedValue in the globals array, maybe newly created as a
   * null.
   */
  emitGetGlobal(i, 1, true /* allowCreate */);
  ScratchReg raxSaver(m_regMap, rax);
  m_regMap.allocInputReg(i, 0);
  PhysReg src = getReg(i.inputs[0]->location);
  m_regMap.allocOutputRegs(i);
  PhysReg out = getReg(i.outStack->location);

  emitTvSet(i, src, type, rax);
  emitMovRegReg(src, out);
}

static bool
isNormalPropertyAccess(const NormalizedInstruction& i,
                       int propInput,
                       int objInput) {
  const LocationCode lcode = i.immVec.locationCode();
  return
    i.immVecM.size() == 1 &&
    (lcode == LC || lcode == LL || lcode == LR) &&
    mcodeMaybePropName(i.immVecM[0]) &&
    i.inputs[propInput]->isString() &&
    i.inputs[objInput]->valueType() == KindOfObject;
}

static bool isContextFixed() {
  // Translations for pseudomains don't have a fixed context class
  return !curFunc()->isPseudoMain();
}

static Slot
getPropertyOffset(const NormalizedInstruction& i,
                  int propInput, int objInput) {
  ASSERT(isNormalPropertyAccess(i, propInput, objInput));
  // The context class must be fixed
  if (!isContextFixed()) {
    return kInvalidSlot;
  }
  // Property name and base class must be known at compile time
  const StringData* name = i.inputs[propInput]->rtt.valueString();
  const Class* baseClass = i.inputs[objInput]->rtt.valueClass();
  if (name == NULL || baseClass == NULL) {
    return kInvalidSlot;
  }
  bool accessible;
  Class* ctx = arGetContextClass(curFrame());
  // If we are not in repo-authoriative mode, we need to check that
  // baseClass cannot change in between requests
  if (!RuntimeOption::RepoAuthoritative ||
      !(baseClass->preClass()->attrs() & AttrUnique)) {
    if (!ctx) return kInvalidSlot;
    if (!ctx->classof(baseClass)) {
      if (baseClass->classof(ctx)) {
        // baseClass can change on us in between requests, but since
        // ctx is an ancestor of baseClass we can make the weaker
        // assumption that the object is an instance of ctx
        baseClass = ctx;
      } else {
        // baseClass can change on us in between requests and it is
        // not related to ctx, so bail out
        return kInvalidSlot;
      }
    }
  }
  // Lookup the index of the property based on ctx and baseClass
  Slot idx = baseClass->getDeclPropIndex(ctx, name, accessible);
  // If we couldn't find a property that is accessible in the current
  // context, bail out
  if (idx == kInvalidSlot || !accessible) {
    return kInvalidSlot;
  }
  // If it's a declared property we're good to go: even if a subclass
  // redefines an accessible property with the same name it's guaranteed
  // to be at the same offset
  return baseClass->declPropOffset(idx);
}

static bool
isSupportedCGetMProp(const NormalizedInstruction& i) {
  if (i.inputs.size() != 2) return false;
  SKTRACE(2, i.source, "CGetM prop candidate: prop supported: %d, "
                       "in[0] %s in[1] %s\n",
          mcodeMaybePropName(i.immVecM[0]),
          i.inputs[0]->rtt.pretty().c_str(),
          i.inputs[1]->rtt.pretty().c_str());
  return isNormalPropertyAccess(i, 1, 0) && !curFunc()->isPseudoMain();
}

static bool
isSupportedCGetM_LE(const NormalizedInstruction& i) {
  if (i.inputs.size() > 2) return false;

  // Return true iff this is a CGetM <H E> instruction where
  // the base is an array and the key is an int or string
  return
    i.immVec.locationCode() == LL &&
    i.immVecM.size() == 1 &&
    mcodeMaybeArrayKey(i.immVecM[0]) &&
    i.inputs[0]->valueType() == KindOfArray && // base
    (i.inputs[1]->isInt() || i.inputs[1]->isString()); // key
}

static bool
isSupportedCGetM_LEE(const NormalizedInstruction& i) {
  if (i.inputs.size() > 3) return false;

  // Return true iff this is a CGetM <L E E> instruction where the
  // base is an array, the first key is a int and the second key is a
  // string.
  return
    i.immVec.locationCode() == LL &&
    i.immVecM.size() == 2 &&
    mcodeMaybeArrayIntKey(i.immVecM[0]) &&
    mcodeMaybeArrayStringKey(i.immVecM[1]) &&
    i.inputs[0]->valueType() == KindOfArray &&
    i.inputs[1]->isInt() && // first key
    i.inputs[2]->isString(); // second key
}

static bool
isSupportedCGetM_RE(const NormalizedInstruction& i) {
  if (i.inputs.size() > 2) return false;
  return
    i.immVec.locationCode() == LR &&
    i.immVecM.size() == 1 &&
    mcodeMaybeArrayKey(i.immVecM[0]) &&
    i.inputs[0]->valueType() == KindOfArray && // base
    (i.inputs[1]->isInt() || i.inputs[1]->isString()); // key;
}

static bool
isSupportedCGetM_GE(const NormalizedInstruction& i) {
  if (i.inputs.size() > 2) return false;
  const LocationCode lcode = i.immVec.locationCode();
  return
    (lcode == LGC || lcode == LGL) &&
    i.immVecM.size() == 1 &&
    mcodeMaybeArrayKey(i.immVecM[0]) &&
    i.inputs[0]->isString() && // name input
    (i.inputs[1]->isInt() || i.inputs[1]->isString()); // key input
}

static bool
isSupportedCGetM(const NormalizedInstruction& i) {
  return isSupportedCGetM_LE(i) || isSupportedCGetM_RE(i) ||
    isSupportedCGetM_LEE(i) || isSupportedCGetMProp(i) ||
    isSupportedCGetM_GE(i);
}

void
TranslatorX64::analyzeCGetM(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(isSupportedCGetM(i));
  if (i.m_txFlags) i.manuallyAllocInputs = true;
}

static const char* getContextName() {
  ASSERT(isContextFixed());
  Class* ctx = arGetContextClass(curFrame());
  return ctx ? ctx->name()->data() : ":anonymous:";
}

static String propCacheName(const StringData* name) {
  if (name == NULL) {
    return String();
  }
  return String(string(name->data()) + ":" + Util::toLower(getContextName()));
}

void
TranslatorX64::emitPropGet(const NormalizedInstruction& i,
                           const DynLocation& base,
                           PhysReg fieldAddr,
                           const Location& outLoc) {
  ASSERT(outLoc.isStack());
  PhysReg baseReg = getReg(base.location);

  emitDerefIfVariant(a, fieldAddr);
  // We may be creating a reference to the field.
  emitIncRefGeneric(fieldAddr, 0);

  PhysReg stackOutReg;
  ScratchReg scratch(m_regMap);
  int stackOutDisp;
  locToRegDisp(outLoc, &stackOutReg, &stackOutDisp);
  emitCopyTo(a, fieldAddr, 0, stackOutReg, stackOutDisp, *scratch);

  if (!base.isLocal()) {
    // Release the base
    emitDecRef(i, baseReg, base.outerType());
  }
}

static void
raiseUndefProp(ObjectData* base, const StringData* name) {
  VMRegAnchor _;
  static_cast<Instance*>(base)->raiseUndefProp(name);
}

void
TranslatorX64::translateCGetMProp(const Tracelet& t,
                                  const NormalizedInstruction& i) {
  using namespace TargetCache;
  ASSERT(i.inputs.size() == 2 && i.outStack);

  const int kBaseIdx = 0;
  const DynLocation& base = *i.inputs[kBaseIdx];
  const DynLocation& prop = *i.inputs[1];
  const Location& baseLoc = base.location;
  const Location& propLoc = prop.location;
  const Location& outLoc  = i.outStack->location;
  const Slot propOffset   = getPropertyOffset(i, 1, 0);

  if (propOffset != kInvalidSlot && i.immVec.locationCode() == LC) {
    m_regMap.allocInputReg(i, 0);
    ASSERT(!base.isLocal() && !base.isVariant());
    Stats::emitInc(a, Stats::Tx64_PropGetFast);
    ScratchReg fieldAddr(m_regMap);
    a.lea_reg64_disp_reg64(getReg(baseLoc), int(propOffset), *fieldAddr);
    // Still have to check for uninit
    a.cmp_imm32_disp_reg32(KindOfUninit, TVOFF(m_type), *fieldAddr);
    {
      UnlikelyIfBlock<CC_Z> ifUninit(a, astubs);
      EMIT_CALL(astubs, raiseUndefProp,
                V(baseLoc), IMM((uint64_t)prop.rtt.valueString()));
      recordReentrantStubCall(i);
      emitImmReg(astubs, (intptr_t)&init_null_variant, *fieldAddr);
    }
    emitPropGet(i, base, *fieldAddr, outLoc);
  } else {
    int args[2];
    args[0] = base.isVariant() ? ArgAnyReg : 1;
    args[1] = 2;
    allocInputsForCall(i, args);
    Stats::emitInc(a, Stats::Tx64_PropGetSlow);
    bool useCtx = !isContextFixed();
    const StringData* name = prop.rtt.valueString();
    ASSERT(name == NULL || name->isStatic());

    CacheHandle ch;
    TargetCache::pcb_lookup_func_t lookupFn = propLookupPrep(
      ch, useCtx ? name : propCacheName(name).get(),
      base.isLocal() ? BASE_LOCAL : BASE_CELL,
      useCtx ? DYN_CONTEXT : STATIC_CONTEXT,
      name ? STATIC_NAME : DYN_NAME);
    if (useCtx) {
      EMIT_CALL(a,
                 lookupFn,
                 IMM(ch),
                 base.isVariant() ? DEREF(baseLoc) : V(baseLoc),
                 V(propLoc),
                 A(outLoc),
                 R(rVmFp));
    } else {
      EMIT_CALL(a,
                 lookupFn,
                 IMM(ch),
                 base.isVariant() ? DEREF(baseLoc) : V(baseLoc),
                 V(propLoc),
                 A(outLoc));
    }
    recordReentrantCall(i);
  }
  m_regMap.invalidate(i.outStack->location);
}

void
TranslatorX64::emitArrayElem(const NormalizedInstruction& i,
                             const DynLocation* baseInput,
                             PhysReg baseReg,
                             const DynLocation* keyIn,
                             const Location& outLoc) {
  // Let the array helpers handle refcounting logic: key down,
  // return value up.
  SKTRACE(1, i.source, "emitCGetM: committed to unary load\n");
  bool decRefBase = baseInput->isStack();
  PhysReg decRefReg = noreg;
  if (decRefBase) {
    // We'll need to decref the base after the call. Make sure we hold
    // on to the value if it's a variant or from a global.
    if (isSupportedCGetM_GE(i)) {
      decRefReg = baseReg;
    } else if (baseInput->isVariant()) {
      decRefReg = getReg(baseInput->location);
    }
    if (decRefReg != noreg && kCallerSaved.contains(decRefReg)) {
      a.    pushr(decRefReg);
      a.    sub_imm32_reg64(8, rsp);
    }
  }

  void* fptr;
  if (keyIn->isInt()) {
    fptr = (void*)array_getm_i;
  } else {
    ASSERT(keyIn->isString());
    bool decRefKey = keyIn->isStack();
    bool doIntCheck = !i.hasConstImm;
    fptr = (decRefKey ?
            (doIntCheck ? (void*)array_getm_s : (void*)array_getm_s_fast) :
            (doIntCheck ? (void*)array_getm_s0 : (void*)array_getm_s0_fast));
  }
  if (false) { // type-check
    void *a;
    TypedValue tv;
    array_getm_i(a, 1, &tv);
    StringData *sd = NULL;
    array_getm_s(a, sd, &tv);
    array_getm_s0(a, sd, &tv);
  }
  EMIT_CALL(a, fptr, R(baseReg), V(keyIn->location), A(outLoc));
  recordReentrantCall(i);
  m_regMap.invalidate(outLoc);

  if (decRefBase) {
    // For convenience of decRefs, the callees return the ArrayData*...
    PhysReg base = rax;
    // but if it was boxed or from a global, we need to get the
    // original address back...
    if (decRefReg != noreg) {
      if (kCallerSaved.contains(decRefReg)) {
        a.    add_imm32_reg64(8, rsp);
        a.    popr(rax);
      } else {
        base = decRefReg;
      }
    }
    emitDecRef(i, base, KindOfArray);
  }
}

void
TranslatorX64::translateCGetM_LEE(const Tracelet& t,
                                  const NormalizedInstruction& i) {
  ASSERT(isSupportedCGetM_LEE(i));

  const DynLocation& array = *i.inputs[0];
  const DynLocation& key1  = *i.inputs[1];
  const DynLocation& key2  = *i.inputs[2];
  const Location& outLoc    = i.outStack->location;
  ASSERT(key1.isInt() && key2.isString());

  TRACE(1, "nvGet2 outLoc: (%s, %lld)\n", outLoc.spaceName(), outLoc.offset);
  if (false) { // typeCheck
    ArrayData *a;
    TypedValue tv;
    StringData *sd = NULL;
    array_getm_is(a, 666, sd, &tv);
    array_getm_is0(a, 666, sd, &tv);
  }
  void* fptr;
  // array_getm_is incRefs the return value if appropriate and
  // it decRefs the string key for us. We don't need to decRef
  // the array because it was passed via a local.
  bool decRefStrKey =
    key2.isStack() && key2.rtt.valueString() == NULL;
  fptr = decRefStrKey ? ((void*)array_getm_is) : ((void*)array_getm_is0);

  int args[3];
  args[0] = array.isVariant() ? ArgAnyReg : 0;
  args[1] = 1;
  args[2] = 2;
  allocInputsForCall(i, args);

  EMIT_CALL(a, fptr,
            array.isVariant() ? DEREF(array.location) : V(array.location),
            V(key1.location),
            V(key2.location),
            A(outLoc));
  recordReentrantCall(i);
  m_regMap.invalidate(outLoc);
}

void TranslatorX64::translateCGetM_GE(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  const int nameIdx = 0;
  const int keyIdx = 1;
  const DynLocation& name = *i.inputs[nameIdx];
  const DynLocation& key  = *i.inputs[keyIdx];
  const Location& outLoc  = i.outStack->location;

  emitGetGlobal(i, nameIdx, false /* allowCreate */);
  ScratchReg holdRax(m_regMap, rax);

  a.  test_reg64_reg64(rax, rax);
  DiamondReturn noGlobalRet;
  {
    UnlikelyIfBlock<CC_Z> ifNoGlobal(a, astubs, &noGlobalRet);
    if (const StringData* name = i.inputs[nameIdx]->rtt.valueString()) {
      EMIT_CALL(astubs, raiseUndefVariable, IMM((uint64_t)name));
    } else {
      m_regMap.allocInputReg(i, nameIdx);
      PhysReg nameReg = getReg(i.inputs[nameIdx]->location);
      EMIT_CALL(astubs, raiseUndefVariable, R(nameReg));
    }
    recordReentrantStubCall(i);

    emitStoreNull(astubs, outLoc);
    m_regMap.invalidate(outLoc);
    // The DiamondReturn patches the jump here to return to the code
    // after this whole function.
  }

  emitIncRefGeneric(rax, 0);
  m_regMap.allocInputReg(i, keyIdx, argNumToRegName[1]);
  // We're going to be making a function call in both branches so
  // let's only emit the spilling code once.
  emitCallSaveRegs();
  a.    cmp_imm32_disp_reg32(KindOfArray, TVOFF(m_type), rax);
  DiamondReturn notArrayRet;
  {
    UnlikelyIfBlock<CC_NZ> ifNotArray(a, astubs, &notArrayRet);
    ASSERT(key.isString() || key.isInt());
    void* fptr =
      key.isString() ? (void*)non_array_getm_s : (void*)non_array_getm_i;
    EMIT_CALL(astubs, (TCA)fptr, R(rax), V(key.location), A(outLoc));
    recordReentrantStubCall(i);
  }
  a.load_reg64_disp_reg64(rax, TVOFF(m_data), rax);
  emitArrayElem(i, &name /* XXX unused in emitArrayElem for GE */,
                rax, &key, outLoc);
}

void
TranslatorX64::translateCGetM(const Tracelet& t,
                              const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() >= 2);
  ASSERT(i.outStack);

  if (isSupportedCGetMProp(i)) {
    translateCGetMProp(t, i);
    return;
  }
  if (isSupportedCGetM_LEE(i)) {
    translateCGetM_LEE(t, i);
    return;
  }
  if (isSupportedCGetM_GE(i)) {
    translateCGetM_GE(t, i);
    return;
  }

  // Remaining case handles LE and RE.
  ASSERT(isSupportedCGetM_LE(i) || isSupportedCGetM_RE(i));

  const DynLocation& base = *i.inputs[0];
  const DynLocation& key  = *i.inputs[1];
  int args[2];
  args[0] = base.isVariant() ? ArgAnyReg : 0;
  args[1] = 1;
  allocInputsForCall(i, args);

  PhysReg baseReg = getReg(base.location);
  LazyScratchReg baseScratch(m_regMap);
  if (base.isVariant()) {
    baseScratch.alloc();
    emitDeref(a, baseReg, *baseScratch);
    baseReg = *baseScratch;
  }
  emitArrayElem(i, &base, baseReg, &key, i.outStack->location);
}

static bool
isSupportedInstrVGetG(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  return (i.inputs[0]->rtt.isString());
}

void
TranslatorX64::analyzeVGetG(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = simplePlan(isSupportedInstrVGetG(i));
}

static TypedValue* lookupAddBoxedGlobal(StringData* name) {
  VarEnv* ve = g_vmContext->m_globalVarEnv;
  TypedValue* r = ve->lookupAdd(name);
  if (r->m_type != KindOfRef) {
    tvBox(r);
  }
  LITSTR_DECREF(name);
  return r;
}

void
TranslatorX64::translateVGetG(const Tracelet& t,
                              const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack);
  ASSERT(i.outStack->isVariant());
  ASSERT(i.inputs[0]->location == i.outStack->location);

  using namespace TargetCache;
  const StringData* maybeName = i.inputs[0]->rtt.valueString();
  if (!maybeName) {
    EMIT_CALL(a, lookupAddBoxedGlobal, V(i.inputs[0]->location));
    recordCall(i);
  } else {
    CacheHandle ch = BoxedGlobalCache::alloc(maybeName);

    if (false) { // typecheck
      StringData *key = NULL;
      TypedValue UNUSED *glob = BoxedGlobalCache::lookupCreate(ch, key);
    }
    SKTRACE(1, i.source, "ch %d\n", ch);
    EMIT_CALL(a, BoxedGlobalCache::lookupCreate,
               IMM(ch),
               V(i.inputs[0]->location));
    recordCall(i);
  }
  m_regMap.bind(rax, i.outStack->location, KindOfRef, RegInfo::DIRTY);
  emitIncRefGeneric(rax, 0);
  emitDeref(a, rax, rax);
}

void
TranslatorX64::translateVGetM(const Tracelet& t,
                              const NormalizedInstruction& i) {
  NOT_REACHED();
}

static TypedValue* lookupGlobal(StringData* name) {
  VarEnv* ve = g_vmContext->m_globalVarEnv;
  TypedValue* r = ve->lookup(name);
  // If the global didn't exist, we need to leave name un-decref'd for
  // the caller to raise warnings.
  if (r) {
    LITSTR_DECREF(name);
    if (r->m_type == KindOfRef) r = r->m_data.ptv;
  }
  return r;
}

static TypedValue* lookupAddGlobal(StringData* name) {
  VarEnv* ve = g_vmContext->m_globalVarEnv;
  TypedValue* r = ve->lookupAdd(name);
  if (r->m_type == KindOfRef) r = r->m_data.ptv;
  LITSTR_DECREF(name);
  return r;
}

/*
 * Look up a global in the TargetCache with the name
 * i.inputs[nameIdx].  If `allowCreate' is true, also creates it.  If
 * we don't create the global, the input name is not decref'd yet.
 */
void
TranslatorX64::emitGetGlobal(const NormalizedInstruction& i, int nameIdx,
    bool allowCreate) {
  using namespace TargetCache;
  ASSERT(i.inputs.size() > size_t(nameIdx));
  ASSERT(i.inputs[nameIdx]->isString());

  const StringData *maybeName = i.inputs[nameIdx]->rtt.valueString();
  if (!maybeName) {
    m_regMap.allocInputReg(i, nameIdx, argNumToRegName[0]);
    // Always do a lookup when there's no statically-known name.
    // There's not much we can really cache here right now anyway.
    EMIT_CALL(a, allowCreate ? lookupAddGlobal : lookupGlobal,
                  V(i.inputs[nameIdx]->location));
    recordCall(i);
    return;
  }

  CacheHandle ch = GlobalCache::alloc(maybeName);
  if (false) { // typecheck
    StringData* UNUSED key = NULL;
    TypedValue* UNUSED glob = GlobalCache::lookup(ch, key);
    TypedValue* UNUSED glob2 = GlobalCache::lookupCreate(ch, key);
  }
  SKTRACE(1, i.source, "ch %d\n", ch);
  EMIT_CALL(a, allowCreate ? GlobalCache::lookupCreate
                            : GlobalCache::lookup,
             IMM(ch),
             IMM((uint64_t)maybeName));
  recordCall(i);
}

static bool
isSupportedInstrCGetG(const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  return (i.inputs[0]->rtt.isString());
}

void
TranslatorX64::analyzeCGetG(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = simplePlan(isSupportedInstrCGetG(i));
  if (i.m_txFlags) i.manuallyAllocInputs = true;
}

void
TranslatorX64::translateCGetG(const Tracelet& t,
                              const NormalizedInstruction& i) {
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.inputs[0]->isString());

  emitGetGlobal(i, 0, false /* allowCreate */);
  ScratchReg raxHolder(m_regMap, rax);

  // If non-null, rax now points to the in-memory location of the
  // object of unknown type. lookup() has already decref'd the name.
  a.  test_reg64_reg64(rax, rax);
  DiamondReturn astubsRet;
  {
    UnlikelyIfBlock<CC_Z> ifNotRax(a, astubs, &astubsRet);
    if (!i.inputs[0]->rtt.valueString()) {
      m_regMap.allocInputReg(i, 0);
      PhysReg reg = getReg(i.inputs[0]->location);
      emitDecRef(astubs, i, reg, BitwiseKindOfString);
    }
    // TODO: if (MoreWarnings) raise a undefined variable warning.
    // (Note: when changing this remember to change the Simple flag to
    // Supported in analyze.)
    emitStoreNull(astubs, vstackOffset(i, 0), rVmSp);
    m_regMap.invalidate(i.outStack->location);
  }

  emitCopyToStack(a, i, rax, 0);
  emitIncRefGeneric(rax, 0);
  m_regMap.invalidate(i.outStack->location);
}

void TranslatorX64::analyzeFPassL(Tracelet& t,
                                  NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    analyzeVGetL(t, ni);
  } else {
    analyzeCGetL(t, ni);
  }
}

void TranslatorX64::translateFPassL(const Tracelet& t,
                                    const NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    translateVGetL(t, ni);
  } else {
    translateCGetL(t, ni);
  }
}

void TranslatorX64::analyzeFPassS(Tracelet& t,
                                  NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    // We need a VGetS translation.
    ni.m_txFlags = Interp;
  } else {
    analyzeCGetS(t, ni);
  }
}

void TranslatorX64::translateFPassS(const Tracelet& t,
                                    const NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    ASSERT(false);
  } else {
    translateCGetS(t, ni);
  }
}

void TranslatorX64::analyzeFPassG(Tracelet& t,
                                  NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    analyzeVGetG(t, ni);
  } else {
    analyzeCGetG(t, ni);
  }
}

void TranslatorX64::translateFPassG(const Tracelet& t,
                                    const NormalizedInstruction& ni) {
  if (ni.preppedByRef) {
    translateVGetG(t, ni);
  } else {
    translateCGetG(t, ni);
  }
}

void
TranslatorX64::analyzeIssetM(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = simplePlan(
    i.inputs.size() == 2 &&
    i.immVec.locationCode() == LL &&
    i.immVecM.size() == 1 &&
    mcodeMaybeArrayKey(i.immVecM[0]) &&
    i.inputs[0]->valueType() == KindOfArray &&
    (i.inputs[1]->isString() || i.inputs[1]->isInt())
  );
}

void
TranslatorX64::translateIssetM(const Tracelet& t,
                               const NormalizedInstruction& i) {
  const DynLocation& base = *i.inputs[0];
  const DynLocation& key  = *i.inputs[1];

  PhysReg arrReg = getReg(base.location);

  ScratchReg scratch(m_regMap);
  if (base.isVariant()) {
    emitDeref(a, arrReg, *scratch);
    arrReg = *scratch;
    SKTRACE(1, i.source, "loaded variant\n");
  }

  typedef uint64 (*HelperFunc)(const void* arr, StringData* sd);
  HelperFunc helper = NULL;
  if (key.isInt()) {
    helper = (HelperFunc)array_issetm_i;
  } else {
    bool doIntCheck = !i.hasConstImm;
    if (key.isStack()) {
      // on the stack, we need to decref strings.
      helper = doIntCheck ? array_issetm_s : array_issetm_s_fast;
    } else {
      ASSERT(key.isLocal() || key.location.isLiteral());
      helper = doIntCheck ? array_issetm_s0 : array_issetm_s0_fast;
    }
  }
  ASSERT(helper);
  // The array helpers can reenter; need to sync state.
  EMIT_CALL(a, helper, R(arrReg), V(key.location));

  // We didn't bother allocating the single output reg above;
  // it lives in rax now.
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(i.outStack->outerType() == KindOfBoolean);
  m_regMap.bind(rax, i.outStack->location, KindOfBoolean,
                RegInfo::DIRTY);
}

void TranslatorX64::analyzeCheckTypeOp(Tracelet& t,
                                       NormalizedInstruction& ni) {
  ASSERT(ni.inputs.size() == 1);

  if (ni.op() == OpIsObjectL || ni.op() == OpIsObjectC) {
    // is_object is weird because it's supposed to return false for
    // things where ObjectData::isResource() is true.  For now we only
    // translate when it is not an object.
    if (ni.inputs[0]->valueType() == KindOfObject) {
      ni.m_txFlags = Interp;
      return;
    }
  }

  if (ni.inputs[0]->isLocal()) {
    ni.manuallyAllocInputs = true;
    if (ni.op() != OpIssetL && ni.inputs[0]->rtt.isUninit()) {
      ni.m_txFlags = Supported;
    } else {
      ni.m_txFlags = Native;
    }
    return;
  }

  ni.m_txFlags = planHingesOnRefcounting(ni.inputs[0]->valueType());
}

static bool checkTypeHelper(Opcode op, DataType dt) {
  switch (op) {
  case OpIssetL:    case OpIssetC:    return !IS_NULL_TYPE(dt);
  case OpIsNullL:   case OpIsNullC:   return IS_NULL_TYPE(dt);
  case OpIsStringL: case OpIsStringC: return IS_STRING_TYPE(dt);
  case OpIsArrayL:  case OpIsArrayC:  return IS_ARRAY_TYPE(dt);
  case OpIsIntL:    case OpIsIntC:    return IS_INT_TYPE(dt);
  case OpIsBoolL:   case OpIsBoolC:   return IS_BOOL_TYPE(dt);
  case OpIsDoubleL: case OpIsDoubleC: return IS_DOUBLE_TYPE(dt);

  case OpIsObjectL: case OpIsObjectC:
    // Note: this is because we refused to translate if it was
    // actually an object for now.  (We'd need to emit some kind of
    // call to ObjectData::isResource or something.)
    return 0;
  }
  ASSERT(false);
  NOT_REACHED();
}

void
TranslatorX64::translateCheckTypeOp(const Tracelet& t,
                                    const NormalizedInstruction& ni) {
  ASSERT(ni.inputs.size() == 1);
  ASSERT(ni.outStack);

  const DataType dt    =  ni.inputs[0]->valueType();
  const bool isLocalOp = ni.inputs[0]->isLocal();
  const bool isType    =
    checkTypeHelper(ni.op(), ni.inputs[0]->valueType()) != ni.invertCond;
  const bool doUninit  = isLocalOp &&
                         ni.op() != OpIssetL &&
                         ni.inputs[0]->rtt.isUninit();

  if (!isLocalOp) {
    emitDecRef(ni, getReg(ni.inputs[0]->location), dt);
  }
  if (doUninit) {
    const StringData* name = local_name(ni.inputs[0]->location);
    ASSERT(name->isStatic());
    EMIT_CALL(a, raiseUndefVariable, IMM((uintptr_t)name));
    recordReentrantCall(ni);
  }
  m_regMap.allocOutputRegs(ni);
  if (ni.changesPC) {
    // Don't bother driving an output reg. Just take the branch
    // where it leads.
    Stats::emitInc(a, Stats::Tx64_FusedTypeCheck);
    fuseBranchAfterStaticBool(t, ni, isType);
    return;
  }
  Stats::emitInc(a, Stats::Tx64_UnfusedTypeCheck);
  emitImmReg(a, isType, getReg(ni.outStack->location));
}

/*
 * SetM. We care about two cases in particular: objects and arrays.
 */
static inline bool
isSupportedSetMArray(const NormalizedInstruction& i) {
  const ImmVector& iv = i.immVec;
  const LocationCode lcode = iv.locationCode();
  return
    i.immVecM.size() == 1 && lcode == LL &&
    mcodeMaybeArrayKey(i.immVecM[0]) &&
    i.inputs.size() == 3 &&
    i.inputs[0]->isStack() && // rhs
    i.inputs[1]->valueType() == KindOfArray && // base
    (i.inputs[2]->isInt() || i.inputs[2]->isString()); // key
}

static bool
isSupportedSetMProp(const NormalizedInstruction& i) {
  if (i.inputs.size() != 3) return false;
  SKTRACE(2, i.source, "setM prop candidate: prop supported: %d, rtt %s\n",
          mcodeMaybePropName(i.immVecM[0]),
          i.inputs[2]->rtt.pretty().c_str());
  return isNormalPropertyAccess(i, 2, 1) && !curFunc()->isPseudoMain();
}

void
TranslatorX64::emitPropSet(const NormalizedInstruction& i,
                           const DynLocation& base,
                           const DynLocation& rhs,
                           PhysReg rhsReg,
                           PhysReg fieldAddr) {
  DataType rhsType = rhs.rtt.valueType();

  if (i.outStack || rhs.isLocal()) {
    emitIncRef(rhsReg, rhsType);
  }

  // If the field is a Var, we need to dereference the Var so that
  // fieldAddr holds the address of the inner cell.
  emitDerefIfVariant(a, fieldAddr);
  // decRef the lhs.
  emitDecRefGeneric(i, fieldAddr);
  // Drop rhs on the lhs.
  a.  store_reg64_disp_reg64(rhsReg, TVOFF(m_data), fieldAddr);
  a.  store_imm32_disp_reg(rhsType, TVOFF(m_type), fieldAddr);

  if (!base.isLocal()) {
    const int kBaseIdx = 1;
    ASSERT(i.inputs[kBaseIdx] == &base);
    m_regMap.allocInputReg(i, kBaseIdx);
    PhysReg baseReg = getReg(base.location);
    emitDecRef(i, baseReg, base.outerType());
  }
}

void
TranslatorX64::translateSetMProp(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  using namespace TargetCache;
  ASSERT(i.inputs.size() == 3);

  const int kRhsIdx       = 0;
  const int kBaseIdx      = 1;
  const int kPropIdx      = 2;
  const DynLocation& val  = *i.inputs[kRhsIdx];
  const DynLocation& base = *i.inputs[kBaseIdx];
  const DynLocation& prop = *i.inputs[kPropIdx];
  const Slot propOffset   = getPropertyOffset(i, kPropIdx, 1);

  const Location& valLoc  = val.location;
  const Location& baseLoc = base.location;
  const Location& propLoc = prop.location;

  // We only combine a CGetL with a SetM if the SetM's value
  // is unused. Otherwise we'd need to do the same incRef
  // on the result of CGetL that CGetL already does for us.
  ASSERT(!i.outStack || !val.isLocal());
  // We cant get a variant unless we combined a CGetL
  ASSERT(val.isLocal() || !val.isVariant());

  bool decRefRhs = !i.outStack && !val.isLocal();

  bool fastSet = propOffset != kInvalidSlot && i.immVec.locationCode() == LC;

  int args[3];
  args[kRhsIdx] = fastSet ? ArgAnyReg : 3;
  args[kBaseIdx] = fastSet || base.isVariant() ? ArgAnyReg : 1;
  args[kPropIdx] = fastSet ? ArgDontAllocate : 2;
  allocInputsForCall(i, args);

  LazyScratchReg rhsTmp(m_regMap);
  PhysReg rhsReg = getReg(valLoc);
  if (val.isVariant()) {
    rhsTmp.alloc();
    emitDeref(a, rhsReg, *rhsTmp);
    rhsReg = *rhsTmp;
  }

  if (fastSet) {
    ASSERT(!base.isLocal() && !base.isVariant());
    Stats::emitInc(a, Stats::Tx64_PropSetFast);
    ScratchReg rField(m_regMap);
    a.lea_reg64_disp_reg64(getReg(baseLoc), int(propOffset), *rField);
    emitPropSet(i, base, val, rhsReg, *rField);
    decRefRhs = false;
  } else {

    Stats::emitInc(a, Stats::Tx64_PropSetSlow);
    bool useCtx = !isContextFixed();
    const StringData* name = prop.rtt.valueString();
    ASSERT(name == NULL || name->isStatic());

    CacheHandle ch;
    TargetCache::pcb_set_func_t setFn = propSetPrep(
      ch, useCtx ? name : propCacheName(name).get(),
      base.isLocal() || base.isVariant() ? BASE_LOCAL : BASE_CELL,
      useCtx ? DYN_CONTEXT : STATIC_CONTEXT,
      name ? STATIC_NAME : DYN_NAME);
    if (useCtx) {
      EMIT_CALL(a, setFn,
                 IMM(ch),
                 base.isVariant() ? DEREF(baseLoc) : V(baseLoc),
                 V(propLoc),
                 R(rhsReg),
                 IMM(val.rtt.valueType()),
                 R(rVmFp));
    } else {
      EMIT_CALL(a, setFn,
                 IMM(ch),
                 base.isVariant() ? DEREF(baseLoc) : V(baseLoc),
                 V(propLoc),
                 R(rhsReg),
                 IMM(val.rtt.valueType()));
    }
    recordReentrantCall(i);
    if (!base.isLocal() && base.isVariant()) {
      m_regMap.allocInputReg(i, kBaseIdx);
      emitDecRef(i, getReg(baseLoc), KindOfRef);
    }
    if (i.outStack || decRefRhs) {
      m_regMap.allocInputReg(i, kRhsIdx);
      rhsReg = getReg(valLoc);
    }
  }

  if (i.outStack) {
    ASSERT(!val.isVariant());
    m_regMap.cleanRegs(RegSet(rhsReg));
    m_regMap.invalidate(valLoc);
    m_regMap.bind(rhsReg, i.outStack->location,
                  val.valueType(), RegInfo::DIRTY);
  } else if (decRefRhs) {
    emitDecRef(i, rhsReg, val.outerType());
  }
}

void
TranslatorX64::analyzeSetM(Tracelet& t, NormalizedInstruction& i) {
  // TODO: We could be more aggressive in the translation plans when the
  // lefthand side isn't refcounted.
  ASSERT(!(isSupportedSetMProp(i) && isSupportedSetMArray(i)));
  i.m_txFlags = supportedPlan(isSupportedSetMProp(i) ||
                              isSupportedSetMArray(i));
  if (i.m_txFlags) {
    i.manuallyAllocInputs = true;
  }
}

void
TranslatorX64::translateSetM(const Tracelet& t,
                             const NormalizedInstruction& i) {
  if (isSupportedSetMProp(i)) {
    translateSetMProp(t, i);
    return;
  }

  ASSERT(i.inputs.size() == 3);
  const DynLocation& val = *i.inputs[0];
  const DynLocation& arr = *i.inputs[1];
  const DynLocation& key = *i.inputs[2];
  ASSERT(arr.isLocal());

  ASSERT(val.isLocal() || !val.isVariant());
  ASSERT(!i.outStack || !val.isLocal());

  bool forceIncDec = false;
  if (val.isLocal() &&
      (val.location == arr.location ||
       (val.isVariant() && arr.isVariant() &&
        val.valueType() == arr.valueType()))) {
    /*
     * cant avoid the inc/dec on val
     * in the case of $a[...] = $a;
     */
    forceIncDec = true;
    /* we dont allow a local input unless we also folded
       a following pop */
    ASSERT(!i.outStack);
  }

  const Location valLoc = val.location;
  const Location arrLoc = arr.location;
  const Location keyLoc = key.location;

  // The array_setm helpers will decRef any old value that is
  // overwritten if appropriate. If copy-on-write occurs, it will also
  // incRef the new array and decRef the old array for us. Finally,
  // some of the array_setm helpers will decRef the key if it is a
  // string (for cases where the key is not a local), while others do
  // not (for cases where the key is a local).
  bool useBoxedForm = arr.isVariant();
  void* fptr;
  if (false) { // helper type-checks
    TypedValue* cell = NULL;
    ArrayData* arr = NULL;
    TypedValue* rhs = NULL;
    StringData* strKey = NULL;
    UNUSED ArrayData* ret = array_setm_ik1_iv(cell, arr, 12, 3);
    ret = array_setm_ik1_v(cell, arr, 12, rhs);
    ret = array_setm_sk1_v(cell, arr, strKey, rhs);
    ret = array_setm_sk1_v0(cell, arr, strKey, rhs);
    ret = array_setm_s0k1_v(cell, arr, strKey, rhs);
    ret = array_setm_s0k1_v0(cell, arr, strKey, rhs);
    ret = array_setm_s0k1nc_v(cell, arr, strKey, rhs);
    ret = array_setm_s0k1nc_v0(cell, arr, strKey, rhs);
  }
  bool isInt = key.isInt() && IS_INT_TYPE(val.rtt.valueType());

  /*
   * The value can be passed as A, V or DEREF according to:
   *
   *                       | val.isVariant() | !val.isVariant()
   * ----------------------+-----------------+-----------------------------
   * int key + int value   | DEREF(valLoc)   | V(valLoc)
   * everything else       | V(valLoc)       | A(valLoc)
   */
  int deRefLevel = isInt + val.isVariant();

  int args[3];
  args[0] = deRefLevel == 1 ? 3 : deRefLevel == 2 ? ArgAnyReg : ArgDontAllocate;
  args[1] = useBoxedForm ? 0 : 1;
  args[2] = 2;
  allocInputsForCall(i, args);

  if (isInt) {
    // If the key and rhs are Int64, we can use a specialized helper
    fptr = (void*)array_setm_ik1_iv;
  } else {
    bool decRefKey = key.rtt.isString() && keyLoc.isStack();
    bool decRefValue = forceIncDec ||
      (!i.outStack && !val.isLocal() &&
       IS_REFCOUNTED_TYPE(val.rtt.valueType()));
    fptr = decRefValue ?
      (key.rtt.isString() ?
       (decRefKey ? (void*)array_setm_sk1_v0 :
        (i.hasConstImm ? (void*)array_setm_s0k1nc_v0 :
         (void*)array_setm_s0k1_v0)) :
       (void*)array_setm_ik1_v0) :
      (key.rtt.isString() ?
       (decRefKey ? (void*)array_setm_sk1_v :
        (i.hasConstImm ? (void*)array_setm_s0k1nc_v :
         (void*)array_setm_s0k1_v)) :
       (void*)array_setm_ik1_v);
  }

  if (forceIncDec) {
    LazyScratchReg tmp(m_regMap);
    PhysReg rhsReg = getReg(valLoc);
    if (val.isVariant()) {
      tmp.alloc();
      emitDeref(a, rhsReg, *tmp);
      rhsReg = *tmp;
    }
    emitIncRef(rhsReg, KindOfArray);
  }
  EMIT_CALL(a, fptr,
            useBoxedForm ? V(arrLoc) : IMM(0),
            useBoxedForm ? DEREF(arrLoc) : V(arrLoc),
            V(keyLoc),
            deRefLevel == 2 ? DEREF(valLoc) :
            deRefLevel == 1 ? V(valLoc) : A(valLoc));

  recordReentrantCall(i);
  // If we did not used boxed form, we need to tell the register allocator
  // to associate rax with arrLoc
  if (!useBoxedForm) {
    // The array_setm helper returns the up-to-date array pointer in rax.
    // Therefore, we can bind rax to arrLoc and mark it as dirty.
    m_regMap.markAsClean(arrLoc);
    m_regMap.bind(rax, arrLoc, KindOfArray, RegInfo::DIRTY);
  }

  if (i.outStack) {
    ASSERT(!val.isVariant());
    // Bring the output value into its correct location on the stack.
    // Note that this has to happen after all of the above, since it
    // needs to read the key from this location.
    m_regMap.allocInputReg(i, 0);
    m_regMap.bind(getReg(valLoc), i.outStack->location,
                  val.outerType(), RegInfo::DIRTY);
  }
}

void
TranslatorX64::analyzeSetOpL(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 2);
  const SetOpOp subOp = SetOpOp(i.imm[1].u_OA);
  Opcode arithOp = setOpOpToOpcodeOp(subOp);
  i.m_txFlags = nativePlan(i.inputs[0]->isInt() &&
                           i.inputs[1]->isInt() &&
                           (arithOp == OpAdd || arithOp == OpSub ||
                            arithOp == OpMul ||
                            arithOp == OpBitAnd || arithOp == OpBitOr ||
                            arithOp == OpBitXor));
}

void
TranslatorX64::translateSetOpL(const Tracelet& t,
                               const NormalizedInstruction& i) {
  const vector<DynLocation*>& inputs  = i.inputs;
  ASSERT(inputs.size() >= 2);
  ASSERT(i.outStack && i.outLocal);
  const int valIdx   = 0;
  const int localIdx = 1;
  ASSERT(inputs[localIdx]->isLocal());
  ASSERT(inputs[valIdx]->isStack());
  ASSERT(inputs[valIdx]->outerType() != KindOfRef);

  const SetOpOp subOp = SetOpOp(i.imm[1].u_OA);
  Opcode arithOp = setOpOpToOpcodeOp(subOp);
  m_regMap.allocOutputRegs(i);
  binaryArithLocal(i, arithOp, *inputs[valIdx], *inputs[localIdx],
                   *i.outStack);
}

void
TranslatorX64::analyzeIncDecL(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = nativePlan(i.inputs[0]->isInt());
}

void
TranslatorX64::translateIncDecL(const Tracelet& t,
                                const NormalizedInstruction& i) {
  const vector<DynLocation*>& inputs = i.inputs;
  ASSERT(inputs.size() == 1);
  ASSERT(i.outLocal);
  ASSERT(inputs[0]->isLocal());
  const IncDecOp oplet = IncDecOp(i.imm[1].u_OA);
  ASSERT(oplet == PreInc || oplet == PostInc || oplet == PreDec ||
         oplet == PostDec);
  ASSERT(inputs[0]->isInt() && (!i.outStack ||
                                i.outStack->isInt()));
  bool post = (oplet == PostInc || oplet == PostDec);
  bool pre  = !post;
  bool inc  = (oplet == PostInc || oplet == PreInc);

  m_regMap.allocOutputRegs(i);
  PhysReg localVal = getReg(inputs[0]->location);
  if (i.outStack && post) { // $a++, $a--
    PhysReg output   = getReg(i.outStack->location);
    emitMovRegReg(localVal, output);
  }
  if (inc) {
    a.  add_imm32_reg64(1, localVal);
  } else {
    a.  sub_imm32_reg64(1, localVal);
  }
  if (i.outStack && pre) { // --$a, ++$a
    PhysReg output   = getReg(i.outStack->location);
    emitMovRegReg(localVal, output);
  }
}

void
TranslatorX64::translateUnsetL(const Tracelet& t,
                               const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(!i.outStack && i.outLocal);
  const int locIdx = 0;
  const DynLocation& localDl = *i.inputs[locIdx];
  ASSERT(localDl.isLocal());

  // We have to mark the output register as dirty to ensure that
  // the type gets spilled at the tend of the tracelet
  m_regMap.allocOutputRegs(i);

  DataType type = localDl.outerType();
  // decRef the value that currently lives in the local if appropriate.
  if (IS_REFCOUNTED_TYPE(type)) {
    emitDecRef(i, getReg(localDl.location), type);
  }
}

void
TranslatorX64::analyzeUnsetM(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(
    i.inputs.size() == 2 &&
    i.immVec.locationCode() == LL &&
    i.immVecM.size() == 1 &&
    mcodeMaybeArrayStringKey(i.immVecM[0]) &&
    i.inputs[0]->outerType() == KindOfArray &&
    i.inputs[1]->isString()
  );
}

void
TranslatorX64::translateUnsetM(const Tracelet& t,
                               const NormalizedInstruction& i) {
  const DynLocation& base = *i.inputs[0];
  const DynLocation& key  = *i.inputs[1];

  ASSERT(!i.outStack && !i.outLocal);
  const bool decRefKey = key.isStack();
  if (decRefKey) {
    if (false) { // type check
      ArrayData *arr = NULL;
      StringData *sd = NULL;
      arr = array_unsetm_s(arr, sd);
    }
    // array_unsetm_s will decRef the key for us, if the key is present in the
    // array is will also decRef the unset value for us if appropriate. If
    // copy-on-write is triggered, it will also incRef the new array and decRef
    // the old array for us.
    EMIT_CALL(a, array_unsetm_s, V(base.location), V(key.location));
  } else {
    if (false) { // type check
      ArrayData *arr = NULL;
      StringData *sd = NULL;
      arr = array_unsetm_s0(arr, sd);
    }
    // If the key is present in the array, array_unsetm_s0 will decRef the
    // unset value for us if appropriate. If copy-on-write is triggered, it
    // will also incRef the new array and decRef the old array for us.
    EMIT_CALL(a, array_unsetm_s0, V(base.location), V(key.location));
  }
  recordReentrantCall(i);
  m_regMap.invalidate(base.location);
  m_regMap.bind(rax, base.location, KindOfArray, RegInfo::DIRTY);
}

void
TranslatorX64::analyzeReqLit(Tracelet& t, NormalizedInstruction& i,
                             InclOpFlags flags) {
  ASSERT(i.inputs.size() == 1);
  Eval::PhpFile* efile = g_vmContext->lookupIncludeRoot(
                                (StringData*)i.inputs[0]->rtt.valueString(),
                                flags, NULL);
  i.m_txFlags = supportedPlan(i.inputs[0]->isString() &&
                              i.inputs[0]->rtt.valueString() != NULL &&
                              efile &&
                              (RuntimeOption::RepoAuthoritative ||
                               RuntimeOption::ServerStatCache));
  if (efile && efile->unit()->getMainReturn()->m_type != KindOfUninit) {
    i.outStack->rtt = RuntimeType(efile->unit()->getMainReturn()->m_type);
  }

  // We don't need the reference lookupIncludeRoot made for us.
  if (efile) efile->decRef();
  i.manuallyAllocInputs = true;
}

void
TranslatorX64::analyzeReqDoc(Tracelet& t, NormalizedInstruction& i) {
  analyzeReqLit(t, i, InclOpDocRoot);
}

void
TranslatorX64::analyzeReqMod(Tracelet& t, NormalizedInstruction& i) {
  analyzeReqLit(t, i, InclOpDocRoot | InclOpLocal);
}

void
TranslatorX64::analyzeReqSrc(Tracelet& t, NormalizedInstruction& i) {
  analyzeReqLit(t, i, InclOpRelative | InclOpLocal);
}

void
TranslatorX64::translateReqLit(const Tracelet& t,
                               const NormalizedInstruction& i,
                               InclOpFlags flags) {
  bool local = flags & InclOpLocal;
  StringData *s = const_cast<StringData*>(i.inputs[0]->rtt.valueString());
  HPHP::Eval::PhpFile* efile =
    g_vmContext->lookupIncludeRoot(s, flags, NULL);
  /*
   * lookupIncludeRoot increments the refcount for us. This reference is
   * going to be burned into the translation cache. We will remove it only
   * when the file changes (via invalidateFile), and we're sure that no
   * outstanding requests are using the old code (via the Treadmill
   * module).
   */
  TRACE(1, "lookupIncludeRoot: %s -> %p c %d\n", s->data(), efile,
        efile->getRef());
  /*
   * Remember that this tracelet (not just this instruction) now depends on the
   * contents of the required file.
   */
  m_srcDB.recordDependency(efile, t.m_sk);
  Unit *unit = efile->unit();
  Func *func = unit->getMain();

  const Offset after = nextSrcKey(t, i).offset();
  TRACE(1, "requireHelper: efile %p offset %d\n", efile, after);

  if (i.skipSync) {
    /*
     * getting here means there was nothing to do between
     * the previous req and this one. Any spill code we generate
     * here would be broken (because the rbx is wrong), so
     * verify that we dont generate anything...
     */
    TCA s DEBUG_ONLY = a.code.frontier;
    syncOutputs(0);
    ASSERT(s == a.code.frontier);
  } else {
    syncOutputs(i);
  }
  ReqLitStaticArgs* args = m_globalData.alloc<ReqLitStaticArgs>();
  emitImmReg(a, (uint64_t)args, argNumToRegName[0]);
  emitCall(a, (TCA)reqLitHelper, true);

  args->m_efile = efile;
  args->m_pseudoMain = emitServiceReq(false, REQ_BIND_REQUIRE, 3,
                                      uint64_t(args),
                                      uint64_t(func), uint64_t(func->base()));
  args->m_pcOff = after;
  args->m_local = local;

  if (i.breaksBB) {
    SrcKey fallThru(curFunc(), after);
    emitBindJmp(fallThru);
  } else {
    /*
     * When we get here, rVmSp points to the actual top of stack,
     * but the rest of this tracelet assumes that rVmSp is set to
     * the top of the stack at the beginning of the tracelet, so we
     * have to fix it up here.
     *
     */
    if (!i.outStack) {
      /* as a special case, if we're followed by a pop, and
         we return a non-refcounted type, and then followed
         by another require, we can avoid the add here and the sub
         in the following require
      */
    } else {
      int delta = i.stackOff + getStackDelta(i);
      if (delta != 0) {
        // i.stackOff is in negative Cells, not bytes.
        a.    add_imm64_reg64(cellsToBytes(delta), rVmSp);
      }
    }
  }
}

void
TranslatorX64::translateReqDoc(const Tracelet& t,
                               const NormalizedInstruction& i) {
  translateReqLit(t, i, InclOpDocRoot);
}

void
TranslatorX64::translateReqMod(const Tracelet& t,
                               const NormalizedInstruction& i) {
  translateReqLit(t, i, InclOpDocRoot | InclOpLocal);
}

void
TranslatorX64::translateReqSrc(const Tracelet& t,
                               const NormalizedInstruction& i) {
  translateReqLit(t, i, InclOpRelative | InclOpLocal);
}

TCA
TranslatorX64::emitNativeTrampoline(TCA helperAddr) {
  if (!atrampolines.code.canEmit(m_trampolineSize)) {
    // not enough space to emit a trampoline, so just return the
    // helper address and emitCall will the emit the right sequence
    // to call it indirectly
    TRACE(1, "Ran out of space to emit a trampoline for %p\n", helperAddr);
    ASSERT(false);
    return helperAddr;
  }
  uint32_t index = m_numNativeTrampolines++;
  TCA trampAddr = atrampolines.code.frontier;
  if (Stats::enabled()) {
    Stats::emitInc(atrampolines, &Stats::tl_helper_counters[0], index);
    Stats::helperNames[index] = Util::getNativeFunctionName(helperAddr);
  }
  atrampolines.mov_imm64_reg((int64_t)helperAddr, reg::rax);
  atrampolines.jmp_reg(reg::rax);
  atrampolines.ud2();
  trampolineMap[helperAddr] = trampAddr;
  if (m_trampolineSize == 0) {
    m_trampolineSize = atrampolines.code.frontier - trampAddr;
    ASSERT(m_trampolineSize >= kMinPerTrampolineSize);
  }
  recordBCInstr(OpNativeTrampoline, atrampolines, trampAddr);
  return trampAddr;
}

TCA
TranslatorX64::getNativeTrampoline(TCA helperAddr) {
  if (!RuntimeOption::EvalJitTrampolines && !Stats::enabled()) {
    return helperAddr;
  }
  TCA trampAddr = (TCA)mapGet<PointerMap>(trampolineMap, helperAddr);
  if (trampAddr) {
    return trampAddr;
  }
  return emitNativeTrampoline(helperAddr);
}
void TranslatorX64::analyzeDefCls(Tracelet& t,
                                  NormalizedInstruction& i) {
  i.m_txFlags = Supported;
}

static void defClsHelper(PreClass *preClass) {
  ASSERT(tl_regState == REGSTATE_DIRTY);
  tl_regState = REGSTATE_CLEAN;
  Unit::defClass(preClass);

  /*
   * m_defClsHelper sync'd the registers for us already.  This means
   * if an exception propagates we want to leave things as
   * REGSTATE_CLEAN, since we're still in sync.  Only set it to dirty
   * if we are actually returning to run in the TC again.
   */
  tl_regState = REGSTATE_DIRTY;
}

void TranslatorX64::translateDefCls(const Tracelet& t,
                                    const NormalizedInstruction& i) {
  int cid = i.imm[0].u_IVA;
  PreClass* c = curFunc()->unit()->lookupPreClassId(cid);

  ASSERT(m_defClsHelper);

  const Opcode* after = curUnit()->at(i.source.offset());

  /*
     compute the corrected stack ptr as a pseudo-param to m_defClsHelper
     which it will store in g_vmContext, in case of fatals, or __autoload
  */
  m_regMap.cleanRegs(RegSet(rax));
  m_regMap.smashReg(rax);
  ScratchReg offset(m_regMap, rax);
  a.   lea_reg64_disp_reg64(rVmSp, -cellsToBytes(i.stackOff), rax);

  EMIT_CALL(a, m_defClsHelper, IMM((uint64)c), IMM((uint64)after));
}

void TranslatorX64::analyzeDefFunc(Tracelet& t,
                                   NormalizedInstruction& i) {
  i.m_txFlags = Supported;
}

static void defFuncHelper(Func *f) {
  f->setCached();
}

void TranslatorX64::translateDefFunc(const Tracelet& t,
                                     const NormalizedInstruction& i) {
  int fid = i.imm[0].u_IVA;
  Func* f = curFunc()->unit()->lookupFuncId(fid);

  EMIT_CALL(a, defFuncHelper, IMM((uint64)f));
  recordReentrantCall(i);
}

void
TranslatorX64::analyzeFPushFunc(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() >= 1);
  // The input might be an object implementing __invoke()
  i.m_txFlags = simplePlan(i.inputs[0]->isString());
}

void
TranslatorX64::translateFPushFunc(const Tracelet& t,
                                  const NormalizedInstruction& i) {
  using namespace TargetCache;
  CacheHandle ch = FuncCache::alloc();
  ASSERT(i.inputs.size() == 1);
  Location& inLoc = i.inputs[0]->location;

  m_regMap.allocOutputRegs(i);
  m_regMap.scrubStackRange(i.stackOff - 1,
                           i.stackOff - 1 + kNumActRecCells);
  // Popped one cell, pushed an actrec
  int startOfActRec = int(sizeof(Cell)) - int(sizeof(ActRec));
  size_t funcOff = AROFF(m_func) + startOfActRec;
  size_t thisOff = AROFF(m_this) + startOfActRec;
  emitVStackStoreImm(a, i, 0, thisOff, sz::qword, &m_regMap);
  emitPushAR(i, NULL, sizeof(Cell) /* bytesPopped */);
  if (false) { // typecheck
    StringData sd("foo");
    const UNUSED Func* f = FuncCache::lookup(ch, &sd);
  }
  SKTRACE(1, i.source, "ch %d\n", ch);
  EMIT_CALL(a, FuncCache::lookup, IMM(ch), V(inLoc));
  recordCall(i);
  emitVStackStore(a, i, rax, funcOff, sz::qword);
}

void
TranslatorX64::analyzeFPushClsMethodD(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(isContextFixed());
}

void
TranslatorX64::translateFPushClsMethodD(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  using namespace TargetCache;
  const StringData* meth = curUnit()->lookupLitstrId(i.imm[1].u_SA);
  const NamedEntityPair& np = curUnit()->lookupNamedEntityPairId(i.imm[2].u_SA);
  const StringData* cls = np.first;
  ASSERT(meth && meth->isStatic() &&
         cls && cls->isStatic());
  ASSERT(i.inputs.size() == 0);

  const Class* baseClass = Unit::lookupClass(np.second);
  bool magicCall = false;
  const Func* func = lookupImmutableMethod(baseClass, meth, magicCall,
                                           true /* staticLookup */);

  m_regMap.scrubStackRange(i.stackOff,
                           i.stackOff + kNumActRecCells);

  int startOfActRec = -int(sizeof(ActRec));
  SKTRACE(2, i.source, "FPushClsMethodD %s :: %s\n",
          cls->data(), meth->data());

  size_t clsOff  = AROFF(m_cls) + startOfActRec;
  if (func) {
    Stats::emitInc(a, Stats::TgtCache_StaticMethodBypass);
    emitPushAR(i, func, 0 /*bytesPopped*/,
               false /* isCtor */, false /* clearThis */,
               magicCall ? uintptr_t(meth) | 1 : 0 /* varEnvInvName */);
    if (!(func->attrs() & AttrStatic) &&
        !(curFunc()->attrs() & AttrStatic) &&
        curFunc()->cls() &&
        curFunc()->cls()->classof(baseClass)) {
      /* might be a non-static call */
      ScratchReg rClsScratch(m_regMap);
      PhysReg rCls = *rClsScratch;
      a.    load_reg64_disp_reg64(rVmFp, AROFF(m_cls), rCls);
      a.    test_imm32_reg64(1, rCls);
      {
        IfElseBlock<CC_NZ> ifThis(a);
        // rCls is holding $this. We should pass it to the callee
        emitIncRef(rCls, KindOfObject);
        emitVStackStore(a, i, rCls, clsOff);
        ifThis.Else();
        emitVStackStoreImm(a, i, uintptr_t(baseClass)|1, clsOff);
      }
    } else {
      emitVStackStoreImm(a, i, uintptr_t(baseClass)|1, clsOff);
    }
  } else {
    Stats::emitInc(a, Stats::TgtCache_StaticMethodHit);
    CacheHandle ch = StaticMethodCache::alloc(cls, meth, getContextName());
    ScratchReg rFunc(m_regMap);
    a.    load_reg64_disp_reg64(rVmTl, ch, *rFunc);
    a.    test_reg64_reg64(*rFunc, *rFunc);
    // Unconditionally set rCls; if we miss, the miss path will
    // clean it up for us. Careful! Flags are live. The fill path
    // has already |'ed in the necessary 1.
    ScratchReg rCls(m_regMap);
    a.    load_reg64_disp_reg64(rVmTl,
                                ch + offsetof(StaticMethodCache, m_cls),
                                *rCls);
    emitVStackStore(a, i, *rCls, clsOff);
    TCA stubsSkipRet;
    {
      UnlikelyIfBlock<CC_Z> miss(a, astubs);
      if (false) { // typecheck
        const UNUSED Func* f = StaticMethodCache::lookup(ch, np.second,
                                                         cls, meth);
      }
      EMIT_CALL(astubs,
                 StaticMethodCache::lookup,
                 IMM(ch),
                 IMM(int64(np.second)),
                 IMM(int64(cls)),
                 IMM(int64(meth)));
      recordReentrantStubCall(i);
      emitMovRegReg(astubs, rax, *rFunc);
      // NULL return means our work is done; see also
      // translateFPushClsMethodF.
      miss.reconcileEarly();
      astubs.test_reg64_reg64(*rFunc, *rFunc);
      stubsSkipRet = astubs.code.frontier;
      astubs.jz(a.code.frontier); // 1f to be patched later
    }

    {
      FreezeRegs ice(m_regMap);
      emitPushAR(i, NULL);
      size_t funcOff = AROFF(m_func) + startOfActRec;
      emitVStackStore(a, i, *rFunc, funcOff, sz::qword);
    }
    // 1:
    astubs.patchJcc(stubsSkipRet, a.code.frontier);
  }
}

void
TranslatorX64::analyzeFPushClsMethodF(Tracelet& t,
                                      NormalizedInstruction& i) {
  ASSERT(i.inputs[0]->valueType() == KindOfClass);
  i.m_txFlags = supportedPlan(
    i.inputs[1]->rtt.valueString() != NULL && // We know the method name
    i.inputs[0]->valueType() == KindOfClass &&
    i.inputs[0]->rtt.valueClass() != NULL && // We know the class name
    isContextFixed()
  );
}

void
TranslatorX64::translateFPushClsMethodF(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  using namespace TargetCache;
  ASSERT(isContextFixed());
  ASSERT(!curFunc()->isPseudoMain());
  ASSERT(curFunc()->cls() != NULL); // self:: and parent:: should only
                                    // appear in methods
  DynLocation* clsLoc = i.inputs[0];
  DynLocation* nameLoc = i.inputs[1];
  const StringData* name = nameLoc->rtt.valueString();
  ASSERT(name && name->isStatic());

  // Even though we know the Class* at compile time, it's not
  // guaranteed to be the same between requests. The name, however, is
  // fixed, so we can use that.
  const Class* cls = clsLoc->rtt.valueClass();
  ASSERT(cls);
  bool magicCall = false;
  const Func* func = lookupImmutableMethod(cls, name, magicCall,
                                           true /* staticLookup */);

  const int bytesPopped = 2 * sizeof(Cell); // [A C] popped
  const int startOfActRec = -int(sizeof(ActRec)) + bytesPopped;
  const Offset clsOff = startOfActRec + AROFF(m_cls);

  UNUSED ActRec* fp = curFrame();
  ASSERT(!fp->hasThis() || fp->getThis()->instanceof(cls));
  if (func) {
    Stats::emitInc(a, Stats::TgtCache_StaticMethodFBypass);
    emitPushAR(i, func, bytesPopped,
               false /* isCtor */, false /* clearThis */,
               magicCall ? uintptr_t(name) | 1 : 0 /* varEnvInvName */);
    ScratchReg rClsScratch(m_regMap);
    PhysReg rCls = *rClsScratch;
    a.    load_reg64_disp_reg64(rVmFp, AROFF(m_cls), rCls);
    if (!(curFunc()->attrs() & AttrStatic)) {
      ASSERT(curFunc()->cls() &&
             curFunc()->cls()->classof(cls));
      /* the context is non-static, so we have to deal
         with passing in $this or getClass($this) */
      a.    test_imm32_reg64(1, rCls);
      {
        JccBlock<CC_NZ> ifThis(a);
        // rCls is holding a real $this.
        if (func->attrs() & AttrStatic) {
          // but we're a static method, so pass getClass($this)|1
          a.load_reg64_disp_reg64(rCls, ObjectData::getVMClassOffset(), rCls);
          a.or_imm32_reg64(1, rCls);
        } else {
          // We should pass $this to the callee
          emitIncRef(rCls, KindOfObject);
        }
      }
    }
    emitVStackStore(a, i, rCls, clsOff);
    m_regMap.scrubStackRange(i.stackOff - 2,
                             i.stackOff - 2 + kNumActRecCells);
  } else {
    const StringData* clsName = cls->name();
    CacheHandle ch = StaticMethodFCache::alloc(clsName, name, getContextName());

    Stats::emitInc(a, Stats::TgtCache_StaticMethodFHit);
    TCA stubsSkipRet;
    ScratchReg rFunc(m_regMap);
    a.    load_reg64_disp_reg64(rVmTl, ch, *rFunc);
    a.    test_reg64_reg64(*rFunc, *rFunc);
    {
      UnlikelyIfBlock<CC_Z> miss(a, astubs);
      if (false) { // typecheck
        const UNUSED Func* f = StaticMethodFCache::lookup(ch, cls, name);
      }
      EMIT_CALL(astubs,
                 StaticMethodFCache::lookup,
                 IMM(ch),
                 V(clsLoc->location),
                 V(nameLoc->location));
      recordReentrantStubCall(i);
      emitMovRegReg(astubs, rax, *rFunc);
      // if rax == NULL, the helper interpreted the entire
      // instruction for us. Skip over the rest of the emitted code in
      // a, but we don't want to skip the branch spill/fill code.
      miss.reconcileEarly();
      astubs.test_reg64_reg64(*rFunc, *rFunc);
      stubsSkipRet = astubs.code.frontier;
      astubs.jz(a.code.frontier); // to be patched later
    }

    const Offset funcOff = startOfActRec + AROFF(m_func);
    m_regMap.scrubStackRange(i.stackOff - 2,
                             i.stackOff - 2 + kNumActRecCells);
    {
      FreezeRegs ice(m_regMap);
      emitPushAR(i, NULL, bytesPopped);
      emitVStackStore(a, i, *rFunc, funcOff);

      // We know we're in a method so we don't have to worry about
      // rVmFp->m_cls being NULL. We just have to figure out if it's a
      // Class* or $this, and whether or not we should pass along $this or
      // its class.
      PhysReg rCls = *rFunc; // no need to allocate another scratch
      a.    load_reg64_disp_reg64(rVmFp, AROFF(m_cls), rCls);
      a.    test_imm32_reg64(1, rCls);
      {
        JccBlock<CC_NZ> ifThis(a);
        // rCls is holding $this. Should we pass it to the callee?
        a.  cmp_imm32_disp_reg32(1, ch + offsetof(StaticMethodFCache, m_static),
                                 rVmTl);
        {
          IfElseBlock<CC_NE> ifStatic(a);
          // We're calling a static method. Load (this->m_cls | 0x1) into rCls.
          a.load_reg64_disp_reg64(rCls, ObjectData::getVMClassOffset(), rCls);
          a.or_imm32_reg64(1, rCls);

          ifStatic.Else();
          // We're calling an instance method. incRef $this.
          emitIncRef(rCls, KindOfObject);
        }
      }
      emitVStackStore(a, i, rCls, clsOff);
    }

    astubs.patchJcc(stubsSkipRet, a.code.frontier);
    // No need to decref our inputs: one was KindOfClass and the other's
    // a static string.
  }
}

void
TranslatorX64::analyzeFPushObjMethodD(Tracelet& t,
                                      NormalizedInstruction &i) {
  DynLocation* objLoc = i.inputs[0];
  i.m_txFlags = supportedPlan(objLoc->valueType() == KindOfObject &&
                              isContextFixed());
}

void
TranslatorX64::translateFPushObjMethodD(const Tracelet &t,
                                        const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  Location& objLoc = i.inputs[0]->location;
  ASSERT(i.inputs[0]->valueType() == KindOfObject);
  int id = i.imm[1].u_IVA;
  const StringData* name = curUnit()->lookupLitstrId(id);

  const Class* baseClass = i.inputs[0]->rtt.valueClass();
  bool magicCall = false;
  const Func* func = lookupImmutableMethod(baseClass, name, magicCall,
                                           false /* staticLookup */);
  m_regMap.scrubStackRange(i.stackOff - 1,
                           i.stackOff - 1 + kNumActRecCells);
  // Popped one cell, pushed an actrec
  int startOfActRec = int(sizeof(Cell)) - int(sizeof(ActRec));
  size_t thisOff = AROFF(m_this) + startOfActRec;
  size_t funcOff = AROFF(m_func) + startOfActRec;
  emitPushAR(i, func, sizeof(Cell) /*bytesPopped*/,
             false /* isCtor */, false /* clearThis */,
             func && magicCall ? uintptr_t(name) | 1 : 0 /* varEnvInvName */);

  if (!func) {
    if (baseClass && !(baseClass->attrs() & AttrInterface)) {
      MethodLookup::LookupResult res =
        g_vmContext->lookupObjMethod(func, baseClass, name, false);
      if ((res == MethodLookup::MethodFoundWithThis ||
           res == MethodLookup::MethodFoundNoThis) &&
          !func->isAbstract()) {
        /*
         * if we found the func in baseClass, then either:
         *  - its private, and this is always going to be the
         *    called function, or
         *  - any derived class must have a func that matches in
         *    staticness, and is at least as accessible (and in
         *    particular, you cant override a public/protected
         *    method with a private method)
         */
        if (func->attrs() & AttrPrivate) {
          emitVStackStoreImm(a, i, uintptr_t(func), funcOff, sz::qword);
        } else {
          Offset methodsOff = Class::getMethodsOffset();
          Offset vecOff = methodsOff + Class::MethodMap::vecOff();
          ScratchReg scratch(m_regMap);
          // get the object's class into *scratch
          a.   load_reg64_disp_reg64(getReg(objLoc),
                                     ObjectData::getVMClassOffset(),
                                     *scratch);
          if (res == MethodLookup::MethodFoundNoThis) {
            emitDecRef(a, i, getReg(objLoc), KindOfObject);
            a.   lea_reg64_disp_reg64(*scratch, 1, getReg(objLoc));
          }
          emitVStackStore(a, i, getReg(objLoc), thisOff, sz::qword);

          // get the method vector into *scratch
          a.   load_reg64_disp_reg64(*scratch, vecOff, *scratch);
          // get the func
          a.   load_reg64_disp_reg64(*scratch,
                                     func->methodSlot() * sizeof(Func*),
                                     *scratch);
          emitVStackStore(a, i, *scratch, funcOff, sz::qword);
          Stats::emitInc(a, Stats::TgtCache_MethodFast);
          return;
        }
      } else {
        func = NULL;
      }
    }
  }

  if (func) {
    if (func->attrs() & AttrStatic) {
      if (func->attrs() & AttrPrivate) {
        emitVStackStoreImm(a, i, uintptr_t(curFunc()->cls()) | 1,
                           thisOff, sz::qword);
      } else {
        ScratchReg scratch(m_regMap);
        a.   load_reg64_disp_reg64(getReg(objLoc),
                                   ObjectData::getVMClassOffset(),
                                   *scratch);
        a.   or_imm32_reg64(1, *scratch);
        emitVStackStore(a, i, *scratch, thisOff, sz::qword);
      }
      emitDecRef(a, i, getReg(objLoc), KindOfObject);
    } else {
      emitVStackStore(a, i, getReg(objLoc), thisOff, sz::qword);
    }
    Stats::emitInc(a, Stats::TgtCache_MethodBypass);
  } else {
    emitVStackStore(a, i, getReg(objLoc), thisOff, sz::qword);
    using namespace TargetCache;
    CacheHandle ch = MethodCache::alloc();
    if (false) { // typecheck
      ActRec* ar = NULL;
      MethodCache::lookup(ch, ar, name);
    }
    int arOff = vstackOffset(i, startOfActRec);
    SKTRACE(1, i.source, "ch %d\n", ch);
    EMIT_CALL(a, MethodCache::lookup, IMM(ch),
               RPLUS(rVmSp, arOff), IMM(uint64_t(name)));
    recordReentrantCall(i);
  }
}

void TranslatorX64::analyzeFPushCtorD(Tracelet& t,
                                      NormalizedInstruction &i) {
  i.m_txFlags = supportedPlan(true);
}

static inline ALWAYS_INLINE Class* getKnownClass(Class** classCache,
                                                 const StringData* clsName) {
  Class* cls = *classCache;
  if (UNLIKELY(cls == NULL)) {
    // lookupKnownClass does its own VMRegAnchor'ing.
    cls = TargetCache::lookupKnownClass<false>(classCache, clsName, true);
    ASSERT(*classCache && *classCache == cls);
  }
  ASSERT(cls);
  return cls;
}

static Instance*
HOT_FUNC_VM
newInstanceHelperNoCtor(Class** classCache, const StringData* clsName) {
  Class* cls = getKnownClass(classCache, clsName);
  Instance* ret = newInstance(cls);
  ret->incRefCount();
  return ret;
}

static Instance*
HOT_FUNC_VM
newInstanceHelper(Class** classCache,
                  const StringData* clsName, int numArgs,
                  ActRec* ar, ActRec* prevAr) {
  Class* cls = getKnownClass(classCache, clsName);
  const Func* f = cls->getCtor();
  Instance* ret = NULL;
  if (UNLIKELY(!(f->attrs() & AttrPublic))) {
    VMRegAnchor _;
    UNUSED MethodLookup::LookupResult res =
      g_vmContext->lookupCtorMethod(f, cls, true /*raise*/);
    ASSERT(res == MethodLookup::MethodFoundWithThis);
  }
  // Don't start pushing the AR until newInstance returns; it may reenter.
  ret = newInstance(cls);
  f->validate();
  ar->m_func = f;
  ar->initNumArgs(numArgs, true /*fromCtor*/);
  // Count stack and this.
  ret->incRefCount();
  ret->incRefCount();
  ar->setThis(ret);
  ar->setVarEnv(NULL);
  arSetSfp(ar, prevAr);
  TRACE(2, "newInstanceHelper: AR %p: f %p, savedRbp %#lx, savedRip %#lx"
        " this %p\n",
        ar, ar->m_func, ar->m_savedRbp, ar->m_savedRip, ar->m_this);
  return ret;
}

void TranslatorX64::translateFPushCtorD(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  using namespace TargetCache;
  int numArgs = i.imm[0].u_IVA;
  const StringData* clsName = curUnit()->lookupLitstrId(i.imm[1].u_SA);
  CacheHandle classCh = allocKnownClass(clsName);
  ScratchReg scr(m_regMap);
  a.   lea_reg64_disp_reg64(rVmTl, classCh, *scr);
  // We first push the new object, then the actrec. Since we're going to
  // need to call out, and possibly reenter in the course of all this,
  // null out the object on the stack, in case we unwind before we're
  // ready.
  int arOff = vstackOffset(i, -int(sizeof(ActRec)) - cellsToBytes(1));
  m_regMap.scrubStackRange(i.stackOff, i.stackOff + kNumActRecCells + 1);
  if (i.noCtor) {
    EMIT_CALL(a, newInstanceHelperNoCtor,
              R(*scr),
              IMM(uintptr_t(clsName)));
  } else {
    EMIT_CALL(a, newInstanceHelper,
              R(*scr),
              IMM(uintptr_t(clsName)),
              IMM(numArgs),
              RPLUS(rVmSp, arOff),     // ActRec
              R(rVmFp));               // prevAR
  }
  recordReentrantCall(i);
  // The callee takes care of initializing the actRec, and returns the new
  // object.
  m_regMap.bind(rax, i.outStack->location, KindOfObject, RegInfo::DIRTY);
}

static void fatalNullThis() { raise_error(Strings::FATAL_NULL_THIS); }

void
TranslatorX64::translateThis(const Tracelet &t,
                             const NormalizedInstruction &i) {
  ASSERT(i.outStack && !i.outLocal);
  ASSERT(curFunc()->isPseudoMain() || curFunc()->cls());
  m_regMap.allocOutputRegs(i);
  PhysReg out = getReg(i.outStack->location);
  a.   load_reg64_disp_reg64(rVmFp, AROFF(m_this), out);

  if (!i.guardedThis) {
    if (curFunc()->cls() == NULL) {  // Non-class
      a.   test_reg64_reg64(out, out);
      a.   jz(astubs.code.frontier); // jz if_null
    }

    a.   test_imm32_reg64(1, out);
    {
      UnlikelyIfBlock<CC_NZ> ifThisNull(a, astubs);
      // if_null:
      EMIT_CALL(astubs, fatalNullThis);
      recordReentrantStubCall(i);
    }
  }
  emitIncRef(out, KindOfObject);
}

void
TranslatorX64::translateInitThisLoc(const Tracelet& t,
                                    const NormalizedInstruction& i) {
  ASSERT(i.outLocal && !i.outStack);
  ASSERT(curFunc()->isPseudoMain() || curFunc()->cls());

  PhysReg base;
  int offset;
  locToRegDisp(i.outLocal->location, &base, &offset);
  ASSERT(base == rVmFp);

  ScratchReg thiz(m_regMap);
  a.load_reg64_disp_reg64(rVmFp, AROFF(m_this), *thiz);
  if (curFunc()->cls() == NULL) {
    // If we're in a pseudomain, m_this could be NULL
    a.test_reg64_reg64(*thiz, *thiz);
    a.jz(astubs.code.frontier); // jz if_null
  }
  // Ok, it's not NULL but it might be a Class which should be treated
  // equivalently
  a.test_imm32_reg64(1, *thiz);
  a.jnz(astubs.code.frontier); // jnz if_null

  // We have a valid $this!
  a.store_imm32_disp_reg(KindOfObject, offset + TVOFF(m_type), base);
  a.store_reg64_disp_reg64(*thiz, offset + TVOFF(m_data), base);
  emitIncRef(*thiz, KindOfObject);

  // if_null:
  emitStoreUninitNull(astubs, offset, base);
  astubs.jmp(a.code.frontier);

  m_regMap.invalidate(i.outLocal->location);
}

void
TranslatorX64::analyzeFPushFuncD(Tracelet& t, NormalizedInstruction& i) {
  Id funcId = i.imm[1].u_SA;
  const NamedEntityPair nep = curUnit()->lookupNamedEntityPairId(funcId);
  const Func* func = Unit::lookupFunc(nep.second, nep.first);
  i.m_txFlags = supportedPlan(func != NULL);
}

void
TranslatorX64::translateFPushFuncD(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outStack && !i.outLocal);
  Id funcId = i.imm[1].u_SA;
  const NamedEntityPair& nep = curUnit()->lookupNamedEntityPairId(funcId);
  const StringData* name = nep.first;
  const Func* func       = Unit::lookupFunc(nep.second, name);

  // Translation is only supported if function lookup succeeds
  func->validate();
  if (Trace::enabled && !func) {
    TRACE(1, "Attempt to invoke undefined function %s\n", name->data());
  }

  // Inform the register allocator that we just annihilated a range of
  // possibly-dirty stack entries.
  m_regMap.scrubStackRange(i.stackOff,
                           i.stackOff + kNumActRecCells);

  size_t thisOff = AROFF(m_this) - sizeof(ActRec);
  bool funcCanChange = !func->isNameBindingImmutable(curUnit());
  emitVStackStoreImm(a, i, 0, thisOff, sz::qword, &m_regMap);
  emitPushAR(i, funcCanChange ? NULL : func, 0, false, false);
  if (funcCanChange) {
    // Look it up in a FuncCache.
    using namespace TargetCache;
    CacheHandle ch = FixedFuncCache::alloc(name);
    size_t funcOff = AROFF(m_func) - sizeof(ActRec);
    size_t funcCacheOff = ch + offsetof(FixedFuncCache, m_func);

    SKTRACE(1, i.source, "ch %d\n", ch);

    Stats::emitInc(a, Stats::TgtCache_FuncDHit);
    ScratchReg scratch(m_regMap);
    a.load_reg64_disp_reg64(rVmTl, funcCacheOff, *scratch);
    a.test_reg64_reg64(*scratch, *scratch);
    {
      UnlikelyIfBlock<CC_Z> ifNull(a, astubs);

      if (false) { // typecheck
        StringData sd("foo");
        FixedFuncCache::lookupFailed(&sd);
      }

      EMIT_CALL(astubs, TCA(FixedFuncCache::lookupFailed),
                        IMM(uintptr_t(name)));
      recordReentrantStubCall(i);
      emitMovRegReg(astubs, rax, *scratch);
    }
    emitVStackStore(a, i, *scratch, funcOff, sz::qword);
  }
}

void
TranslatorX64::translateFPushContFunc(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  // All functions that use this instruction are cloned into
  // subclasses so we can burn in the Continuation's body Func*
  ASSERT(curFrame()->hasThis());
  c_GenericContinuation* cont =
    dynamic_cast<c_GenericContinuation*>(curFrame()->getThis());
  ASSERT(cont);
  const Func* genFunc = cont->m_vmFunc;
  genFunc->validate();
  ASSERT(IMPLIES(genFunc->isTraitMethod(), cont->m_isMethod));

  // TODO: Make the FuncIds for Continuation subclasses depend on the
  // containing class's parents as well, so we can burn in the Func*
  // for method generators. See t1061448.
  if (cont->m_isMethod) {
    genFunc = NULL;
  }

  size_t thisOff = AROFF(m_this) - sizeof(ActRec);
  size_t funcOff = AROFF(m_func) - sizeof(ActRec);
  m_regMap.scrubStackRange(i.stackOff,
                           i.stackOff + kNumActRecCells);
  emitPushAR(i, genFunc, 0, false, !cont->m_isMethod);
  if (cont->m_isMethod) {
    ScratchReg rCont(m_regMap);
    ScratchReg rScratch(m_regMap);
    a.  load_reg64_disp_reg64(rVmFp, AROFF(m_this), *rCont);
    if (genFunc == NULL) {
      a.load_reg64_disp_reg64(*rCont,
                              offsetof(c_GenericContinuation, m_vmFunc),
                              *rScratch);
      emitVStackStore(a, i, *rScratch, funcOff, sz::qword);
    }
    a.  load_reg64_disp_reg64(*rCont,
                              offsetof(c_GenericContinuation, m_obj),
                              *rScratch);
    a.  test_reg64_reg64(*rScratch, *rScratch);
    {
      IfElseBlock<CC_Z> ifThis(a);
      emitVStackStore(a, i, *rScratch, thisOff, sz::qword);
      emitIncRef(*rScratch, KindOfObject);

      ifThis.Else();
      a.load_reg64_disp_reg64(*rCont,
                              offsetof(c_GenericContinuation, m_vmCalledClass),
                              *rScratch);
      // m_vmCalledClass already has its low bit set
      emitVStackStore(a, i, *rScratch, thisOff, sz::qword);
    }
  }
}

void
TranslatorX64::analyzeFPassCOp(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = nativePlan(!i.preppedByRef);
}

void
TranslatorX64::translateFPassCOp(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 0);
  ASSERT(!i.outStack && !i.outLocal);
  ASSERT(!i.preppedByRef);
}

void
TranslatorX64::analyzeFPassM(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(
    // TODO: once we have VGetM, implement the byRef case in terms of it.
    !i.preppedByRef &&
    // Implement this in terms of CGetM.
    isSupportedCGetM(i));
}

void
TranslatorX64::translateFPassM(const Tracelet& t,
                               const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() >= 1);
  ASSERT(i.outStack && !i.outLocal);
  ASSERT_NOT_IMPLEMENTED(!i.preppedByRef);
  translateCGetM(t, i);
}


void
TranslatorX64::translateFPassR(const Tracelet& t,
                               const NormalizedInstruction& i) {
  /*
   * Like FPassC, FPassR is able to cheat on boxing if the current
   * parameter is pass by reference but we have a cell: the box would refer
   * to exactly one datum (the value currently on the stack).
   *
   * However, if the callee wants a cell and we have a variant we must
   * unbox; otherwise we might accidentally make callee changes to its
   * parameter globally visible.
   */
  ASSERT(i.inputs.size() == 1);
  const RuntimeType& inRtt = i.inputs[0]->rtt;
  if (inRtt.isVariant() && !i.preppedByRef) {
    emitUnboxTopOfStack(i);
  }
}

void
TranslatorX64::translateFCall(const Tracelet& t,
                              const NormalizedInstruction& i) {
  // Sync all dirty registers and adjust rVmSp to point to the
  // top of stack at the beginning of the current instruction
  syncOutputs(i);

  int numArgs = i.imm[0].u_IVA;
  const Opcode* atCall = i.pc();
  const Opcode* after = curUnit()->at(nextSrcKey(t, i).offset());

  // Caller-specific fields: return addresses and the frame pointer
  // offset.
  ASSERT(sizeof(Cell) == 1 << 4);
  const Func* srcFunc = curFunc();
  // Record the hardware return address. This will be patched up below; 2
  // is a magic number dependent on assembler implementation.
  uint64 *retIP = (uint64*)(a.code.frontier + 2);
  emitImmReg(a, kUninitializedRIP, rax);
  ASSERT(*retIP == kUninitializedRIP);
  a.    store_reg64_disp_reg64 (rax,
                                cellsToBytes(numArgs) + AROFF(m_savedRip),
                                rVmSp);

  // The kooky offset here a) gets us to the current ActRec,
  // and b) accesses m_soff.
  int32 callOffsetInUnit = srcFunc->unit()->offsetOf(after - srcFunc->base());
  a.    store_imm32_disp_reg(callOffsetInUnit,
                             cellsToBytes(numArgs) + AROFF(m_soff),
                             rVmSp);

  emitBindCall(t, i,
               curUnit()->offsetOf(atCall),
               curUnit()->offsetOf(after)); // ...
  *retIP = uint64(a.code.frontier);

  if (i.breaksBB) {
    SrcKey fallThru(curFunc(), after);
    emitBindJmp(fallThru);
  } else {
    /*
     * Before returning, the callee restored rVmSp to point to the
     * current top of stack but the rest of this tracelet assumes that
     * rVmSp is set to the top of the stack at the beginning of the
     * tracelet, so we have to fix it up here.
     *
     * TODO: in the case of an inlined NativeImpl, we're essentially
     * emitting two adds to rVmSp in a row, which we can combine ...
     */
    int delta = i.stackOff + getStackDelta(i);
    if (delta != 0) {
      // i.stackOff is in negative Cells, not bytes.
      a.    add_imm64_reg64(cellsToBytes(delta), rVmSp);
    }
  }
}

void
TranslatorX64::analyzeStaticLocInit(Tracelet& t, NormalizedInstruction& i) {
  // Class methods, closures, and generators all violate the
  // one-source == one-static assumption for our current translation
  // strategy.
  i.m_txFlags = supportedPlan(!curFunc()->isClosureBody() &&
                              !curFunc()->isGeneratorFromClosure() &&
                              !curFunc()->cls());
}

static TypedValue*
staticLocHelper(StringData* name, TargetCache::CacheHandle ch) {
  VMRegAnchor _;
  Stats::inc(Stats::TgtCache_StaticMiss);
  Stats::inc(Stats::TgtCache_StaticHit, -1);
  HphpArray* map = get_static_locals(curFrame());
  TypedValue* retval = map->nvGet(name); // Local to num
  if (!retval) {
    // Read the initial value off the stack.
    TypedValue tv = *vmsp();
    map->nvSet(name, &tv, false);
    retval = map->nvGet(name);
  }
  ASSERT(retval);
  if (retval->m_type != KindOfRef) {
    tvBox(retval);
  }
  ASSERT(retval->m_type == KindOfRef);
  TypedValue** chTv = (TypedValue**)TargetCache::handleToPtr(ch);
  ASSERT(*chTv == NULL);
  return (*chTv = retval);
}

void
TranslatorX64::translateStaticLocInit(const Tracelet& t,
                                      const NormalizedInstruction& i) {
  // Miss path explicitly decrements.
  Stats::emitInc(a, Stats::TgtCache_StaticHit);

  using namespace TargetCache;
  CacheHandle ch = allocStatic();
  ScratchReg output(m_regMap);
  const Location& outLoc = i.outLocal->location;
  a.    load_reg64_disp_reg64(rVmTl, ch, *output);
  a.    test_reg64_reg64(*output, *output);
  {
    UnlikelyIfBlock<CC_Z> fooey(a, astubs);
    // No point spilling outLoc in this branch; it doesn't have its final
    // value yet.
    //
    // The helper is going to read the value from memory, so record it.  We
    // could also pass type/value as parameters, but this is hopefully a
    // rare path.
    m_regMap.cleanLoc(i.inputs[0]->location);
    if (false) { // typecheck
      StringData* sd = NULL;
      UNUSED TypedValue* tv = staticLocHelper(sd, ch);
    }
    const StringData* name = curFunc()->unit()->lookupLitstrId(i.imm[1].u_SA);
    ASSERT(name->isStatic());
    EMIT_CALL(astubs, TCA(staticLocHelper), IMM(uintptr_t(name)), IMM(ch));
    recordReentrantStubCall(i);
    emitMovRegReg(astubs, rax, *output);
  }
  // Now we've got the outer variant in *output. Get the address of the
  // inner cell, since that's the enregistered representation of a variant.
  emitDeref(a, *output, *output);
  emitIncRef(*output, KindOfRef);
  // Turn output into the local we just initialized.
  m_regMap.bindScratch(output, outLoc, KindOfRef, RegInfo::DIRTY);
}

void
TranslatorX64::analyzeVerifyParamType(Tracelet& t, NormalizedInstruction& i) {
  int param = i.imm[0].u_IVA;
  const TypeConstraint& tc = curFunc()->params()[param].typeConstraint();
  if (!tc.isObject()) {
    // We are actually using the translation-time value of this local as a
    // prediction; if the param check failed at compile-time, we predict it
    // will continue failing.
    bool compileTimeCheck = tc.check(frame_local(curFrame(), param), curFunc());
    i.m_txFlags = nativePlan(compileTimeCheck);
    i.manuallyAllocInputs = true;
  } else {
    bool trace = i.inputs[0]->isObject() ||
                 (i.inputs[0]->isNull() && tc.nullable());
    i.m_txFlags = supportedPlan(trace);
  }
}

static void
VerifyParamTypeFail(int paramNum) {
  VMRegAnchor _;
  const ActRec* ar = curFrame();
  const Func* func = ar->m_func;
  const TypeConstraint& tc = func->params()[paramNum].typeConstraint();
  ASSERT(tc.isObject());
  TypedValue* tv = frame_local(ar, paramNum);
  TRACE(3, "%s Obj %s, needs type %s\n",
        __func__,
        tv->m_data.pobj->getVMClass()->name()->data(),
        tc.typeName()->data());
  tc.verifyFail(func, paramNum, tv);
}

// check class hierarchy and fail if no match
static uint64_t
VerifyParamTypeSlow(const Class* cls, const Class* constraint) {
  Stats::inc(Stats::Tx64_VerifyParamTypeSlow);
  Stats::inc(Stats::Tx64_VerifyParamTypeFast, -1);

  // ensure C++ returns a 0 or 1 with upper bits zeroed
  return static_cast<uint64_t>(constraint && cls->classof(constraint));
}

void
TranslatorX64::translateVerifyParamType(const Tracelet& t,
                                        const NormalizedInstruction& i) {
  Stats::emitInc(a, Stats::Tx64_VerifyParamTypeFast);

  int param = i.imm[0].u_IVA;
  const TypeConstraint& tc = curFunc()->params()[param].typeConstraint();
  // not quite a nop. The guards should have verified that the m_type field
  // is compatible, but for objects we need to go one step further and
  // ensure that we're dealing with the right class.
  // NULL inputs only get traced when constraint is nullable.
  ASSERT(i.inputs.size() == 1);
  if (!i.inputs[0]->isObject()) return; // nop.

  // Get the input's class from ObjectData->m_cls
  const Location& in = i.inputs[0]->location;
  PhysReg src = getReg(in);
  ScratchReg inCls(m_regMap);
  if (i.inputs[0]->rtt.isVariant()) {
    emitDeref(a, src, *inCls);
    a.  load_reg64_disp_reg64(*inCls, ObjectData::getVMClassOffset(), *inCls);
  } else {
    a.  load_reg64_disp_reg64(src, ObjectData::getVMClassOffset(), *inCls);
  }

  ScratchReg cls(m_regMap);
  // Constraint may not be in the class-hierarchy of the method being traced,
  // look up the class handle and emit code to put the Class* into a reg.
  if (!tc.isSelf() && !tc.isParent()) {
    const StringData* clsName = tc.typeName();
    using namespace TargetCache;
    CacheHandle ch = allocKnownClass(clsName);
    a.  load_reg64_disp_reg64(rVmTl, ch, *cls);
  } else {
    const Class *constraint = NULL;
    if (tc.isSelf()) {
      tc.selfToClass(curFunc(), &constraint);
    } else if (tc.isParent()) {
      tc.parentToClass(curFunc(), &constraint);
    }
    emitImmReg(a, uintptr_t(constraint), *cls);
  }
  // Compare this class to the incoming object's class. If the typehint's class
  // is not present, can not be an instance: fail
  a.  cmp_reg64_reg64(*inCls, *cls);

  {
    JccBlock<CC_Z> subclassCheck(a);
    // Call helper since ObjectData::instanceof is a member function
    if (false) {
      Class* cls = NULL;
      Class* constraint = NULL;
      VerifyParamTypeSlow(cls, constraint);
    }
    EMIT_CALL(a, VerifyParamTypeSlow, R(*inCls), R(*cls));
    // Pin the return value, check if a match or take slow path
    m_regMap.bind(rax, Location(), KindOfInvalid, RegInfo::SCRATCH);
    a.  test_reg64_reg64(rax, rax);
    m_regMap.freeScratchReg(rax);

    // Put the failure path into astubs
    {
      UnlikelyIfBlock<CC_Z> fail(a, astubs);
      if (false) { // typecheck
        VerifyParamTypeFail(param);
      }
      EMIT_CALL(astubs, VerifyParamTypeFail, IMM(param));
      recordReentrantStubCall(i);
    }
  }
}

void
TranslatorX64::analyzeInstanceOfD(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);
  i.m_txFlags = planHingesOnRefcounting(i.inputs[0]->outerType());
}

// check class hierarchy and fail if no match
static uint64_t
InstanceOfDSlow(const Class* cls, const Class* constraint) {
  Stats::inc(Stats::Tx64_InstanceOfDSlow);
  Stats::inc(Stats::Tx64_InstanceOfDFast, -1);

  // ensure C++ returns a 0 or 1 with upper bits zeroed
  return static_cast<uint64_t>(constraint && cls->classof(constraint));
}

void
TranslatorX64::translateInstanceOfD(const Tracelet& t,
                                    const NormalizedInstruction& i) {
  Stats::emitInc(a, Stats::Tx64_InstanceOfDFast);
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.outStack && !i.outLocal);

  DynLocation* input0 = i.inputs[0];
  bool input0IsLoc = input0->isLocal();
  DataType type = input0->valueType();
  PhysReg srcReg = getReg(input0->location);
  ScratchReg result(m_regMap);

  if (type != KindOfObject) {
    // All non-object inputs are not instances
    if (!input0IsLoc && IS_REFCOUNTED_TYPE(type)) {
      ASSERT(!input0->isVariant());
      emitDecRef(i, srcReg, type);
    }
    emitImmReg(a, false, *result);

  } else {
    // Get the input's class from ObjectData->m_cls
    ScratchReg inCls(m_regMap);
    if (input0->rtt.isVariant()) {
      ASSERT(input0IsLoc);
      emitDeref(a, srcReg, *inCls);
      a.  load_reg64_disp_reg64(*inCls, ObjectData::getVMClassOffset(), *inCls);
    } else {
      a.  load_reg64_disp_reg64(srcReg, ObjectData::getVMClassOffset(), *inCls);
    }
    if (!input0IsLoc && IS_REFCOUNTED_TYPE(type)) {
      emitDecRef(i, srcReg, type);
    }

    // Set result to true for now. If take slow path, use its return val
    emitImmReg(a, true, *result);
    ScratchReg cls(m_regMap);
    // Constraint may not be in the class-hierarchy of the method being traced,
    // look up the class handle and emit code to put the Class* into a reg.
    using namespace TargetCache;
    int param = i.imm[0].u_SA;
    const StringData* clsName = curUnit()->lookupLitstrId(param);
    CacheHandle ch = allocKnownClass(clsName);
    a.    load_reg64_disp_reg64(rVmTl, ch, *cls);
    // Compare this class to the incoming object's class. If the typehint's
    // class is not present, can not be an instance: fail
    a.    cmp_reg64_reg64(*inCls, *cls);

    {
      UnlikelyIfBlock<CC_NZ> subclassCheck(a, astubs);
      // Call helper since ObjectData::instanceof is a member function
      if (false) {
        Class* cls = NULL;
        Class* constraint = NULL;
        InstanceOfDSlow(cls, constraint);
      }
      EMIT_CALL(astubs, InstanceOfDSlow, R(*inCls), R(*cls));
      astubs.  mov_reg32_reg32(rax, *result);
    }
  }
  // Bind result and destination
  m_regMap.bindScratch(result, i.outStack->location, i.outStack->outerType(),
                       RegInfo::DIRTY);
}

void
TranslatorX64::analyzeIterInit(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = simplePlan(i.inputs[0]->valueType() == KindOfArray);
}

void
TranslatorX64::translateIterInit(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(!i.outStack && !i.outLocal);
  DynLocation* arrIn = i.inputs[0];
  ASSERT(arrIn->outerType() != KindOfRef);

  PhysReg src = getReg(arrIn->location);
  SKTRACE(1, i.source, "IterInit: committed to translation\n");
  syncOutputs(t); // Ends BB
  SrcKey taken, notTaken;
  branchDests(t, i, &taken, &notTaken, 1 /* immIdx */);
  Location iterLoc(Location::Iter, i.imm[0].u_IVA);
  PhysReg base;
  int offset;
  locToRegDisp(iterLoc, &base, &offset);
  TRACE(2, "IterInit: iter %lld -> base r%d, offset %d\n", iterLoc.offset,
      base, offset);
  if (false) { // typecheck
    Iter *dest = NULL;
    HphpArray *arr = NULL;
    new_iter(dest, arr);
  }
  // We've already synced the outputs, so we need to manually
  // set up the parameters and do the call
  emitMovRegReg(src, argNumToRegName[1]);
  a.    lea_reg64_disp_reg64(base, offset, argNumToRegName[0]);
  // If a new iterator is created, new_iter will not adjust the refcount of
  // the array. If a new iterator is not created, new_iter will decRef the
  // array for us.
  emitCall(a, (TCA)new_iter);
  // new_iter returns 0 if an iterator was not created, otherwise it
  // returns 1
  a.    test_reg64_reg64(rax, rax);
  TCA toPatch = a.code.frontier;
  a.    jz(0); // 1f
  emitBindJmp(notTaken);
  // 1:
  a.patchJcc(toPatch, a.code.frontier);
  emitBindJmp(taken);
  translator_not_reached(a);
}

void
TranslatorX64::analyzeIterValueC(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(i.inputs[0]->rtt.iterType() == Iter::TypeArray);
}

void
TranslatorX64::translateIterValueC(const Tracelet& t,
                                   const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.inputs[0]->rtt.isIter());

  Location outLoc;

  if (false) { // type check
    Iter* it = NULL;
    TypedValue* out = NULL;
    iter_value_cell(it, out);
  }
  if (i.outStack) {
    outLoc = i.outStack->location;
    EMIT_CALL(a, iter_value_cell, A(i.inputs[0]->location), A(outLoc));
  } else {
    outLoc = i.outLocal->location;
    EMIT_CALL(a, iter_value_cell_local, A(i.inputs[0]->location), A(outLoc));
  }
  recordReentrantCall(a, i);
  m_regMap.invalidate(outLoc);
}

void
TranslatorX64::analyzeIterKey(Tracelet& t, NormalizedInstruction& i) {
  i.m_txFlags = supportedPlan(i.inputs[0]->rtt.iterType() == Iter::TypeArray);
}

void
TranslatorX64::translateIterKey(const Tracelet& t,
                                     const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(i.inputs[0]->rtt.isIter());

  Location outLoc;
  if (false) { // type check
    Iter* it = NULL;
    TypedValue* out = NULL;
    iter_key_cell(it, out);
  }
  if (i.outStack) {
    outLoc = i.outStack->location;
    EMIT_CALL(a, iter_key_cell, A(i.inputs[0]->location), A(outLoc));
  } else {
    outLoc = i.outLocal->location;
    EMIT_CALL(a, iter_key_cell_local, A(i.inputs[0]->location), A(outLoc));
  }
  recordReentrantCall(a, i);
  m_regMap.invalidate(outLoc);
}

void
TranslatorX64::analyzeIterNext(Tracelet& t, NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  i.m_txFlags = supportedPlan(i.inputs[0]->rtt.iterType() == Iter::TypeArray);
}

void
TranslatorX64::translateIterNext(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  ASSERT(i.inputs.size() == 1);
  ASSERT(!i.outStack && !i.outLocal);
  ASSERT(i.inputs[0]->rtt.isIter());

  if (false) { // type check
    Iter* it = NULL;
    int64 ret = iter_next_array(it);
    if (ret) printf("\n");
  }
  m_regMap.cleanAll(); // input might be in-flight
  // If the iterator reaches the end, iter_next_array will handle
  // freeing the iterator and it will decRef the array
  EMIT_CALL(a, iter_next_array, A(i.inputs[0]->location));
  recordReentrantCall(a, i);
  // RAX is now a scratch register with no progam meaning...
  m_regMap.bind(rax, Location(), KindOfInvalid, RegInfo::SCRATCH);

  // syncOutputs before we handle the branch.
  syncOutputs(t);
  SrcKey taken, notTaken;
  branchDests(t, i, &taken, &notTaken, 1 /* destImmIdx */);

  a.   test_reg64_reg64(rax, rax);
  emitCondJmp(taken, notTaken, CC_NZ);
}

// PSEUDOINSTR_DISPATCH is a switch() fragment that routes opcodes to their
// shared handlers, as per the PSEUDOINSTRS macro.
#define PSEUDOINSTR_DISPATCH(func)              \
  case OpBitAnd:                                \
  case OpBitOr:                                 \
  case OpBitXor:                                \
  case OpSub:                                   \
  case OpMul:                                   \
    func(BinaryArithOp, t, i)                   \
  case OpSame:                                  \
  case OpNSame:                                 \
    func(SameOp, t, i)                          \
  case OpEq:                                    \
  case OpNeq:                                   \
    func(EqOp, t, i)                            \
  case OpLt:                                    \
  case OpLte:                                   \
  case OpGt:                                    \
  case OpGte:                                   \
    func(LtGtOp, t, i)                          \
  case OpEmptyL:                                \
  case OpCastBool:                              \
    func(UnaryBooleanOp, t, i)                  \
  case OpJmpZ:                                  \
  case OpJmpNZ:                                 \
    func(BranchOp, t, i)                        \
  case OpSetL:                                  \
  case OpBindL:                                 \
    func(AssignToLocalOp, t, i)                 \
  case OpFPassC:                                \
  case OpFPassCW:                               \
  case OpFPassCE:                               \
    func(FPassCOp, t, i)                        \
  case OpIssetL:                                \
  case OpIsNullL:                               \
  case OpIsStringL:                             \
  case OpIsArrayL:                              \
  case OpIsIntL:                                \
  case OpIsObjectL:                             \
  case OpIsBoolL:                               \
  case OpIsDoubleL:                             \
  case OpIssetC:                                \
  case OpIsNullC:                               \
  case OpIsStringC:                             \
  case OpIsArrayC:                              \
  case OpIsIntC:                                \
  case OpIsObjectC:                             \
  case OpIsBoolC:                               \
  case OpIsDoubleC:                             \
    func(CheckTypeOp, t, i)

void
TranslatorX64::analyzeInstr(Tracelet& t,
                            NormalizedInstruction& i) {
  const Opcode op = i.op();
  switch (op) {
#define CASE(iNm) \
  case Op ## iNm: { \
    analyze ## iNm(t, i); \
  } break;
#define ANALYZE(a, b, c) analyze ## a(b, c); break;
  INSTRS
  PSEUDOINSTR_DISPATCH(ANALYZE)

#undef ANALYZE
#undef CASE
    default: {
      ASSERT(i.m_txFlags == Interp);
    }
  }
  SKTRACE(1, i.source, "translation plan: %x\n", i.m_txFlags);
}

bool
TranslatorX64::dontGuardAnyInputs(Opcode op) {
  switch (op) {
#define CASE(iNm) case Op ## iNm:
#define NOOP(a, b, c)
  INSTRS
    PSEUDOINSTR_DISPATCH(NOOP)
    return false;
  }
  return true;
#undef NOOP
#undef CASE

}

void TranslatorX64::emitOneGuard(const Tracelet& t,
                                 const NormalizedInstruction& i,
                                 PhysReg reg, int disp, DataType type,
                                 TCA &sideExit) {
  bool isFirstInstr = (&i == t.m_instrStream.first);
  emitTypeCheck(a, type, reg, disp);
  if (isFirstInstr) {
    SrcRec& srcRec = *getSrcRec(t.m_sk);
    // If it's the first instruction, we haven't made any forward
    // progress yet, so this is really a tracelet-level guard rather
    // than a side exit. If we tried to "side exit", we'd come right
    // back to this check!
    //
    // We need to record this as a fallback branch.
    emitFallbackJmp(srcRec);
  } else if (!sideExit) {
    UnlikelyIfBlock<CC_NZ> branchToSideExit(a, astubs);
    sideExit = astubs.code.frontier;
    emitSideExit(astubs, i, false /*next*/);
  } else {
    a.    jnz(sideExit);
  }
}

void
TranslatorX64::emitVariantGuards(const Tracelet& t,
                                 const NormalizedInstruction& i) {
  bool pseudoMain = Translator::liveFrameIsPseudoMain();
  TCA sideExit = NULL;
  const NormalizedInstruction *base = &i;
  while (base->grouped) {
    base = base->prev;
    ASSERT(base);
  }
  for (size_t in = 0; in < i.inputs.size(); ++in) {
    DynLocation* input = i.inputs[in];
    if (!input->isValue()) continue;
    bool isRef = input->isVariant() &&
      !i.ignoreInnerType &&
      input->rtt.innerType() != KindOfInvalid;
    bool modifiableLocal = pseudoMain && input->isLocal() &&
      !input->rtt.isVagueValue();

    if (!modifiableLocal && !isRef) continue;

    SKTRACE(1, i.source, "guarding %s: (%s:%d) :: %d!\n",
            modifiableLocal ? "pseudoMain local" : "variant inner",
            input->location.spaceName(),
            input->location.offset,
            input->rtt.valueType());
    // TODO task 1122807: don't check the inner type if we've already
    // checked it and have executed no possibly-aliasing instructions in
    // the meanwhile.
    if (modifiableLocal) {
      PhysReg reg;
      int disp;
      locToRegDisp(input->location, &reg, &disp);
      emitOneGuard(t, *base, reg, disp,
                   input->rtt.outerType(), sideExit);
    }
    if (isRef) {
      m_regMap.allocInputReg(i, in);
      emitOneGuard(t, *base, getReg(input->location), 0,
                   input->rtt.innerType(), sideExit);
    }
  }
}

NormalizedInstruction::OutputUse
NormalizedInstruction::outputIsUsed(DynLocation* output) const {
  for (NormalizedInstruction* succ = next;
       succ; succ = succ->next) {
    for (size_t i = 0; i < succ->inputs.size(); ++i) {
      if (succ->inputs[i] == output) {
        if (succ->inputWasInferred(i)) {
          return OutputInferred;
        }
        if (Translator::Get()->dontGuardAnyInputs(succ->op())) {
          /* the consumer doesnt care about its inputs
             but we may still have inferred something about
             its outputs that a later instruction may depend on
          */
          if (!outputDependsOnInput(succ->op()) ||
              !(succ->outStack && !succ->outStack->rtt.isVagueValue() &&
                succ->outputIsUsed(succ->outStack) != OutputUsed) ||
              !(succ->outLocal && !succ->outLocal->rtt.isVagueValue() &&
                succ->outputIsUsed(succ->outLocal)) != OutputUsed) {
            return OutputDoesntCare;
          }
        }
        return OutputUsed;
      }
    }
  }
  return OutputUnused;
}

void
TranslatorX64::emitPredictionGuards(const NormalizedInstruction& i) {
  if (!i.outputPredicted || i.breaksBB) return;
  NormalizedInstruction::OutputUse u = i.outputIsUsed(i.outStack);
  switch (u) {
    case NormalizedInstruction::OutputUsed:
      break;
    case NormalizedInstruction::OutputUnused:
      return;
    case NormalizedInstruction::OutputInferred:
      Stats::emitInc(a, Stats::TC_TypePredOverridden);
      return;
    case NormalizedInstruction::OutputDoesntCare:
      Stats::emitInc(a, Stats::TC_TypePredUnneeded);
      return;
  }

  ASSERT(i.outStack);
  PhysReg base;
  int disp;
  locToRegDisp(i.outStack->location, &base, &disp);
  ASSERT(base == rVmSp);
  TRACE(1, "PREDGUARD: %p dt %d offset %d voffset %lld\n",
        a.code.frontier, i.outStack->outerType(), disp,
        i.outStack->location.offset);
  emitTypeCheck(a, i.outStack->outerType(), rVmSp, disp);
  {
    UnlikelyIfBlock<CC_NZ> branchToSideExit(a, astubs);
    Stats::emitInc(astubs, Stats::TC_TypePredMiss);
    emitSideExit(astubs, i, true);
  }
  Stats::emitInc(a, Stats::TC_TypePredHit);
}

static void failedTypePred() {
  raise_error("A type prediction was incorrect");
}

void
TranslatorX64::translateInstr(const Tracelet& t,
                              const NormalizedInstruction& i) {
  /**
   * translateInstr() translates an individual instruction in a tracelet,
   * either by directly emitting machine code for that instruction or by
   * emitting a call to the interpreter.
   *
   * If the instruction ends the current tracelet, we must emit machine code
   * to transfer control to some target that will continue to make forward
   * progress. This target may be the beginning of another tracelet, or it may
   * be a translator service request. Before transferring control, a tracelet
   * must ensure the following invariants hold:
   *   1) The machine registers rVmFp and rVmSp are in sync with vmfp()
   *      and vmsp().
   *   2) All "dirty" values are synced in memory. This includes the
   *      evaluation stack, locals, globals, statics, and any other program
   *      accessible locations. This also means that all refcounts must be
   *      up to date.
   */
  ASSERT(!i.outStack || i.outStack->isStack());
  ASSERT(!i.outLocal || i.outLocal->isLocal());
  const char *opNames[] = {
#define O(name, imm, push, pop, flags) \
#name,
  OPCODES
#undef O
  };
  SpaceRecorder sr(opNames[i.op()], a);
  SKTRACE(1, i.source, "translate %#lx\n", long(a.code.frontier));
  const Opcode op = i.op();
  TCA start = a.code.frontier;
  TCA astart = astubs.code.frontier;
  m_regMap.bumpEpoch();

  if (debug) {
    for (unsigned j = 0; j < i.inputs.size(); j++) {
      if (i.inputWasInferred(j)) {
        DynLocation* dl = i.inputs[j];
        ASSERT(dl->rtt.isValue() &&
               !dl->rtt.isVagueValue() &&
               dl->outerType() != KindOfInvalid);
        PhysReg base;
        int disp;
        locToRegDisp(dl->location, &base, &disp);
        emitTypeCheck(a, dl->rtt.typeCheckValue(), base, disp);
        {
          UnlikelyIfBlock<CC_NZ> typePredFailed(a, astubs);
          EMIT_CALL(astubs, failedTypePred);
          recordReentrantStubCall(i);
        }
      }
    }
  }

  if (!i.grouped) {
    emitVariantGuards(t, i);
    const NormalizedInstruction* n = &i;
    while (n->next && n->next->grouped) {
      n = n->next;
      emitVariantGuards(t, *n);
    }
  }

  // Allocate the input regs upfront unless instructed otherwise
  // or the instruction is interpreted
  if (!i.manuallyAllocInputs && i.m_txFlags) {
    m_regMap.allocInputRegs(i);
  }

  if (i.m_txFlags == Interp || RuntimeOption::EvalThreadingJit) {
    // If the problem is local to this instruction, just call out to
    // the interpreter. emitInterpOne will perform end-of-tracelet duties
    // if this instruction ends the tracelet.
    SKTRACE(1, i.source, "Interp\n");
    emitInterpOne(t, i);
  } else {
    // Actually translate the instruction's body.
    Stats::emitIncTranslOp(a, op);
    switch (op) {
#define CASE(iNm)                               \
    case Op ## iNm:                             \
                    translate ## iNm(t, i);     \
      break;
#define TRANSLATE(a, b, c) translate ## a(b, c); break;
      INSTRS
        PSEUDOINSTR_DISPATCH(TRANSLATE)
#undef TRANSLATE
#undef CASE
    default: ASSERT(false);
    }
  }

  // Invalidate locations that are no longer live
  for (unsigned k = 0; k < i.deadLocs.size(); ++k) {
    const Location& l = i.deadLocs[k];
    m_regMap.invalidate(l);
  }

  emitPredictionGuards(i);
  recordBCInstr(op, a, start);
  recordBCInstr(op + Op_count, astubs, astart);

  if (i.breaksBB && !i.changesPC) {
    // If this instruction's opcode always ends the tracelet then the
    // instruction case is responsible for performing end-of-tracelet
    // duties. Otherwise, we handle ending the tracelet here.
    syncOutputs(t);
    emitBindJmp(t.m_nextSk);
  }
}

bool
TranslatorX64::checkTranslationLimit(const SrcKey& sk,
                                     const SrcRec& srcRec) const {
  if (srcRec.translations().size() == SrcRec::kMaxTranslations) {
    INC_TPC(max_trans);
    if (debug && Trace::moduleEnabled(Trace::tx64, 2)) {
      const vector<TCA>& tns = srcRec.translations();
      TRACE(1, "Too many (%ld) translations: %s, BC offset %d\n",
            tns.size(), curUnit()->filepath()->data(),
            sk.offset());
      SKTRACE(2, sk, "{\n", tns.size());
      TCA topTrans = srcRec.getTopTranslation();
      for (size_t i = 0; i < tns.size(); ++i) {
        const TransRec* rec = getTransRec(tns[i]);
        ASSERT(rec);
        SKTRACE(2, sk, "%d %p\n", i, tns[i]);
        if (tns[i] == topTrans) {
          SKTRACE(2, sk, "%d: *Top*\n", i);
        }
        if (rec->kind == TransAnchor) {
          SKTRACE(2, sk, "%d: Anchor\n", i);
        } else {
          SKTRACE(2, sk, "%d: guards {\n", i);
          for (unsigned j = 0; j < rec->dependencies.size(); ++j) {
            TRACE(2, rec->dependencies[j]);
          }
          SKTRACE(2, sk, "%d } guards\n", i);
        }
      }
      SKTRACE(2, sk, "} /* Too many translations */\n");
    }
    return true;
  }

  return false;
}

void
TranslatorX64::emitGuardChecks(X64Assembler& a,
                               const SrcKey& sk,
                               const ChangeMap& dependencies,
                               const RefDeps& refDeps,
                               SrcRec& fail) {
  if (Trace::moduleEnabled(Trace::stats, 2)) {
    Stats::emitInc(a, Stats::TraceletGuard_enter);
  }

  bool pseudoMain = Translator::liveFrameIsPseudoMain();

  emitRB(a, RBTypeTraceletGuards, sk);
  for (DepMap::const_iterator dep = dependencies.begin();
       dep != dependencies.end();
       ++dep) {
    if (!pseudoMain || !dep->second->isLocal() || !dep->second->isValue()) {
      checkType(a, dep->first, dep->second->rtt, fail);
    } else {
      TRACE(3, "Skipping tracelet guard for %d:%d %d\n",
            (int)dep->second->location.space,
            (int)dep->second->location.offset,
            (int)dep->second->rtt.outerType());
    }
  }

  checkRefs(a, sk, refDeps, fail);

  if (Trace::moduleEnabled(Trace::stats, 2)) {
    Stats::emitInc(a, Stats::TraceletGuard_execute);
  }
}

static void dumpTranslationInfo(const Tracelet& t, TCA postGuards) {
  if (!debug) return;

  const SrcKey& sk = t.m_sk;

  TRACE(3, "----------------------------------------------\n");
  TRACE(3, "  Translating from file %s:%d %s at %p:\n",
        curUnit()->filepath()->data(),
        curUnit()->getLineNumber(sk.offset()),
        curFunc()->name()->data(),
        postGuards);
  TRACE(3, "  preconds:\n");
  TRACE(3, "    types:\n");
  for (DepMap::const_iterator i = t.m_dependencies.begin();
       i != t.m_dependencies.end(); ++i) {
    TRACE(3, "      %-5s\n", i->second->pretty().c_str());
  }
  if (t.m_refDeps.size() != 0) {
    TRACE(3, "    refs:\n");
    for (RefDeps::ArMap::const_iterator i = t.m_refDeps.m_arMap.begin();
        i != t.m_refDeps.m_arMap.end();
        ++i) {
      TRACE(3, "      (ActRec %lld : %-5s)\n", i->first,
        i->second.pretty().c_str());
    }
  }
  TRACE(3, "  postconds:\n");
  for (ChangeMap::const_iterator i = t.m_changes.begin();
       i != t.m_changes.end(); ++i) {
    TRACE(3, "    %-5s\n", i->second->pretty().c_str());
  }
  for (SrcKey traceKey(t.m_sk);
      traceKey != t.m_nextSk;
      traceKey.advance(curUnit())) {
    string s = instrToString(
      curUnit()->at(traceKey.offset()), curUnit());
    TRACE(3, "  %6d: %s\n", traceKey.offset(), s.c_str());
  }
  TRACE(3, "----------------------------------------------\n");
  if (Trace::moduleEnabled(Trace::tx64, 5)) {
    // prettyStack() expects to use vmpc(). Leave it in the state we
    // found it since this code is debug-only, and we don't want behavior
    // to vary across the optimized/debug builds.
    PC oldPC = vmpc();
    vmpc() = curUnit()->at(sk.offset());
    TRACE(3, g_vmContext->prettyStack(string(" tx64 ")));
    vmpc() = oldPC;
    TRACE(3, "----------------------------------------------\n");
  }
}

void
TranslatorX64::translateTracelet(const Tracelet& t) {
  const SrcKey &sk = t.m_sk;

  SKTRACE(1, sk, "translateTracelet\n");
  ASSERT(m_srcDB.find(sk));
  TCA                     start = a.code.frontier;
  TCA                     stubStart = astubs.code.frontier;
  SrcRec&                 srcRec = *getSrcRec(sk);
  vector<TransBCMapping>  bcMapping;

  try {
    if (t.m_analysisFailed || checkTranslationLimit(t.m_sk, srcRec)) {
      punt();
    }

    emitGuardChecks(a, t.m_sk, t.m_dependencies, t.m_refDeps, srcRec);
    dumpTranslationInfo(t, a.code.frontier);

    // Now that all check passed, add a counter for the translation if requested
    if (RuntimeOption::EvalJitTransCounters) {
      emitTransCounterInc(a);
    }

    emitRB(a, RBTypeTraceletBody, t.m_sk);
    Stats::emitInc(a, Stats::Instr_TC, t.m_numOpcodes);
    recordBCInstr(OpTraceletGuard, a, start);

    // Translate each instruction in the tracelet
    for (NormalizedInstruction* ni = t.m_instrStream.first; ni; ni = ni->next) {

      if (isTransDBEnabled()) {
        bcMapping.push_back((TransBCMapping){ni->offset(),
                                             a.code.frontier,
                                             astubs.code.frontier});
      }

      translateInstr(t, *ni);
      ASSERT(ni->source.offset() >= curFunc()->base());
      // We sometimes leave the tail of a truncated tracelet in place to aid
      // analysis, but breaksBB is authoritative.
      if (ni->breaksBB) break;
    }
  } catch (TranslationFailedExc& tfe) {
    // The whole translation failed; give up on this BB. Since it is not
    // linked into srcDB yet, it is guaranteed not to be reachable.
    m_regMap.reset();
    // Permanent reset; nothing is reachable yet.
    a.code.frontier = start;
    astubs.code.frontier = stubStart;
    bcMapping.clear();
    // Discard any pending fixups.
    m_pendingFixups.clear();
    TRACE(1, "emitting %d-instr interp request for failed translation @%s:%d\n",
          int(t.m_numOpcodes), tfe.m_file, tfe.m_line);
    // Add a counter for the translation if requested
    if (RuntimeOption::EvalJitTransCounters) {
      emitTransCounterInc(a);
    }
    a.    jmp(
              emitServiceReq(REQ_INTERPRET, 2ull, uint64_t(t.m_sk.offset()),
                             uint64_t(t.m_numOpcodes)));
    // Fall through.
  }
  for (uint i = 0; i < m_pendingFixups.size(); i++) {
    TCA tca = m_pendingFixups[i].m_tca;
    ASSERT(isValidCodeAddress(tca));
    m_fixupMap.recordFixup(tca, m_pendingFixups[i].m_fixup);
  }
  m_pendingFixups.clear();

  addTranslation(TransRec(t.m_sk, curUnit()->md5(), t, start,
                          a.code.frontier - start, stubStart,
                          astubs.code.frontier - stubStart, bcMapping));

  recordGdbTranslation(sk, curUnit(), a, start,
                       false, false);
  recordGdbTranslation(sk, curUnit(), astubs, stubStart,
                       false, false);
  // SrcRec::newTranslation() makes this code reachable. Do this last;
  // otherwise there's some chance of hitting in the reader threads whose
  // metadata is not yet visible.
  TRACE(1, "newTranslation: %p\n", start);
  srcRec.newTranslation(a, astubs, start);
  m_regMap.reset();
  TRACE(1, "tx64: %zd-byte tracelet\n", a.code.frontier - start);
  if (Trace::moduleEnabledRelease(Trace::tcspace, 1)) {
    Trace::traceRelease(getUsage().c_str());
  }
}

static const size_t kASize = 512 << 20;
static const size_t kAStubsSize = 512 << 20;
static const size_t kGDataSize = kASize / 4;
static const size_t kTotalSize = kASize + kAStubsSize +
                                         kTrampolinesBlockSize + kGDataSize;

TranslatorX64::TranslatorX64()
: Translator(),
  m_numNativeTrampolines(0),
  m_trampolineSize(0),
  m_spillFillCode(&a),
  m_interceptHelper(0),
  m_defClsHelper(0),
  m_funcPrologueRedispatch(0),
  m_regMap(kCallerSaved, kCalleeSaved, this),
  m_interceptsEnabled(false),
  m_unwindRegMap(128)
{
  TRACE(1, "TranslatorX64@%p startup\n", this);
  tx64 = this;

  static_assert(kTotalSize < (2ul << 30),
                "Combined size of all code/data blocks in TranslatorX64 "
                "must be < 2GiB to support 32-bit relative addresses");

  static bool profileUp = false;
  if (!profileUp) {
    profileInit();
    profileUp = true;
  }

  // We want to ensure that the block for "a", "astubs",
  // "atrampolines", and "m_globalData" are nearby so that we can
  // short jump/point between them. Thus we allocate one slab and
  // divide it between "a", "astubs", and "atrampolines".
  uint8_t *base = allocSlab(kTotalSize);
  atrampolines.init(base, kTrampolinesBlockSize);
  base += kTrampolinesBlockSize;
  a.init(base, kASize);
  m_unwindRegistrar = register_unwind_region(base, kTotalSize);
  base += kASize;
  astubs.init(base, kAStubsSize);
  base += kAStubsSize;
  m_globalData.init(base, kGDataSize);

  // Emit some special helpers that are shared across translations.

  // Emit a byte of padding. This is a kind of hacky way to
  // avoid hitting an assert in recordGdbStub when we call
  // it with m_callToExit - 1 as the start address.
  astubs.emitNop(1);

  // Call to exit with whatever value the program leaves on
  // the return stack.
  m_callToExit = emitServiceReq(false, REQ_EXIT, 0ull);

  m_retHelper = emitRetFromInterpretedFrame();

  moveToAlign(astubs);
  m_resumeHelper = astubs.code.frontier;
  emitGetGContext(astubs, rax);
  astubs.   load_reg64_disp_reg64(rax, offsetof(VMExecutionContext, m_fp),
                                       rVmFp);
  astubs.   load_reg64_disp_reg64(rax, offsetof(VMExecutionContext, m_stack) +
                                       Stack::topOfStackOffset(), rVmSp);
  emitServiceReq(false, REQ_RESUME, 0ull);

  // Helper for DefCls
  if (false) {
    PreClass *preClass = 0;
    defClsHelper(preClass);
  }
  m_defClsHelper = TCA(a.code.frontier);
  PhysReg rEC = argNumToRegName[2];
  emitGetGContext(a, rEC);
  a.   store_reg64_disp_reg64(rVmFp, offsetof(VMExecutionContext, m_fp), rEC);
  a.   store_reg64_disp_reg64(argNumToRegName[1],
                              offsetof(VMExecutionContext, m_pc), rEC);
  // rax holds the up-to-date top of stack pointer
  a.   store_reg64_disp_reg64(rax,
                              offsetof(VMExecutionContext, m_stack) +
                              Stack::topOfStackOffset(), rEC);
  a.   jmp((TCA)defClsHelper);

  moveToAlign(astubs);
  m_stackOverflowHelper = astubs.code.frontier;
  // We are called from emitStackCheck, with the new stack frame in
  // rStashedAR. Get the caller's PC into rdi and save it off.
  astubs.    load_reg64_disp_reg64(rVmFp, AROFF(m_func), rax);
  astubs.    load_reg64_disp_reg32(rStashedAR, AROFF(m_soff), rdi);
  astubs.    load_reg64_disp_reg64(rax, Func::sharedOffset(), rax);
  astubs.    load_reg64_disp_reg32(rax, Func::sharedBaseOffset(), rax);
  astubs.    add_reg32_reg32(rax, rdi);

  emitEagerVMRegSave(astubs, SaveFP | SavePC);
  emitServiceReq(false, REQ_STACK_OVERFLOW, 0ull);

  // The decRef helper for when we bring the count down to zero. Callee needs to
  // bring the value into rdi. These can be burned in for all time, and for all
  // translations.
  if (false) { // type-check
    StringData* str = NULL;
    ArrayData* arr = NULL;
    ObjectData* obj = NULL;
    RefData* ref = NULL;
    tv_release_str(str);
    tv_release_arr(arr);
    tv_release_obj(obj);
    tv_release_ref(ref);
  }
  typedef void* vp;
  m_dtorStubs[BitwiseKindOfString] = emitUnaryStub(a, vp(tv_release_str));
  m_dtorStubs[KindOfArray]         = emitUnaryStub(a, vp(tv_release_arr));
  m_dtorStubs[KindOfObject]        = emitUnaryStub(a, vp(tv_release_obj));
  m_dtorStubs[KindOfRef]           = emitUnaryStub(a, vp(tv_release_ref));
  m_dtorGenericStub                = genericRefCountStub(a);
  m_typedDtorStub                  = emitBinaryStub(a, vp(tv_release_typed));

  if (trustSigSegv) {
    // Install SIGSEGV handler for timeout exceptions
    struct sigaction sa;
    struct sigaction old_sa;
    sa.sa_sigaction = &TranslatorX64::SEGVHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &old_sa) != 0) {
      throw std::runtime_error(
        std::string("Failed to install SIGSEGV handler: ") +
          strerror(errno));
    }
    m_segvChain = old_sa.sa_flags & SA_SIGINFO ?
      old_sa.sa_sigaction : (sigaction_t)old_sa.sa_handler;
  }
}

// do gdb specific initialization. This has to happen after
// the TranslatorX64 constructor is called, because gdb initialization
// calls backs into TranslatorX64::Get()
void TranslatorX64::initGdb() {
  // On a backtrace, gdb tries to locate the calling frame at address
  // returnRIP-1. However, for the first VM frame, there is no code at
  // returnRIP-1, since the AR was set up manually. For this frame,
  // record the tracelet address as starting from callToExit-1, so gdb
  // does not barf
  recordGdbStub(astubs, m_callToExit - 1, "HHVM::callToExit");

  recordBCInstr(OpRetFromInterp, astubs, m_retHelper);
  recordGdbStub(astubs, m_retHelper - 1, "HHVM::retHelper");
  recordBCInstr(OpResumeHelper, astubs, m_resumeHelper);
  recordBCInstr(OpDefClsHelper, a, m_defClsHelper);
  recordBCInstr(OpDtorStub, a, m_dtorStubs[BitwiseKindOfString]);
  recordGdbStub(a, m_dtorStubs[BitwiseKindOfString],
                    "HHVM::destructorStub");
}

TranslatorX64*
TranslatorX64::Get() {
  /*
   * Called from outrageously early, pre-main code, and will
   * allocate the first translator space.
   */
  if (!nextTx64) {
    nextTx64 = new TranslatorX64();
    nextTx64->initGdb();
  }
  if (!tx64) {
    tx64 = nextTx64;
  }
  ASSERT(tx64);
  return tx64;
}

template<int Arity>
TCA TranslatorX64::emitNAryStub(X64Assembler& a, void* fptr) {
  BOOST_STATIC_ASSERT((Arity < kNumRegisterArgs));

  // The callNAryStub has already saved these regs on a.
  RegSet alreadySaved;
  for (size_t i = 0; i < Arity; ++i) {
    alreadySaved |= RegSet(argNumToRegName[i]);
  }

  /*
   * We've made a call instruction, and pushed Arity args on the
   * stack.  So the stack address will be odd coming into the stub if
   * Arity + 1 (for the call) is odd.  We need to correct for this
   * when saving other registers below to keep SSE-friendly alignment
   * of the stack.
   */
  const int Parity = (Arity + 1) % 2;

  // These dtor stubs are meant to be called with the call
  // instruction, unlike most translator code.
  moveToAlign(a);
  TCA start = a.code.frontier;
  /*
   * Preserve most caller-saved regs. The calling code has already
   * preserved regs in `alreadySaved'; we push the rest of the caller
   * saved regs and rbp.  It should take 9 qwords in total, and the
   * incoming call instruction made it 10.  This is an even number of
   * pushes, so we preserve the SSE-friendliness of our execution
   * environment (without real intervention from PhysRegSaverParity).
   *
   * Note that we don't need to clean all registers because the only
   * reason we could need those locations written back is if stack
   * unwinding were to happen.  These stubs can re-enter due to user
   * destructors, but exceptions are not allowed to propagate out of
   * those, so it's not a problem.
   */
  a.    pushr(rbp); // {
  a.    mov_reg64_reg64(rsp, rbp);
  {
    RegSet s = kCallerSaved - alreadySaved;
    PhysRegSaverParity<Parity> rs(a, s);
    emitCall(a, TCA(fptr));
  }
  a.    popr(rbp);  // }
  a.    ret();
  return start;
}

TCA TranslatorX64::emitUnaryStub(X64Assembler& a, void* fptr) {
  return emitNAryStub<1>(a, fptr);
}

TCA TranslatorX64::emitBinaryStub(X64Assembler& a, void* fptr) {
  return emitNAryStub<2>(a, fptr);
}

/*
 * Both callUnaryStubImpl and callBinaryStub assume that the stub they
 * are calling cannot throw an exception.
 */

template <bool reentrant>
void
TranslatorX64::callUnaryStubImpl(X64Assembler& a,
                                 const NormalizedInstruction& i,
                                 TCA stub, PhysReg arg, int disp/*=0*/) {
  // Call the generic dtor stub. They all take one arg.
  a.    pushr(rdi);
  if (disp == 0) {
    emitMovRegReg(a, arg, rdi);
  } else {
    a.    lea_reg64_disp_reg64(arg, disp, rdi);
  }
  ASSERT(isValidCodeAddress(stub));
  emitCall(a, stub);
  recordCallImpl<reentrant>(a, i);
  a.    popr(rdi);
}

void
TranslatorX64::callBinaryStub(X64Assembler& a, const NormalizedInstruction& i,
                              TCA stub, PhysReg arg1, PhysReg arg2) {
  a.    pushr(rdi);
  a.    pushr(rsi);

  // We need to be careful not to clobber our arguments when moving
  // them into the appropriate registers.  (If we ever need ternary
  // stubs, this should probably be converted to use ArgManager.)
  if (arg2 == rdi && arg1 == rsi) {
    a.  xchg_reg64_reg64(rdi, rsi);
  } else if (arg2 == rdi) {
    emitMovRegReg(a, arg2, rsi);
    emitMovRegReg(a, arg1, rdi);
  } else {
    emitMovRegReg(a, arg1, rdi);
    emitMovRegReg(a, arg2, rsi);
  }

  ASSERT(isValidCodeAddress(stub));
  emitCall(a, stub);
  recordReentrantCall(a, i);
  a.    popr(rsi);
  a.    popr(rdi);
}

namespace {

struct DeferredFileInvalidate : public DeferredWorkItem {
  Eval::PhpFile* m_f;
  DeferredFileInvalidate(Eval::PhpFile* f) : m_f(f) {
    TRACE(2, "DeferredFileInvalidate @ %p, m_f %p\n", this, m_f); }
  void operator()() {
    TRACE(2, "DeferredFileInvalidate: Firing @ %p , m_f %p\n", this, m_f);
    tx64->invalidateFileWork(m_f);
  }
};

struct DeferredPathInvalidate : public DeferredWorkItem {
  const std::string m_path;
  DeferredPathInvalidate(const std::string& path) : m_path(path) {
    ASSERT(m_path.size() >= 1 && m_path[0] == '/');
  }
  void operator()() {
    String spath(m_path);
    /*
     * inotify saw this path change. Now poke the file repository;
     * it will notice the underlying PhpFile* has changed, and notify
     * us via ::invalidateFile.
     *
     * We don't actually need to *do* anything with the PhpFile* from
     * this lookup; since the path has changed, the file we'll get out is
     * going to be some new file, not the old file that needs invalidation.
     */
    UNUSED Eval::PhpFile* f =
      g_vmContext->lookupPhpFile(spath.get(), "");
    // We don't keep around the extra ref.
    if (f) f->decRefAndDelete();
  }
};

}

void
TranslatorX64::requestInit() {
  TRACE(1, "in requestInit(%ld)\n", g_vmContext->m_currentThreadIdx);
  tl_regState = REGSTATE_CLEAN;
  PendQ::drain();
  requestResetHighLevelTranslator();
  Treadmill::startRequest(g_vmContext->m_currentThreadIdx);
  memset(&s_perfCounters, 0, sizeof(s_perfCounters));
}

void
TranslatorX64::requestExit() {
  if (s_writeLease.amOwner()) {
    s_writeLease.drop();
  }
  TRACE_MOD(txlease, 2, "%lx write lease stats: %15lld kept, %15lld grabbed\n",
            pthread_self(), s_writeLease.m_hintKept,
            s_writeLease.m_hintGrabbed);
  PendQ::drain();
  Treadmill::finishRequest(g_vmContext->m_currentThreadIdx);
  TRACE(1, "done requestExit(%ld)\n", g_vmContext->m_currentThreadIdx);
  Stats::dump();
  Stats::clear();

  if (Trace::moduleEnabledRelease(Trace::tx64stats, 1)) {
    Trace::traceRelease("TranslatorX64 perf counters for %s:\n",
                        g_context->getRequestUrl(50).c_str());
    for (int i = 0; i < tpc_num_counters; i++) {
      Trace::traceRelease("%-20s %10lld\n",
                          kPerfCounterNames[i], s_perfCounters[i]);
    }
    Trace::traceRelease("\n");
  }
}

bool
TranslatorX64::isPseudoEvent(const char* event) {
  for (int i = 0; i < tpc_num_counters; i++) {
    if (!strcmp(event, kPerfCounterNames[i])) {
      return true;
    }
  }
  return false;
}

void
TranslatorX64::getPerfCounters(Array& ret) {
  for (int i = 0; i < tpc_num_counters; i++) {
    // Until Perflab can automatically scale the values we give it to
    // an appropriate range, we have to fudge these numbers so they
    // look more like reasonable hardware counter values.
    ret.set(kPerfCounterNames[i], s_perfCounters[i] * 1000);
  }
}

TranslatorX64::~TranslatorX64() {
  freeSlab(atrampolines.code.base, kTotalSize);
}

static Debug::TCRange rangeFrom(const X64Assembler& a, const TCA addr,
                                bool isAstubs) {
  ASSERT(a.code.isValidAddress(addr));
  return Debug::TCRange(addr, a.code.frontier, isAstubs);
}

void TranslatorX64::recordBCInstr(uint32_t op,
                                  const X64Assembler& a,
                                  const TCA addr) {
  if (addr != a.code.frontier) {
    m_debugInfo.recordBCInstr(Debug::TCRange(addr, a.code.frontier,
                                             &a == &astubs ? true : false), op);
  }
}

void TranslatorX64::recordGdbTranslation(const SrcKey& sk,
                                         const Unit* srcUnit,
                                         const X64Assembler& a,
                                         const TCA start,
                                         bool exit,
                                         bool inPrologue) {
  if (start != a.code.frontier && !RuntimeOption::EvalJitNoGdb) {
    ASSERT(s_writeLease.amOwner());
    m_debugInfo.recordTracelet(rangeFrom(a, start,
                                         &a == &astubs ? true : false),
                               srcUnit,
                               srcUnit->at(sk.offset()),
                               exit, inPrologue);
  }
}

void TranslatorX64::recordGdbStub(const X64Assembler& a,
                                  const TCA start, const char* name) {
  if (!RuntimeOption::EvalJitNoGdb) {
    m_debugInfo.recordStub(rangeFrom(a, start, &a == &astubs ? true : false),
                           name);
  }
}

void TranslatorX64::defineCns(StringData* name) {
  TargetCache::fillConstant(name);
}

std::string TranslatorX64::getUsage() {
  std::string usage;
  size_t aUsage = a.code.frontier - a.code.base;
  size_t stubsUsage = astubs.code.frontier - astubs.code.base;
  size_t tcUsage = TargetCache::s_frontier;
  Util::string_printf(usage,
                      "tx64: %9zd bytes (%ld%%) in a.code\n"
                      "tx64: %9zd bytes (%ld%%) in astubs.code\n"
                      "tx64: %9zd bytes (%ld%%) in targetCache\n",
                      aUsage,     100 * aUsage / a.code.size,
                      stubsUsage, 100 * stubsUsage / astubs.code.size,
                      tcUsage,
                      100 * tcUsage / TargetCache::tl_targetCaches.size);
  return usage;
}

bool TranslatorX64::addDbgGuards(const Unit* unit) {
  // TODO refactor
  // It grabs the write lease and iterating through whole SrcDB...
  bool locked = s_writeLease.acquire(true);
  if (!locked) {
    return false;
  }
  struct timespec tsBegin, tsEnd;
  gettime(CLOCK_MONOTONIC, &tsBegin);
  // Doc says even find _could_ invalidate iterator, in pactice it should
  // be very rare, so go with it now.
  for (SrcDB::iterator it = m_srcDB.begin(); it != m_srcDB.end(); ++it) {
    SrcKey const sk = SrcKey::fromAtomicInt(it->first);
    SrcRec& sr = *it->second;
    if (sr.unitMd5() == unit->md5() &&
        !sr.hasDebuggerGuard() &&
        isSrcKeyInBL(unit, sk)) {
      addDbgGuardImpl(sk, sr);
    }
  }
  s_writeLease.drop();
  gettime(CLOCK_MONOTONIC, &tsEnd);
  int64 elapsed = gettime_diff_us(tsBegin, tsEnd);
  if (Trace::moduleEnabledRelease(Trace::tx64, 5)) {
    Trace::traceRelease("addDbgGuards got lease for %lld us\n", elapsed);
  }
  return true;
}

bool TranslatorX64::addDbgGuard(const Func* func, Offset offset) {
  SrcKey sk(func, offset);
  {
    if (SrcRec* sr = m_srcDB.find(sk)) {
      if (sr->hasDebuggerGuard()) {
        return true;
      }
    } else {
      // no translation yet
      return true;
    }
  }
  if (debug) {
    if (!isSrcKeyInBL(func->unit(), sk)) {
      TRACE(5, "calling addDbgGuard on PC that is not in blacklist");
      return false;
    }
  }
  bool locked = s_writeLease.acquire(true);
  if (!locked) {
    return false;
  }
  {
    if (SrcRec* sr = m_srcDB.find(sk)) {
      addDbgGuardImpl(sk, *sr);
    }
  }
  s_writeLease.drop();
  return true;
}

void TranslatorX64::addDbgGuardImpl(const SrcKey& sk, SrcRec& srcRec) {
  TCA dbgGuard = a.code.frontier;
  // Emit the checks for debugger attach
  emitTLSLoad<ThreadInfo>(a, ThreadInfo::s_threadInfo, rScratch);
  static COff dbgOff = offsetof(ThreadInfo, m_reqInjectionData) +
                       offsetof(RequestInjectionData, debugger);
  a.   load_reg64_disp_reg32(rScratch, dbgOff, rScratch);
  a.   test_imm32_reg32(0xff, rScratch);
  // Branch to a special REQ_INTERPRET if attached
  {
    TCA fallback = emitServiceReq(REQ_INTERPRET, 2, uint64_t(sk.offset()), 0);
    a. jnz(fallback);
  }
  // Emit a jump to the actual code
  TCA realCode = srcRec.getTopTranslation();
  prepareForSmash(kJmpLen);
  TCA dbgBranchGuardSrc = a.code.frontier;
  a.   jmp(realCode);
  // Add it to srcRec
  srcRec.addDebuggerGuard(a, astubs, dbgGuard, dbgBranchGuardSrc);
}

bool TranslatorX64::dumpTCCode(const char* filename) {
  string aFilename = string(filename).append("_a");
  string astubFilename = string(filename).append("_astub");
  FILE* aFile = fopen(aFilename.c_str(),"wb");
  if (aFile == NULL)
    return false;
  FILE* astubFile = fopen(astubFilename.c_str(),"wb");
  if (astubFile == NULL) {
    fclose(aFile);
    return false;
  }
  string helperAddrFilename = string(filename).append("_helpers_addrs.txt");
  FILE* helperAddrFile = fopen(helperAddrFilename.c_str(),"wb");
  if (helperAddrFile == NULL) {
    fclose(aFile);
    fclose(astubFile);
    return false;
  }
  // dump starting from the trampolines; this assumes processInit() places
  // trampolines before the translation cache
  size_t count = a.code.frontier-atrampolines.code.base;
  bool result = (fwrite(atrampolines.code.base, 1, count, aFile) == count);
  if (result) {
    count = astubs.code.frontier - astubs.code.base;
    result = (fwrite(astubs.code.base, 1, count, astubFile) == count);
  }
  if (result) {
    for(PointerMap::iterator iter = trampolineMap.begin();
        iter != trampolineMap.end();
        iter++) {
      void* helperAddr = iter->first;
      void* trampAddr = iter->second;
      char* functionName = Util::getNativeFunctionName(helperAddr);
      fprintf(helperAddrFile,"%10p %10p %s\n",
              trampAddr, helperAddr,
              functionName);
      free(functionName);
    }
  }
  fclose(aFile);
  fclose(astubFile);
  fclose(helperAddrFile);
  return result;
}

// Returns true on success
bool TranslatorX64::dumpTC() {
  if (!s_writeLease.acquire(true)) return false;
  bool success = dumpTCData();
  if (success) {
    success = dumpTCCode("/tmp/tc_dump");
  }
  s_writeLease.drop();
  return success;
}

// Returns true on success
bool tc_dump(void) {
  return TranslatorX64::Get()->dumpTC();
}

// Returns true on success
bool TranslatorX64::dumpTCData() {
  gzFile tcDataFile = gzopen("/tmp/tc_data.txt.gz", "w");
  if (!tcDataFile) return false;

  if (!gzprintf(tcDataFile,
                "repo_schema     = %s\n"
                "a.base          = %p\n"
                "a.frontier      = %p\n"
                "astubs.base     = %p\n"
                "astubs.frontier = %p\n\n",
                Repo::kSchemaId,
                atrampolines.code.base, a.code.frontier,
                astubs.code.base, astubs.code.frontier)) {
    return false;
  }

  if (!gzprintf(tcDataFile, "total_translations = %lu\n\n",
                m_translations.size())) {
    return false;
  }

  for (size_t t = 0; t < m_translations.size(); t++) {
    if (gzputs(tcDataFile,
               m_translations[t].print(getTransCounter(t)).c_str()) == -1) {
      return false;
    }
  }

  gzclose(tcDataFile);
  return true;
}

#define NATIVE_OP(X) PLAN(X, Native)
#define SUPPORTED_OP(X) PLAN(X, Supported)
#define SIMPLE_OP(X) PLAN(X, Simple)
#define INTERP_OP(X) PLAN(X, Interp)

#define SUPPORTED_OPS() \
  NATIVE_OP(Null) \
  NATIVE_OP(True) \
  NATIVE_OP(False) \
  NATIVE_OP(Int) \
  NATIVE_OP(String) \
  NATIVE_OP(Array) \
  NATIVE_OP(NewArray) \
  NATIVE_OP(InitThisLoc) \
  NATIVE_OP(Dup) \
  NATIVE_OP(FPushContFunc) \
  NATIVE_OP(ContDone) \
  NATIVE_OP(ContValid) \
  NATIVE_OP(ContStopped) \
  /*
   * Invariably call a possibly-reentrant helper.
   */ \
  SIMPLE_OP(Jmp) \
  SIMPLE_OP(FCall) \
  SIMPLE_OP(CreateCont) \
  SIMPLE_OP(UnpackCont) \
  /*
   * Translations with a reentrant helper.
   *
   * TODO: neither UnboxR nor FPassR can actually call destructors.
   */ \
  SUPPORTED_OP(UnboxR) \
  SUPPORTED_OP(FPassR) \
  SUPPORTED_OP(NativeImpl) \
  SUPPORTED_OP(UnsetL) \
  SUPPORTED_OP(Cns) \
  SUPPORTED_OP(ClsCnsD) \
  SUPPORTED_OP(This) \
  SUPPORTED_OP(PackCont) \
  SUPPORTED_OP(ContReceive) \
  SUPPORTED_OP(ContRaised) \
  SUPPORTED_OP(ContNext) \
  SUPPORTED_OP(ContSend) \
  SUPPORTED_OP(ContRaise) \
  SUPPORTED_OP(ContCurrent) \
  /*
   * Always-interp instructions,
   */ \
  INTERP_OP(VGetM) \
  INTERP_OP(ContHandle)

// Define the trivial analyze methods
#define PLAN(Op, Spt) \
void \
TranslatorX64::analyze ## Op(Tracelet& t, NormalizedInstruction& i) { \
  i.m_txFlags = Spt; \
}

SUPPORTED_OPS()

#undef NATIVE_OP
#undef SUPPORTED_OP
#undef SIMPLE_OP
#undef INTERP_OP
#undef SUPPORTED_OPS

void TranslatorX64::invalidateSrcKey(const SrcKey& sk) {
  ASSERT(!RuntimeOption::RepoAuthoritative);
  ASSERT(s_writeLease.amOwner());
  /*
   * Reroute existing translations for SrcKey to an as-yet indeterminate
   * new one.
   */
  SrcRec* sr = m_srcDB.find(sk);
  ASSERT(sr);
  /*
   * Since previous translations aren't reachable from here, we know we
   * just created some garbage in the TC. We currently have no mechanism
   * to reclaim this.
   */
  sr->replaceOldTranslations(a, astubs);
}

void TranslatorX64::invalidateFileWork(Eval::PhpFile* f) {
  class FileInvalidationTrigger : public Treadmill::WorkItem {
    Eval::PhpFile* m_f;
    int m_nRefs;
  public:
    FileInvalidationTrigger(Eval::PhpFile* f, int n) : m_f(f), m_nRefs(n) { }
    virtual void operator()() {
      if (m_f->decRef(m_nRefs) == 0) {
        Eval::FileRepository::onDelete(m_f);
      }
    }
  };
  size_t nSmashed = m_srcDB.invalidateCode(f);
  if (nSmashed) {
    // The srcDB found an entry for this file. The entry's dependency
    // on this file was counted as a reference, and the code is no longer
    // reachable. We need to wait until the last outstanding request
    // drains to know that we can really remove the reference.
    Treadmill::WorkItem::enqueue(new FileInvalidationTrigger(f, nSmashed));
  }
}

bool TranslatorX64::invalidateFile(Eval::PhpFile* f) {
  // This is called from high rank, but we'll need the write lease to
  // invalidate code.
  if (!RuntimeOption::EvalJit) return false;
  ASSERT(f != NULL);
  PendQ::defer(new DeferredFileInvalidate(f));
  return true;
}

} // HPHP::VM::Transl

static const Trace::Module TRACEMOD = Trace::tx64;

void invalidatePath(const std::string& path) {
  TRACE(1, "invalidatePath: abspath %s\n", path.c_str());
  PendQ::defer(new DeferredPathInvalidate(path));
}

} } // HPHP::VM
