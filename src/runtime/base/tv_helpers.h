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

#ifndef __INSIDE_HPHP_COMPLEX_TYPES_H__
#error Directly including 'tv_helpers.h' is prohibited. \
       Include 'complex_types.h' instead.
#endif

#ifndef __HPHP_TV_HELPERS_H__
#define __HPHP_TV_HELPERS_H__

#include <runtime/base/types.h>
#include <runtime/base/tv_macros.h>

namespace HPHP {
namespace VM {
class Class;
class Stack;
void tv_release_generic(TypedValue* tv);
void tv_release_typed(void*, DataType dt);
}
}

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

// Assumes 'data' is live
// Assumes 'IS_REFCOUNTED_TYPE(type)'
void tvDecRefHelper(DataType type, uint64_t datum);

inline bool tvIsPlausibleType(DataType type) {
  return type >= MinDataType && type < MaxNumDataTypes;
}

inline bool tvIsPlausible(const TypedValue* tv) {
  return tvIsPlausibleType(tv->m_type);
}

// Assumes 'tv' is live
inline void tvRefcountedDecRefCell(TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  if (IS_REFCOUNTED_TYPE(tv->m_type)) {
    tvDecRefHelper(tv->m_type, tv->m_data.num);
  }
}

inline void tvDecRefStr(TypedValue* tv) {
  ASSERT(tv->m_type == KindOfString);
  if (tv->m_data.pstr->decRefCount() == 0) {
    tv->m_data.pstr->release();
  }
}

inline void tvDecRefArr(TypedValue* tv) {
  ASSERT(tv->m_type == KindOfArray);
  if (tv->m_data.parr->decRefCount() == 0) {
    tv->m_data.parr->release();
  }
}

inline void tvDecRefObj(TypedValue* tv) {
  ASSERT(tv->m_type == KindOfObject);
  if (tv->m_data.pobj->decRefCount() == 0) {
    tv->m_data.pobj->release();
  }
}

// Assumes 'r' is live and points to a RefData
inline void tvDecRefRefInternal(RefData* r) {
  ASSERT(tvIsPlausible(r->tv()));
  ASSERT(r->tv()->m_type != KindOfRef);
  ASSERT(r->_count > 0);
  if (r->decRefCount() == 0) {
    r->release();
  }
}

// Assumes 'tv' is live
inline void tvDecRefRef(TypedValue* tv) {
  ASSERT(tv->m_type == KindOfRef);
  tvDecRefRefInternal(tv->m_data.pref);
}

inline void tvReleaseHelper(DataType type, uint64_t datum) {
  VM::tv_release_typed((void*)datum, type);
}

// Assumes 'tv' is live
inline void tvRefcountedDecRefHelper(DataType type, uint64_t datum) {
  if (IS_REFCOUNTED_TYPE(type)) {
    tvDecRefHelper(type, datum);
  }
}

// Assumes 'tv' is live
// Assumes 'IS_REFCOUNTED_TYPE(tv->m_type)'
inline void tvDecRef(TypedValue* tv) {
  tvDecRefHelper(tv->m_type, tv->m_data.num);
}

// Assumes 'tv' is live
inline void tvRefcountedDecRef(TypedValue* tv) {
  if (IS_REFCOUNTED_TYPE(tv->m_type)) {
    tvDecRef(tv);
  }
}

// tvBoxHelper sets the refcount of the newly allocated inner cell to 1
inline TypedValue* tvBoxHelper(DataType type, uint64_t datum) {
  return (NEW(RefData)(type, datum))->tv();
}

// Assumes 'tv' is live
inline void tvBox(TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  ASSERT(tv->m_type != KindOfRef);
  tv->m_data.ptv = tvBoxHelper(tv->m_type, tv->m_data.num);
  tv->m_type = KindOfRef;
}

// Assumes 'tv' is live
//
// Assumes 'IS_REFCOUNTED_TYPE(tv->m_type)'
inline void tvIncRef(TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  TV_INCREF(tv);
}

inline void tvRefcountedIncRef(TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  if (IS_REFCOUNTED_TYPE(tv->m_type)) {
    tvIncRef(tv);
  }
}

// Assumes 'tv' is live
// Assumes 'tv.m_type == KindOfRef'
inline void tvUnbox(TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  TV_UNBOX(tv);
  ASSERT(tvIsPlausible(tv));
}

// Assumes 'fr' is live and 'to' is dead
inline void tvReadCell(const TypedValue* fr, TypedValue* to) {
  ASSERT(tvIsPlausible(fr));
  TV_READ_CELL(fr, to);
}

// Assumes 'fr' is live and 'to' is dead
// Assumes 'fr->m_type != KindOfRef'
// NOTE: this helper will initialize to->_count to 0
inline void tvDupCell(const TypedValue* fr, TypedValue* to) {
  ASSERT(tvIsPlausible(fr));
  TV_DUP_CELL(fr, to);
}

// Assumes 'fr' is live and 'to' is dead
// Assumes 'fr->m_type == KindOfRef'
// NOTE: this helper will initialize to->_count to 0
inline void tvDupVar(const TypedValue* fr, TypedValue* to) {
  ASSERT(tvIsPlausible(fr));
  TV_DUP_VAR(fr,to);
}

// Assumes 'fr' is live and 'to' is dead
// NOTE: this helper will initialize to->_count to 0
inline void tvDup(const TypedValue* fr, TypedValue* to) {
  ASSERT(tvIsPlausible(fr));
  TV_DUP(fr,to);
}

