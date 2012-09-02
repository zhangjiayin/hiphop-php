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

#ifndef incl_VM_HHBC_H_
#define incl_VM_HHBC_H_

#include <runtime/vm/core_types.h>

namespace HPHP {
namespace VM {

struct Unit;

// Variable-size immediates are implemented as follows. To determine which size
// the immediate is, examine the first byte where the immediate is expected, and
// examine its low-order bit. If it is zero, it's a 1-byte immediate; otherwise,
// it's 4 bytes. The immediate has to be logical-shifted to the right by one to
// get rid of the flag bit.
#define ARGTYPES \
  ARGTYPE(NA,    void*)         /* unused */  \
  ARGTYPEVEC(MA, int32_t)       /* Member vector immediate */ \
  ARGTYPEVEC(BLA,Offset)        /* Bytecode address vector immediate */ \
  ARGTYPE(IVA,   int32_t)       /* variable size: 8 or 32-bit integer */  \
  ARGTYPE(I64A,  int64_t)       /* 64-bit Integer */ \
  ARGTYPE(HA,    int32_t)       /* Local variable ID: 8 or 32-bit int */  \
                                /* TODO(jdelong): rename HA to LA */ \
  ARGTYPE(IA,    int32_t)       /* Iterator variable ID: 8 or 32-bit int */ \
  ARGTYPE(DA,    double)        /* Double */ \
  ARGTYPE(SA,    Id)            /* litStr ID */ \
  ARGTYPE(AA,    Id)            /* static array ID */ \
  ARGTYPE(BA,    Offset)        /* Bytecode address */ \
  ARGTYPE(OA,    unsigned char) /* Opcode */

enum ArgType {
#define ARGTYPE(name, type) name,
#define ARGTYPEVEC(name, type) name,
  ARGTYPES
#undef ARGTYPE
#undef ARGTYPEVEC
};

union ArgUnion {
  char bytes[0];
#define ARGTYPE(name, type) type u_##name;
#define ARGTYPEVEC(name, type) type u_##name;
  ARGTYPES
#undef ARGTYPE
#undef ARGTYPEVEC
};

static const Offset InvalidAbsoluteOffset = -1;

enum FlavorDesc {
  NOV = 0, // None
  CV = 1,  // Cell
  VV = 2,  // Var
  AV = 3,  // Classref
  RV = 4,  // Return value (cell or var)
  FV = 5,  // Function parameter (cell or var)
};

enum InstrFlags {
  NF = 0x0, // No flags
  TF = 0x1, // Next instruction is not reachable via fall through or return
  CF = 0x2, // Control flow instruction (branch, call, return, throw, etc)
  FF = 0x4, // Instruction uses current FPI
  CF_TF = (CF | TF),
  CF_FF = (CF | FF)
};

enum LocationCode {
  // Base is the object stored in a cell or a local.
  LL,
  LC,

  // Base is the global name referred to by a cell or a local.
  LGL,
  LGC,

  // Base is the name of a local, given by a cell or the value of a
  // local.
  LNL,
  LNC,

  /*
   * Base is a static property member of a class.  The S-vector takes
   * two things to define a base.  The classref portion comes at the
   * end of the M-vector, and the property name can be defined by
   * either a cell or a local immediate.
   */
  LSL,
  LSC,

  // Base is a function return value.
  LR,

  NumLocationCodes,
  InvalidLocationCode = NumLocationCodes
};

inline int numLocationCodeImms(LocationCode lc) {
  return
    (lc == LL || lc == LGL || lc == LNL || lc == LSL) ? 1 :
    0;
}

inline int numLocationCodeStackVals(LocationCode lc) {
  return
    (lc == LR || lc == LC || lc == LGC || lc == LNC || lc == LSL) ? 1 :
    (lc == LSC) ? 2 :
    0;
}

// Returns string representation of `lc'.  (Pointer to internal static
// data, does not need to be freed.)
const char* locationCodeString(LocationCode lc);

// Grok a LocationCode from a string---if the string doesn't represent
// a location code, returns InvalidLocationCode.  This looks at at
// most the first two bytes in `s'---the parse will not fail if there
// is more junk after the first two bytes.
LocationCode parseLocationCode(const char* s);

enum MemberCode {
  // Element and property, consuming a cell from the stack.
  MEC,
  MPC,