// Assumes 'tv' is dead
// NOTE: this helper will initialize tv->_count to 0
inline void tvWriteNull(TypedValue* tv) {
  TV_WRITE_NULL(tv);
}

// Assumes 'tv' is dead
// NOTE: this helper will initialize tv->_count to 0
inline void tvWriteUninit(TypedValue* tv) {
  TV_WRITE_UNINIT(tv);
}

// Assumes 'to' and 'fr' are live
// Assumes that 'fr->m_type != KindOfRef'
// If 'to->m_type == KindOfRef', this will perform the set
// operation on the inner cell (to->m_data.ptv)
inline void tvSet(const TypedValue* fr, TypedValue* to) {
  ASSERT(fr->m_type != KindOfRef);
  if (to->m_type == KindOfRef) {
    to = to->m_data.ptv;
  }
  DataType oldType = to->m_type;
  uint64_t oldDatum = to->m_data.num;
  TV_DUP_CELL_NC(fr, to);
  tvRefcountedDecRefHelper(oldType, oldDatum);
}

// Assumes 'to' and 'fr' are live
// Assumes that 'fr->m_type == KindOfRef'
inline void tvBind(TypedValue * fr, TypedValue * to) {
  ASSERT(fr->m_type == KindOfRef);
  DataType oldType = to->m_type;
  uint64_t oldDatum = to->m_data.num;
  TV_DUP_VAR_NC(fr, to);
  tvRefcountedDecRefHelper(oldType, oldDatum);
}

// Assumes 'to' is live
inline void tvUnset(TypedValue * to) {
  tvRefcountedDecRef(to);
  TV_WRITE_UNINIT(to);
}

// Assumes `fr' is dead and binds it using KindOfIndirect to `to'.
inline void tvBindIndirect(TypedValue* fr, TypedValue* to) {
  ASSERT(tvIsPlausible(to));
  fr->m_type = KindOfIndirect;
  fr->m_data.ptv = to;
}

// If a TypedValue is KindOfIndirect, dereference to the inner
// TypedValue.
inline TypedValue* tvDerefIndirect(TypedValue* tv) {
  return tv->m_type == KindOfIndirect ? tv->m_data.ptv : tv;
}
inline const TypedValue* tvDerefIndirect(const TypedValue* tv) {
  return tvDerefIndirect(const_cast<TypedValue*>(tv));
}

/*
 * Returns true if this tv is not a ref-counted type, or if it is a
 * ref-counted type and the object pointed to is static.
 */
inline bool tvIsStatic(const TypedValue* tv) {
  ASSERT(tvIsPlausible(tv));
  return !IS_REFCOUNTED_TYPE(tv->m_type) ||
    tv->m_data.ptv->_count == RefCountStaticValue;
}

/**
 * tvAsVariant and tvAsCVarRef serve as escape hatches that allow us to call
 * into the Variant machinery. Ideally we will use these as little as possible
 * in the long term.
 */

// Assumes 'tv' is live
inline Variant& tvAsVariant(TypedValue* tv) {
  // Avoid treating uninitialized TV's as variants. We have some slightly
  // perverse, but defensible uses where we pass in NULL (and later check
  // a Variant* against NULL) so tolerate it.
  ASSERT(NULL == tv || tvIsPlausible(tv));
  return *(Variant*)(tv);
}

inline Variant& tvAsUninitializedVariant(TypedValue* tv) {
  // A special case, for use when constructing a variant and we don't
  // assume initialization.
  return *(Variant*)(tv);
}

// Assumes 'tv' is live
inline const Variant& tvAsCVarRef(const TypedValue* tv) {
  return *(const Variant*)(tv);
}

// Assumes 'tv' is live
inline Variant& tvCellAsVariant(TypedValue* tv) {
  ASSERT(tv->m_type != KindOfRef);
  return *(Variant*)(tv);
}

// Assumes 'tv' is live
inline const Variant& tvCellAsCVarRef(const TypedValue* tv) {
  ASSERT(tv->m_type != KindOfRef);
  return *(const Variant*)(tv);
}

// Assumes 'tv' is live
inline Variant& tvVarAsVariant(TypedValue* tv) {
  ASSERT(tv->m_type == KindOfRef);
  return *(Variant*)(tv);
}

// Assumes 'tv' is live
inline const Variant& tvVarAsCVarRef(const TypedValue* tv) {
  ASSERT(tv->m_type == KindOfRef);
  return *(const Variant*)(tv);
}

inline bool tvIsStronglyBound(const TypedValue* tv) {
  return (tv->m_type == KindOfRef && tv->m_data.ptv->_count > 1);
}

inline bool tvSame(const TypedValue* tv1, const TypedValue* tv2) {
  if (tv1->m_type == KindOfUninit || tv2->m_type == KindOfUninit) {
    return tv1->m_type == tv2->m_type;
  }
  return tvAsCVarRef(tv1).same(tvAsCVarRef(tv2));
}

void tvCastToBooleanInPlace(TypedValue* tv);
void tvCastToInt64InPlace(TypedValue* tv, int base = 10);
void tvCastToDoubleInPlace(TypedValue* tv);
void tvCastToStringInPlace(TypedValue* tv);
void tvCastToArrayInPlace(TypedValue* tv);
void tvCastToObjectInPlace(TypedValue* tv);

///////////////////////////////////////////////////////////////////////////////
}

#endif // __HPHP_TV_HELPERS_H__