  // Element and property, using an immediate local id.
  MEL,
  MPL,

  // Element and property, using a string immediate
  MET,
  MPT,

  // Element, using an int64 immediate
  MEI,

  // New element operation.  (No real stack element.)
  MW,

  NumMemberCodes,
  InvalidMemberCode = NumMemberCodes
};

inline bool memberCodeHasImm(MemberCode mc) {
  return mc == MEL || mc == MPL || mc == MET || mc == MPT || mc == MEI;
}

inline bool memberCodeImmIsLoc(MemberCode mc) {
  return mc == MEL || mc == MPL;
}

inline bool memberCodeImmIsString(MemberCode mc) {
  return mc == MET || mc == MPT;
}

inline bool memberCodeImmIsInt(MemberCode mc) {
  return mc == MEI;
}

// Returns string representation of `mc'.  (Pointer to internal static
// data, does not need to be freed.)
const char* memberCodeString(MemberCode mc);

// Same semantics as parseLocationCode, but for member codes.
MemberCode parseMemberCode(const char*);

enum IncDecOp {
  PreInc,
  PostInc,
  PreDec,
  PostDec,
  IncDec_invalid
};

// Each of the setop ops maps to a binary bytecode op. We have reasons
// for using distinct bitwise representations, though. This macro records
// their correspondence for mapping either direction.
#define SETOP_OPS \
  SETOP_OP(SetOpPlusEqual,   OpAdd) \
  SETOP_OP(SetOpMinusEqual,  OpSub) \
  SETOP_OP(SetOpMulEqual,    OpMul) \
  SETOP_OP(SetOpConcatEqual, OpConcat) \
  SETOP_OP(SetOpDivEqual,    OpDiv) \
  SETOP_OP(SetOpModEqual,    OpMod) \
  SETOP_OP(SetOpAndEqual,    OpBitAnd) \
  SETOP_OP(SetOpOrEqual,     OpBitOr) \
  SETOP_OP(SetOpXorEqual,    OpBitXor) \
  SETOP_OP(SetOpSlEqual,     OpShl) \
  SETOP_OP(SetOpSrEqual,     OpShr)

enum SetOpOp {
#define SETOP_OP(setOpOp, bcOp) setOpOp,
  SETOP_OPS
#undef SETOP_OP
  SetOp_invalid
};

//  name             immediates        inputs           outputs     flags
#define OPCODES \
  O(LowInvalid,      NA,               NOV,             NOV,        NF) \
  O(Nop,             NA,               NOV,             NOV,        NF) \
  O(PopC,            NA,               ONE(CV),         NOV,        NF) \
  O(PopV,            NA,               ONE(VV),         NOV,        NF) \
  O(PopR,            NA,               ONE(RV),         NOV,        NF) \
  O(Dup,             NA,               ONE(CV),         TWO(CV,CV), NF) \
  O(Box,             NA,               ONE(CV),         ONE(VV),    NF) \
  O(Unbox,           NA,               ONE(VV),         ONE(CV),    NF) \
  O(BoxR,            NA,               ONE(RV),         ONE(VV),    NF) \
  O(UnboxR,          NA,               ONE(RV),         ONE(CV),    NF) \
  O(Null,            NA,               NOV,             ONE(CV),    NF) \
  O(True,            NA,               NOV,             ONE(CV),    NF) \
  O(False,           NA,               NOV,             ONE(CV),    NF) \
  O(Int,             ONE(I64A),        NOV,             ONE(CV),    NF) \
  O(Double,          ONE(DA),          NOV,             ONE(CV),    NF) \
  O(String,          ONE(SA),          NOV,             ONE(CV),    NF) \
  O(Array,           ONE(AA),          NOV,             ONE(CV),    NF) \
  O(NewArray,        NA,               NOV,             ONE(CV),    NF) \
  O(AddElemC,        NA,               THREE(CV,CV,CV), ONE(CV),    NF) \
  O(AddElemV,        NA,               THREE(VV,CV,CV), ONE(CV),    NF) \
  O(AddNewElemC,     NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(AddNewElemV,     NA,               TWO(VV,CV),      ONE(CV),    NF) \
  O(Cns,             ONE(SA),          NOV,             ONE(CV),    NF) \
  O(ClsCns,          ONE(SA),          ONE(AV),         ONE(CV),    NF) \
  O(ClsCnsD,         TWO(SA,SA),       NOV,             ONE(CV),    NF) \
  O(File,            NA,               NOV,             ONE(CV),    NF) \
  O(Dir,             NA,               NOV,             ONE(CV),    NF) \
  O(Concat,          NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Add,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Sub,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Mul,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Div,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Mod,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Xor,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Not,             NA,               ONE(CV),         ONE(CV),    NF) \
  O(Same,            NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(NSame,           NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Eq,              NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Neq,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Lt,              NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Lte,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Gt,              NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Gte,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(BitAnd,          NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(BitOr,           NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(BitXor,          NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(BitNot,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(Shl,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(Shr,             NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(CastBool,        NA,               ONE(CV),         ONE(CV),    NF) \
  O(CastInt,         NA,               ONE(CV),         ONE(CV),    NF) \
  O(CastDouble,      NA,               ONE(CV),         ONE(CV),    NF) \
  O(CastString,      NA,               ONE(CV),         ONE(CV),    NF) \
  O(CastArray,       NA,               ONE(CV),         ONE(CV),    NF) \
  O(CastObject,      NA,               ONE(CV),         ONE(CV),    NF) \
  O(InstanceOf,      NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(InstanceOfD,     ONE(SA),          ONE(CV),         ONE(CV),    NF) \
  O(Print,           NA,               ONE(CV),         ONE(CV),    NF) \
  O(Clone,           NA,               ONE(CV),         ONE(CV),    NF) \
  O(Exit,            NA,               ONE(CV),         ONE(CV),    NF) \
  O(Raise,           NA,               TWO(CV,CV),      NOV,        NF) \
  O(Fatal,           NA,               ONE(CV),         NOV,        CF_TF) \
  O(Jmp,             ONE(BA),          NOV,             NOV,        CF_TF) \
  O(JmpZ,            ONE(BA),          ONE(CV),         NOV,        CF) \
  O(JmpNZ,           ONE(BA),          ONE(CV),         NOV,        CF) \
  O(Switch,          THREE(BLA,I64A,IVA),                               \
                                       ONE(CV),         NOV,        CF_TF) \
  O(RetC,            NA,               ONE(CV),         NOV,        CF_TF) \
  O(RetV,            NA,               ONE(VV),         NOV,        CF_TF) \
  O(Unwind,          NA,               NOV,             NOV,        CF_TF) \
  O(Throw,           NA,               ONE(CV),         NOV,        CF_TF) \
  O(CGetL,           ONE(HA),          NOV,             ONE(CV),    NF) \
  O(CGetL2,          ONE(HA),          NOV,             INS_1(CV),  NF) \
  O(CGetL3,          ONE(HA),          NOV,             INS_2(CV),  NF) \
  O(CGetN,           NA,               ONE(CV),         ONE(CV),    NF) \
  O(CGetG,           NA,               ONE(CV),         ONE(CV),    NF) \
  O(CGetS,           NA,               TWO(AV,CV),      ONE(CV),    NF) \
  O(CGetM,           ONE(MA),          LMANY(),         ONE(CV),    NF) \
  O(VGetL,           ONE(HA),          NOV,             ONE(VV),    NF) \
  O(VGetN,           NA,               ONE(CV),         ONE(VV),    NF) \
  O(VGetG,           NA,               ONE(CV),         ONE(VV),    NF) \
  O(VGetS,           NA,               TWO(AV,CV),      ONE(VV),    NF) \
  O(VGetM,           ONE(MA),          LMANY(),         ONE(VV),    NF) \
  O(AGetC,           NA,               ONE(CV),         ONE(AV),    NF) \
  O(AGetL,           ONE(HA),          NOV,             ONE(AV),    NF) \
  O(IssetC,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(IssetL,          ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IssetN,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(IssetG,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(IssetS,          NA,               TWO(AV,CV),      ONE(CV),    NF) \
  O(IssetM,          ONE(MA),          LMANY(),         ONE(CV),    NF) \
  O(EmptyL,          ONE(HA),          NOV,             ONE(CV),    NF) \
  O(EmptyN,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(EmptyG,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(EmptyS,          NA,               TWO(AV,CV),      ONE(CV),    NF) \
  O(EmptyM,          ONE(MA),          LMANY(),         ONE(CV),    NF) \
  /* NB: isTypePred depends on this ordering. */ \
  O(IsNullC,         NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsBoolC,         NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsIntC,          NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsDoubleC,       NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsStringC,       NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsArrayC,        NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsObjectC,       NA,               ONE(CV),         ONE(CV),    NF) \
  O(IsNullL,         ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsBoolL,         ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsIntL,          ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsDoubleL,       ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsStringL,       ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsArrayL,        ONE(HA),          NOV,             ONE(CV),    NF) \
  O(IsObjectL,       ONE(HA),          NOV,             ONE(CV),    NF) \
  O(SetL,            ONE(HA),          ONE(CV),         ONE(CV),    NF) \
  O(SetN,            NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(SetG,            NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(SetS,            NA,               THREE(CV,AV,CV), ONE(CV),    NF) \
  O(SetM,            ONE(MA),          C_LMANY(),       ONE(CV),    NF) \
  O(SetOpL,          TWO(HA, OA),      ONE(CV),         ONE(CV),    NF) \
  O(SetOpN,          ONE(OA),          TWO(CV,CV),      ONE(CV),    NF) \
  O(SetOpG,          ONE(OA),          TWO(CV,CV),      ONE(CV),    NF) \
  O(SetOpS,          ONE(OA),          THREE(CV,AV,CV), ONE(CV),    NF) \
  O(SetOpM,          TWO(OA,MA),       C_LMANY(),       ONE(CV),    NF) \
  O(IncDecL,         TWO(HA,OA),       NOV,             ONE(CV),    NF) \
  O(IncDecN,         ONE(OA),          ONE(CV),         ONE(CV),    NF) \
  O(IncDecG,         ONE(OA),          ONE(CV),         ONE(CV),    NF) \
  O(IncDecS,         ONE(OA),          TWO(AV,CV),      ONE(CV),    NF) \
  O(IncDecM,         TWO(OA,MA),       LMANY(),         ONE(CV),    NF) \
  O(BindL,           ONE(HA),          ONE(VV),         ONE(VV),    NF) \
  O(BindN,           NA,               TWO(VV,CV),      ONE(VV),    NF) \
  O(BindG,           NA,               TWO(VV,CV),      ONE(VV),    NF) \
  O(BindS,           NA,               THREE(VV,AV,CV), ONE(VV),    NF) \
  O(BindM,           ONE(MA),          V_LMANY(),       ONE(VV),    NF) \
  O(UnsetL,          ONE(HA),          NOV,             NOV,        NF) \
  O(UnsetN,          NA,               ONE(CV),         NOV,        NF) \
  O(UnsetG,          NA,               ONE(CV),         NOV,        NF) \
  O(UnsetM,          ONE(MA),          LMANY(),         NOV,        NF) \
  /* NOTE: isFPush below relies on the grouping of FPush* here */       \
  O(FPushFunc,       ONE(IVA),         ONE(CV),         NOV,        NF) \
  O(FPushFuncD,      TWO(IVA,SA),      NOV,             NOV,        NF) \
  O(FPushObjMethod,  ONE(IVA),         TWO(CV,CV),      NOV,        NF) \
  O(FPushObjMethodD, TWO(IVA,SA),      ONE(CV),         NOV,        NF) \
  O(FPushClsMethod,  ONE(IVA),         TWO(AV,CV),      NOV,        NF) \
  O(FPushClsMethodF, ONE(IVA),         TWO(AV,CV),      NOV,        NF) \
  O(FPushClsMethodD, THREE(IVA,SA,SA), NOV,             NOV,        NF) \
  O(FPushCtor,       ONE(IVA),         ONE(AV),         ONE(CV),    NF) \
  O(FPushCtorD,      TWO(IVA,SA),      NOV,             ONE(CV),    NF) \
  O(FPushContFunc,   ONE(IVA),         NOV,             NOV,        NF) \
  O(FPassC,          ONE(IVA),         ONE(CV),         ONE(FV),    FF) \
  O(FPassCW,         ONE(IVA),         ONE(CV),         ONE(FV),    FF) \
  O(FPassCE,         ONE(IVA),         ONE(CV),         ONE(FV),    FF) \
  O(FPassV,          ONE(IVA),         ONE(VV),         ONE(FV),    FF) \
  O(FPassR,          ONE(IVA),         ONE(RV),         ONE(FV),    FF) \
  O(FPassL,          TWO(IVA,HA),      NOV,             ONE(FV),    FF) \
  O(FPassN,          ONE(IVA),         ONE(CV),         ONE(FV),    FF) \
  O(FPassG,          ONE(IVA),         ONE(CV),         ONE(FV),    FF) \
  O(FPassS,          ONE(IVA),         TWO(AV,CV),      ONE(FV),    FF) \
  O(FPassM,          TWO(IVA,MA),      LMANY(),         ONE(FV),    FF) \
  O(FCall,           ONE(IVA),         FMANY,           ONE(RV),    CF_FF) \
  O(IterInit,        TWO(IA,BA),       ONE(CV),         NOV,        CF) \
  O(IterInitM,       TWO(IA,BA),       ONE(VV),         NOV,        CF) \
  O(IterValueC,      ONE(IA),          NOV,             ONE(CV),    NF) \
  O(IterValueV,      ONE(IA),          NOV,             ONE(VV),    NF) \
  O(IterKey,         ONE(IA),          NOV,             ONE(CV),    NF) \
  O(IterNext,        TWO(IA,BA),       NOV,             NOV,        CF) \
  O(IterFree,        ONE(IA),          NOV,             NOV,        NF) \
  O(Incl,            NA,               ONE(CV),         ONE(CV),    CF) \
  O(InclOnce,        NA,               ONE(CV),         ONE(CV),    CF) \
  O(Req,             NA,               ONE(CV),         ONE(CV),    CF) \
  O(ReqOnce,         NA,               ONE(CV),         ONE(CV),    CF) \
  O(ReqDoc,          NA,               ONE(CV),         ONE(CV),    CF) \
  O(ReqMod,          NA,               ONE(CV),         ONE(CV),    CF) \
  O(ReqSrc,          NA,               ONE(CV),         ONE(CV),    CF) \
  O(Eval,            NA,               ONE(CV),         ONE(CV),    CF) \
  O(DefFunc,         ONE(IVA),         NOV,             NOV,        NF) \
  O(DefCls,          ONE(IVA),         NOV,             NOV,        NF) \
  O(DefCns,          ONE(SA),          ONE(CV),         ONE(CV),    NF) \
  O(This,            NA,               NOV,             ONE(CV),    NF) \
  O(InitThisLoc,     ONE(IVA),         NOV,             NOV,        NF) \
  O(StaticLoc,       TWO(IVA,SA),      NOV,             ONE(CV),    NF) \
  O(StaticLocInit,   TWO(IVA,SA),      ONE(CV),         NOV,        NF) \
  O(Catch,           NA,               NOV,             ONE(CV),    NF) \
  O(ClassExists,     NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(InterfaceExists, NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(TraitExists,     NA,               TWO(CV,CV),      ONE(CV),    NF) \
  O(VerifyParamType, ONE(IVA),         NOV,             NOV,        NF) \
  O(Self,            NA,               NOV,             ONE(AV),    NF) \
  O(Parent,          NA,               NOV,             ONE(AV),    NF) \
  O(LateBoundCls,    NA,               NOV,             ONE(AV),    NF) \
  O(NativeImpl,      NA,               NOV,             NOV,        CF_TF) \
  O(CreateCont,      THREE(IVA,SA,SA), NOV,             ONE(CV),    NF) \
  O(UnpackCont,      NA,               NOV,             ONE(CV),    NF) \
  O(PackCont,        ONE(IVA),         ONE(CV),         NOV,        NF) \
  O(ContRaised,      NA,               NOV,             NOV,        NF) \
  O(ContReceive,     NA,               NOV,             ONE(CV),    NF) \
  O(ContDone,        NA,               NOV,             NOV,        NF) \
  O(ContNext,        NA,               NOV,             ONE(CV),    NF) \
  O(ContSend,        NA,               NOV,             ONE(CV),    NF) \
  O(ContRaise,       NA,               NOV,             ONE(CV),    NF) \
  O(ContValid,       NA,               NOV,             ONE(CV),    NF) \
  O(ContCurrent,     NA,               NOV,             ONE(CV),    NF) \
  O(ContStopped,     NA,               NOV,             NOV,        NF) \
  O(ContHandle,      NA,               ONE(CV),         NOV,        CF_TF) \
  O(HighInvalid,     NA,               NOV,             NOV,        NF) \

enum Op {
#define O(name, imm, pop, push, flags) Op##name,
  OPCODES
#undef O
  Op_count
};

enum AstubsOp {
  OpAstubStart = Op_count-1,
#define O(name, imm, pop, push, flags) OpAstub##name,
  OPCODES
#undef O
  OpAstubCount
};

#define HIGH_OPCODES \
  O(FuncPrologue) \
  O(TraceletGuard) \
  O(NativeTrampoline) \
  O(ServiceRequest) \
  O(DtorStub) \
  O(SyncOutputs) \
  O(RetFromInterp) \
  O(ResumeHelper) \
  O(RequireHelper) \
  O(DefClsHelper) \

enum HighOp {
  OpHighStart = OpAstubCount-1,
#define O(name) Op##name,
  HIGH_OPCODES
#undef O
};

struct ImmVector {
  explicit ImmVector() : m_start(0) {}

  explicit ImmVector(const uint8_t* start,
                     int32_t length,
                     int32_t numStack)
    : m_length(length)
    , m_numStack(numStack)
    , m_start(start)
  {}

  /*
   * Returns an ImmVector from a pointer to the immediate vector
   * itself.  Use getImmVector() if you want to get it from an Opcode*
   * that points to the opcode.
   */
  static ImmVector createFromStream(const uint8_t* opcode) {
    int32_t size = reinterpret_cast<const int32_t*>(opcode)[0];
    int32_t stackCount = reinterpret_cast<const int32_t*>(opcode)[1];
    const uint8_t* start = opcode + sizeof(int32_t) + sizeof(int32_t);
    return ImmVector(start, size, stackCount);
  }

  /*
   * Returns an ImmVector of 32-bit ints from a pointer to the
   * immediate vector itself.
   */
  static ImmVector createFromStream(const int32_t* stream) {
    int32_t size = stream[0];
    return ImmVector(reinterpret_cast<const uint8_t*>(stream + 1), size, 0);
  }

  bool isValid() const { return m_start != 0; }

  const uint8_t* vec() const { return m_start; }
  const int32_t* vec32() const {
    return reinterpret_cast<const int32_t*>(m_start);
  }

  LocationCode locationCode() const { return LocationCode(*vec()); }

  /*
   * Returns the length of the immediate vector in bytes.
   */
  int32_t size() const { return m_length; }

  /*
   * Returns the number of elements on the execution stack that this
   * vector will need to access.  (Includes stack elements for both
   * the base and members.)
   */
  int numStackValues() const { return m_numStack; }

  /*
   * Returns a pointer to the last member code in the vector.
   *
   * Requires: isValid() && size() >= 1
   */
  const uint8_t* findLastMember() const;

  /*
   * Decode the terminating string immediate, if any.
   */
  bool decodeLastMember(const Unit*, StringData*& sdOut,
                        MemberCode& membOut,
                        int64_t* strIdOut = NULL) const;


private:
  int32_t m_length;
  int32_t m_numStack;
  const uint8_t* m_start;
};

// Must be an opcode that actually has an ImmVector.
ImmVector getImmVector(const Opcode* opcode);

/* Some decoding helper functions. */
int numImmediates(Opcode opcode);
ArgType immType(Opcode opcode, int idx);
int immSize(const Opcode* opcode, int idx);
bool immIsVector(Opcode opcode, int idx);
bool hasImmVector(Opcode opcode);
static inline bool isTypePred(const Opcode op) {
  return op >= OpIsNullC && op <= OpIsObjectL;
}
int instrLen(const Opcode* opcode);
InstrFlags instrFlags(Opcode opcode);
int numSuccs(const Opcode* opcode);

// The returned struct has normalized variable-sized immediates
ArgUnion getImm(const Opcode* opcode, int idx);
// Don't use this with variable-sized immediates!
ArgUnion* getImmPtr(const Opcode* opcode, int idx);

// Pass a pointer to the pointer to the immediate; this function will advance
// the pointer past the immediate
inline int32 decodeVariableSizeImm(const unsigned char** immPtr) {
  const unsigned char small = **immPtr;
  if (UNLIKELY(small & 0x1)) {
    const unsigned int large = *((const unsigned int*)*immPtr);
    *immPtr += sizeof(large);
    return (int32)(large >> 1);
  } else {
    *immPtr += sizeof(small);
    return (int32)(small >> 1);
  }
}

int64 decodeMemberCodeImm(const unsigned char** immPtr, MemberCode mcode);

// Encodes a variable sized immediate for `val' into `buf'.  Returns
// the number of bytes used taken.  At most 4 bytes can be used.
size_t encodeVariableSizeImm(int32_t val, unsigned char* buf);

// Encodes a variable sized immediate to the end of vec.
void encodeIvaToVector(std::vector<uchar>& vec, int32_t val);

template<typename T>
void encodeToVector(std::vector<uchar>& vec, T val) {
  size_t currentLen = vec.size();
  vec.resize(currentLen + sizeof(T));
  memcpy(&vec[currentLen], &val, sizeof(T));
}

void staticStreamer(TypedValue* tv, std::stringstream& out);

std::string instrToString(const Opcode* it, const Unit* u = NULL);
std::string opcodeToName(Opcode op);

// returns a pointer to the location within the bytecode containing the jump
//   Offset, or NULL if the instruction cannot jump. Note that this offset is
//   relative to the current instruction.
Offset* instrJumpOffset(Opcode* instr);

// returns absolute address of target, or InvalidAbsoluteOffset if instruction
//   cannot jump
Offset instrJumpTarget(const Opcode* instrs, Offset pos);

struct StackTransInfo {
  enum Kind {
    PushPop,
    InsertMid
  };
  Kind kind;
  int numPops;
  int numPushes;
  int pos;
};

bool isValidOpcode(Opcode opcode);
bool instrIsControlFlow(Opcode opcode);
bool instrReadsCurrentFpi(Opcode opcode);

inline bool isFPush(Opcode opcode) {
  return opcode >= OpFPushFunc && opcode <= OpFPushContFunc;
}

int instrNumPops(const Opcode* opcode);
int instrNumPushes(const Opcode* opcode);
StackTransInfo instrStackTransInfo(const Opcode* opcode);
int instrSpToArDelta(const Opcode* opcode);

inline bool
mcodeMaybePropName(MemberCode mcode) {
  return mcode == MPC || mcode == MPL || mcode == MPT;
}

inline bool
mcodeMaybeArrayKey(MemberCode mcode) {
  return mcode == MEC || mcode == MEL || mcode == MET || mcode == MEI;
}

inline bool
mcodeMaybeArrayStringKey(MemberCode mcode) {
  return mcode == MEC || mcode == MEL || mcode == MET;
}

inline bool
mcodeMaybeArrayIntKey(MemberCode mcode) {
  return mcode == MEC || mcode == MEL || mcode == MEI;
}


} }

#endif
