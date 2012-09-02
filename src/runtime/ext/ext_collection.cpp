/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include <runtime/ext/ext_collection.h>
#include <runtime/ext/ext_array.h>
#include <runtime/ext/ext_math.h>
#include <runtime/ext/ext_intl.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

c_Vector::c_Vector(const ObjectStaticCallbacks *cb) :
    ExtObjectDataFlags<ObjectData::VectorAttrInit>(cb), m_data(NULL),
    m_size(0), m_capacity(0), m_versionNumber(0) {
}

c_Vector::~c_Vector() {
  int sz = m_size;
  for (int i = 0; i < sz; ++i) {
    tvRefcountedDecRef(&m_data[i]);
  }
  c_Vector::sweep();
}

void c_Vector::sweep() {
  if (m_data) {
    free(m_data);
    m_data = NULL;
  }
}
  
void c_Vector::t___construct() {
}

Variant c_Vector::t___destruct() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::__destruct);
  return null;
}

void c_Vector::grow() {
  if (m_capacity) {
    m_capacity += m_capacity;
  } else {
    m_capacity = 8;
  }
  m_data = (TypedValue*)realloc(m_data, m_capacity * sizeof(TypedValue));
}

void c_Vector::resize(int64 sz, TypedValue* val) {
  ASSERT(val && val->m_type != KindOfRef);
  ++m_versionNumber;
  ASSERT(sz >= 0);
  if (m_capacity < sz) {
    m_capacity = sz;
    m_data = (TypedValue*)realloc(m_data, m_capacity * sizeof(TypedValue));
  }
  for (int64 i = m_size-1; i >= sz; --i) {
    tvRefcountedDecRef(&m_data[i]);
  }
  for (int64 i = m_size; i < sz; ++i) {
    tvDup(val, &m_data[i]);
  }
  m_size = sz;
}

bool c_Vector::contains(int64 key) {
  return ((unsigned long long)key < (unsigned long long)m_size);
}

ObjectData* c_Vector::clone() {
  ObjectData* obj = ObjectData::clone();
  c_Vector* vec = static_cast<c_Vector*>(obj);
  int sz = m_size;
  TypedValue* data;
  vec->m_capacity = vec->m_size = sz;
  vec->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));
  for (int i = 0; i < sz; ++i) {
    tvDup(&m_data[i], &data[i]);
  }
  return obj;
}

Object c_Vector::t_add(CVarRef val) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::add);
  TypedValue* tv = (TypedValue*)(&val);
  if (UNLIKELY(tv->m_type == KindOfRef)) {
    tv = tv->m_data.ptv;
  }
  add(tv);
  return this;
}

Object c_Vector::t_append(CVarRef val) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::append);
  TypedValue* tv = (TypedValue*)(&val);
  if (UNLIKELY(tv->m_type == KindOfRef)) {
    tv = tv->m_data.ptv;
  }
  add(tv);
  return this;
}

void c_Vector::t_pop() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::pop);
  ++m_versionNumber;
  if (m_size) {
    --m_size;
    tvDecRef(&m_data[m_size]);
  } else {
    Object e(SystemLib::AllocRuntimeExceptionObject(
      "Cannot pop empty Vector"));
    throw e;
  }
}

void c_Vector::t_resize(CVarRef sz, CVarRef value) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::resize);
  if (!sz.isInteger()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter sz must be a non-negative integer"));
    throw e;
  }
  int64 intSz = sz.toInt64();
  if (intSz < 0) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter sz must be a non-negative integer"));
    throw e;
  }
  TypedValue* val = (TypedValue*)(&value);
  if (UNLIKELY(val->m_type == KindOfRef)) {
    val = val->m_data.ptv;
  }
  resize(intSz, val);
}

Object c_Vector::t_clear() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::clear);
  ++m_versionNumber;
  int sz = m_size;
  for (int i = 0; i < sz; ++i) {
    tvRefcountedDecRef(&m_data[i]);
  }
  free(m_data);
  m_data = NULL;
  m_size = 0;
  m_capacity = 0;
  return this;
}

bool c_Vector::t_isempty() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::isempty);
  return (m_size == 0);
}
  
int64 c_Vector::t_count() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::count);
  return m_size;
}

Variant c_Vector::t_at(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::at);
  if (key.isInteger()) {
    return at(key.toInt64());
  }
  throwBadKeyType();
  return null;
}

Variant c_Vector::t_get(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::get);
  if (key.isInteger()) {
    return get(key.toInt64());
  }
  throwBadKeyType();
  return null;
}

bool c_Vector::t_contains(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::contains);
  if (!key.isInteger()) {
    throwBadKeyType();
  }
  return contains(key.toInt64());
}

Array c_Vector::t_toarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::toarray);
  ArrayInit ai(m_size, ArrayInit::vectorInit);
  int sz = m_size;
  for (int i = 0; i < sz; ++i) {
    ai.set(tvAsCVarRef(&m_data[i]));
  }
  return ai.create();
}

void c_Vector::t_sort(CVarRef col /* = null */) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::sort);
  // Terribly inefficient, but produces correct results for now
  Variant arr = t_toarray();
  if (col.isNull()) {
    f_sort(ref(arr));
  } else {
    if (!col.isObject()) {
      Object e(SystemLib::AllocInvalidArgumentExceptionObject(
        "Expected col to be an instance of Collator"));
      throw e;
    }
    ObjectData* obj = col.getObjectData();
    if (!obj->o_instanceof("Collator")) {
      Object e(SystemLib::AllocInvalidArgumentExceptionObject(
        "Expected col to be an instance of Collator"));
      throw e;
    }
    c_Collator* collator = static_cast<c_Collator*>(obj);
    // TODO Task #1429976: What do we do if the Collator encountered errors
    // while sorting? How is this reported to the user?
    collator->t_sort(ref(arr));
  }
  if (!arr.isArray()) {
    ASSERT(false);
    return;
  }
  ArrayData* ad = arr.getArrayData();
  int sz = ad->size();
  ssize_t pos = ad->iter_begin(); 
  for (int i = 0; i < sz; ++i, pos = ad->iter_advance(pos)) {
    ASSERT(pos != ArrayData::invalid_index);
    tvAsVariant(&m_data[i]) = ad->getValue(pos);
  } 
}

void c_Vector::t_reverse() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::reverse);
  TypedValue* start = m_data;
  TypedValue* end = m_data + m_size - 1;
  for (; start < end; ++start, --end) {
    std::swap(start->m_data.num, end->m_data.num);
    std::swap(start->m_type, end->m_type);
  }
}

void c_Vector::t_splice(CVarRef offset, CVarRef len /* = null */,
                        CVarRef replacement /* = null */) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::splice);
  if (!offset.isInteger()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter offset must be an integer"));
    throw e;
  }
  if (!len.isNull() && !len.isInteger()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter len must be null or an integer"));
    throw e;
  }
  if (!replacement.isNull()) {
    Object e(SystemLib::AllocRuntimeExceptionObject(
      "Vector::splice does not support replacement parameter"));
    throw e;
  }
  int64 startPos = offset.toInt64();
  if (UNLIKELY(uint64_t(startPos) >= uint64_t(m_size))) {
    if (startPos >= 0) {
      return;
    }
    startPos += m_size;
    if (startPos < 0) {
      startPos = 0;
    }
  }
  int64 endPos;
  if (len.isInteger()) {
    int64 intLen = len.toInt64();
    if (LIKELY(intLen > 0)) {
      endPos = startPos + intLen;
      if (endPos > m_size) {
        endPos = m_size;
      }
    } else {
      if (intLen == 0) {
        return;
      }
      endPos = m_size + intLen;
      if (endPos <= startPos) {
        return;
      }
    }
  } else {
    endPos = m_size;
  }
  // Null out each element before decreffing it. We need to do this in case
  // a __destruct method reenters and accesses this Vector object.
  for (int64 i = startPos; i < endPos; ++i) {
    uint64_t datum = m_data[i].m_data.num;
    DataType t = m_data[i].m_type;
    TV_WRITE_NULL(&m_data[i]);
    tvRefcountedDecRefHelper(t, datum);
  }
  // Move elements that came after the deleted elements (if there are any)
  if (endPos < m_size) {
    memmove(&m_data[startPos], &m_data[endPos],
            (m_size-endPos) * sizeof(TypedValue));
  }
  m_size -= (endPos - startPos);
}

int64 c_Vector::t_linearsearch(CVarRef search_value) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::linearsearch);
  int sz = m_size;
  for (int i = 0; i < sz; ++i) {
    if (search_value.same(tvAsCVarRef(&m_data[i]))) {
      return i;
    }
  }
  return -1;
}

void c_Vector::t_shuffle() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::shuffle);
  for (int64 i = 1; i < m_size; ++i) {
    int64 j = f_mt_rand(0, i);
    std::swap(m_data[i], m_data[j]);
  }
}

Object c_Vector::t_getiterator() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::getiterator);
  c_VectorIterator* it = NEWOBJ(c_VectorIterator)();
  it->m_obj = this;
  it->m_pos = 0;
  it->m_versionNumber = getVersionNumber();
  return it;
}

Object c_Vector::t_put(CVarRef key, CVarRef value) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::put);
  if (key.isInteger()) {
    TypedValue* tv = (TypedValue*)(&value);
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    put(key.toInt64(), tv);
    return this;
  }
  throwBadKeyType();
  return this;
}

String c_Vector::t___tostring() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Vector, Vector::__tostring);
  return "Vector";
}

Variant c_Vector::ti_fromarray(const char* cls, CVarRef arr) {
  STATIC_METHOD_INJECTION_BUILTIN(Vector, Vector::fromarray);
  if (!arr.isArray()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter arr must be an array"));
    throw e;
  }
  c_Vector* vec;
  Object ret = vec = NEWOBJ(c_Vector)();
  ArrayData* ad = arr.getArrayData();
  int sz = ad->size();
  vec->m_capacity = vec->m_size = sz;
  TypedValue* data;
  vec->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));
  ssize_t pos = ad->iter_begin(); 
  for (int i = 0; i < sz; ++i, pos = ad->iter_advance(pos)) {
    ASSERT(pos != ArrayData::invalid_index);
    TypedValue* tv = (TypedValue*)(&ad->getValueRef(pos));
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    tvRefcountedIncRef(tv);
    data[i].m_data.num = tv->m_data.num;
    data[i].m_type = tv->m_type;
  }
  return ret;
}

Variant c_Vector::ti_fromvector(const char* cls, CVarRef vec) {
  STATIC_METHOD_INJECTION_BUILTIN(Vector, Vector::fromvector);
  if (!vec.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "vec must be an instance of Vector"));
    throw e;
  }
  ObjectData* obj = vec.getObjectData();
  if (!obj->o_instanceof("Vector")) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "vec must be an instance of Vector"));
    throw e;
  }
  c_Vector* v = static_cast<c_Vector*>(obj); 
  c_Vector* target;
  Object ret = target = NEWOBJ(c_Vector)();
  int sz = v->m_size;
  TypedValue* data;
  target->m_capacity = target->m_size = sz;
  target->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));
  for (int i = 0; i < sz; ++i) {
    tvDup(&v->m_data[i], &data[i]);
  }
  return ret;
}

Variant c_Vector::ti_slice(const char* cls, CVarRef vec, CVarRef offset,
                           CVarRef len /* = null */) {
  STATIC_METHOD_INJECTION_BUILTIN(Vector, Vector::slice);
  if (!vec.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "vec must be an instance of Vector"));
    throw e;
  }
  ObjectData* obj = vec.getObjectData();
  if (!obj->o_instanceof("Vector")) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "vec must be an instance of Vector"));
    throw e;
  }
  if (!offset.isInteger()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter offset must be an integer"));
    throw e;
  }
  if (!len.isNull() && !len.isInteger()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter len must be null or an integer"));
    throw e;
  }
  c_Vector* target;
  Object ret = target = NEWOBJ(c_Vector)();
  c_Vector* v = static_cast<c_Vector*>(obj); 
  int64 startPos = offset.toInt64();
  if (UNLIKELY(uint64_t(startPos) >= uint64_t(v->m_size))) {
    if (startPos >= 0) {
      return ret;
    }
    startPos += v->m_size;
    if (startPos < 0) {
      startPos = 0;
    }
  }
  int64 endPos;
  if (len.isInteger()) {
    int64 intLen = len.toInt64();
    if (LIKELY(intLen > 0)) {
      endPos = startPos + intLen;
      if (endPos > v->m_size) {
        endPos = v->m_size;
      }
    } else {
      if (intLen == 0) {
        return ret;
      }
      endPos = v->m_size + intLen;
      if (endPos <= startPos) {
        return ret;
      }
    }
  } else {
    endPos = v->m_size;
  }
  ASSERT(startPos < endPos);
  int64 sz = endPos - startPos;
  TypedValue* data;
  target->m_capacity = target->m_size = sz;
  target->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));
  for (int64 i = 0; i < sz; ++i, ++startPos) {
    tvDup(&v->m_data[startPos], &data[i]);
  }
  return ret;
}

void c_Vector::throwBadKeyType() {
  Object e(SystemLib::AllocInvalidArgumentExceptionObject(
    "Only integer keys may be used with Vectors"));
  throw e;
}

TypedValue* c_Vector::OffsetGet(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Vector* vec = static_cast<c_Vector*>(obj);
  if (key->m_type == KindOfInt64) {
    return vec->at(key->m_data.num);
  }
  throwBadKeyType();
  return NULL;
}

void c_Vector::OffsetSet(ObjectData* obj, TypedValue* key, TypedValue* val) {
  ASSERT(key->m_type != KindOfRef);
  ASSERT(val->m_type != KindOfRef);
  c_Vector* vec = static_cast<c_Vector*>(obj);
  if (key->m_type == KindOfInt64) {
    vec->put(key->m_data.num, val);
    return;
  }
  throwBadKeyType();
}

bool c_Vector::OffsetIsset(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Vector* vec = static_cast<c_Vector*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = vec->get(key->m_data.num);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? isset(tvAsCVarRef(result)) : false;
}

bool c_Vector::OffsetEmpty(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Vector* vec = static_cast<c_Vector*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = vec->get(key->m_data.num);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? empty(tvAsCVarRef(result)) : true;
}

void c_Vector::OffsetAppend(ObjectData* obj, TypedValue* val) {
  ASSERT(val->m_type != KindOfRef);
  c_Vector* vec = static_cast<c_Vector*>(obj);
  vec->add(val);
}

void c_Vector::OffsetUnset(ObjectData* obj, TypedValue* key) {
  Object e(SystemLib::AllocRuntimeExceptionObject(
    "Vector does not support unset"));
  throw e;
}

c_VectorIterator::c_VectorIterator(const ObjectStaticCallbacks *cb) :
    ExtObjectData(cb) {
}

c_VectorIterator::~c_VectorIterator() {
}

void c_VectorIterator::t___construct() {
}

Variant c_VectorIterator::t_current() {
  INSTANCE_METHOD_INJECTION_BUILTIN(VectorIterator, VectorIterator::current);
  c_Vector* vec = m_obj.get();
  if (UNLIKELY(m_versionNumber != vec->getVersionNumber())) {
    throw_collection_modified();
    return null;
  }
  if (!vec->contains(m_pos)) {
    throw_iterator_not_valid();
    return null;
  }
  return tvAsCVarRef(&vec->m_data[m_pos]);
}

Variant c_VectorIterator::t_key() {
  INSTANCE_METHOD_INJECTION_BUILTIN(VectorIterator, VectorIterator::key);
  c_Vector* vec = m_obj.get();
  if (!vec->contains(m_pos)) {
    throw_iterator_not_valid();
    return null;
  }
  return m_pos;
}

bool c_VectorIterator::t_valid() {
  INSTANCE_METHOD_INJECTION_BUILTIN(VectorIterator, VectorIterator::valid);
  ASSERT(m_pos >= 0);
  c_Vector* vec = m_obj.get();
  return vec && (m_pos < vec->m_size);
}

void c_VectorIterator::t_next() {
  INSTANCE_METHOD_INJECTION_BUILTIN(VectorIterator, VectorIterator::next);
  m_pos++;
}

void c_VectorIterator::t_rewind() {
  INSTANCE_METHOD_INJECTION_BUILTIN(VectorIterator, VectorIterator::rewind);
  m_pos = 0;
}

///////////////////////////////////////////////////////////////////////////////

static const char emptyMapSlot[sizeof(c_Map::Bucket)] = { 0 };

c_Map::c_Map(const ObjectStaticCallbacks *cb) :
    ExtObjectDataFlags<ObjectData::MapAttrInit>(cb), m_size(0), m_load(0),
    m_nLastSlot(0), m_versionNumber(0) {
  m_data = (Bucket*)emptyMapSlot;
}

c_Map::~c_Map() {
  deleteBuckets();
  sweep();
}

void c_Map::sweep() {
  if (m_data != (Bucket*)emptyMapSlot) {
    free(m_data);
  }
  m_data = (Bucket*)emptyMapSlot;
}

void c_Map::deleteBuckets() {
  if (!m_size) return;
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      tvRefcountedDecRef(&p.data);
      if (p.hasStrKey() && p.skey->decRefCount() == 0) {
        DELETE(StringData)(p.skey);
      }
    }
  }
}
  
void c_Map::t___construct() {
}

Variant c_Map::t___destruct() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::__destruct);
  return null;
}

ObjectData* c_Map::clone() {
  ObjectData* obj = ObjectData::clone();
  c_Map* target = static_cast<c_Map*>(obj);

  if (!m_size) return obj;

  ASSERT(m_nLastSlot != 0);
  target->m_size = m_size;
  target->m_load = m_load;
  target->m_nLastSlot = m_nLastSlot;
  target->m_data = (Bucket*)malloc(numSlots() * sizeof(Bucket));
  memcpy(target->m_data, m_data, numSlots() * sizeof(Bucket));

  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      if (IS_REFCOUNTED_TYPE(p.data.m_type)) {
        tvIncRef(&p.data);
      }
      if (p.hasStrKey()) {
        p.skey->incRefCount();
      }
    }
  }

  return obj;
}
  
Object c_Map::t_clear() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::clear);
  deleteBuckets();
  sweep();
  m_size = 0;
  m_load = 0;
  m_nLastSlot = 0;
  m_data = (Bucket*)emptyMapSlot;
  return this;
}

bool c_Map::t_isempty() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::isempty);
  return (m_size == 0);
}

int64 c_Map::t_count() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::count);
  return m_size;
}

Variant c_Map::t_at(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::at);
  if (key.isInteger()) {
    return at(key.toInt64());
  } else if (key.isString()) {
    return at(key.getStringData());
  }
  throwBadKeyType();
  return null;
}

Variant c_Map::t_get(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::get);
  if (key.isInteger()) {
    return get(key.toInt64());
  } else if (key.isString()) {
    return get(key.getStringData());
  }
  throwBadKeyType();
  return null;
}

Object c_Map::t_put(CVarRef key, CVarRef value) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::put);
  TypedValue* val = (TypedValue*)(&value);
  if (UNLIKELY(val->m_type == KindOfRef)) {
    val = val->m_data.ptv;
  }
  if (key.isInteger()) {
    update(key.toInt64(), val);
  } else if (key.isString()) {
    update(key.getStringData(), val);
  } else {
    throwBadKeyType();
  }
  return this;
}

bool c_Map::t_contains(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::contains);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    Bucket* p = find(key.toInt64());
    return (p != NULL);
  }
  if (IS_STRING_TYPE(t)) {
    StringData* sd = key.getStringData();
    Bucket* p = find(sd->data(), sd->size(), sd->hash());
    return (p != NULL);
  }
  return false;
}

Object c_Map::t_remove(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::remove);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    remove(key.toInt64());
  } else if (IS_STRING_TYPE(t)) {
    remove(key.getStringData());
  }
  return this;
}

Object c_Map::t_discard(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::discard);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    remove(key.toInt64());
  } else if (IS_STRING_TYPE(t)) {
    remove(key.getStringData());
  }
  return this;
}

Array c_Map::t_toarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::toarray);
  ArrayInit ai(m_size);
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      if (p.hasIntKey()) {
        ai.set((int64)p.ikey, tvAsCVarRef(&p.data));
      } else {
        ai.set(*(const String*)(&p.skey), tvAsCVarRef(&p.data));
      }
    }
  }
  return ai.create();
}

Array c_Map::t_copyasarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::copyasarray);
  return t_toarray();
}

Array c_Map::t_tokeysarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::tokeysarray);
  ArrayInit ai(m_size, ArrayInit::vectorInit);
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      if (p.hasIntKey()) {
        ai.set((int64)p.ikey);
      } else {
        ai.set(*(const String*)(&p.skey));
      }
    }
  }
  return ai.create();
}

Object c_Map::t_values() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::values);
  c_Vector* target;
  Object ret = target = NEWOBJ(c_Vector)();
  int64 sz = m_size;
  if (!sz) {
    return ret;
  }
  TypedValue* data;
  target->m_capacity = target->m_size = sz;
  target->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));

  int64 j = 0;
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      TypedValue* tv = &p.data;
      tvRefcountedIncRef(tv);
      data[j].m_data.num = tv->m_data.num;
      data[j].m_type = tv->m_type;
      ++j;
    }
  }
  return ret;
}

Array c_Map::t_tovaluesarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::tovaluesarray);
  ArrayInit ai(m_size, ArrayInit::vectorInit);
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket& p = m_data[i];
    if (p.validValue()) {
      ai.set(tvAsCVarRef(&p.data));
    }
  }
  return ai.create();
}

Object c_Map::t_updatefromarray(CVarRef arr) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::updatefromarray);
  if (!arr.isArray()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Expected arr to be an array"));
    throw e;
  }
  ArrayData* ad = arr.getArrayData();
  for (ssize_t pos = ad->iter_begin(); pos != ArrayData::invalid_index;
       pos = ad->iter_advance(pos)) {
    Variant k = ad->getKey(pos);
    TypedValue* tv = (TypedValue*)(&ad->getValueRef(pos));
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    if (k.isInteger()) {
      update(k.toInt64(), (TypedValue*)(&ad->getValueRef(pos)));
    } else {
      ASSERT(k.isString());
      update(k.getStringData(), (TypedValue*)(&ad->getValueRef(pos)));
    }
  }
  return this;
}

Object c_Map::t_updatefromiterable(CVarRef it) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::updatefromiterable);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  if (obj->getCollectionType() == Collection::MapType) {
    c_Map* mp = static_cast<c_Map*>(obj);
    for (uint i = 0; i <= mp->m_nLastSlot; ++i) {
      c_Map::Bucket& p = mp->m_data[i];
      if (p.validValue()) {
        if (p.hasIntKey()) {
          update((int64)p.ikey, &p.data);
        } else {
          update(p.skey, &p.data);
        }
      }
    }
    return this;
  }
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    Variant v = iter.second();
    TypedValue* tv = (TypedValue*)(&v);
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    if (k.isInteger()) {
      update(k.toInt64(), tv);
    } else {
      ASSERT(k.isString());
      update(k.getStringData(), tv);
    }
  }
  return this;
}

Object c_Map::t_differencebykey(CVarRef it) {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::differencebykey);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  c_Map* target;
  Object ret = target = static_cast<c_Map*>(clone());
  if (obj->getCollectionType() == Collection::MapType) {
    c_Map* mp = static_cast<c_Map*>(obj);
    for (uint i = 0; i <= mp->m_nLastSlot; ++i) {
      c_Map::Bucket& p = mp->m_data[i];
      if (p.validValue()) {
        if (p.hasIntKey()) {
          target->remove((int64)p.ikey);
        } else {
          target->remove(p.skey);
        }
      }
    }
    return ret;
  }
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    if (k.isInteger()) {
      target->remove(k.toInt64());
    } else {
      ASSERT(k.isString());
      target->remove(k.getStringData());
    }
  }
  return ret;
}

Object c_Map::t_getiterator() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::getiterator);
  c_MapIterator* it = NEWOBJ(c_MapIterator)();
  it->m_obj = this;
  it->m_pos = iter_begin();
  it->m_versionNumber = getVersionNumber();
  return it;
}

String c_Map::t___tostring() {
  INSTANCE_METHOD_INJECTION_BUILTIN(Map, Map::__tostring);
  return "Map";
}

Variant c_Map::ti_fromarray(const char* cls, CVarRef arr) {
  STATIC_METHOD_INJECTION_BUILTIN(Map, Map::fromarray);
  if (!arr.isArray()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter arr must be an array"));
    throw e;
  }
  c_Map* mp;
  Object ret = mp = NEWOBJ(c_Map)();
  ArrayData* ad = arr.getArrayData();
  for (ssize_t pos = ad->iter_begin(); pos != ArrayData::invalid_index;
       pos = ad->iter_advance(pos)) {
    Variant k = ad->getKey(pos);
    TypedValue* tv = (TypedValue*)(&ad->getValueRef(pos));
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    if (k.isInteger()) {
      mp->update(k.toInt64(), tv);
    } else {
      ASSERT(k.isString());
      mp->update(k.getStringData(), tv);
    }
  }
  return ret;
}

Variant c_Map::ti_fromiterable(const char* cls, CVarRef it) {
  STATIC_METHOD_INJECTION_BUILTIN(Map, Map::fromiterable);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  Object ret;
  if (obj->getCollectionType() == Collection::MapType) {
    ret = obj->clone();
    return ret;
  }
  c_Map* target;
  ret = target = NEWOBJ(c_Map)();
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    Variant v = iter.second();
    TypedValue* tv = (TypedValue*)(&v);
    if (UNLIKELY(tv->m_type == KindOfRef)) {
      tv = tv->m_data.ptv;
    }
    if (k.isInteger()) {
      target->update(k.toInt64(), tv);
    } else {
      ASSERT(k.isString());
      target->update(k.getStringData(), tv);
    }
  }
  return ret;
}

void c_Map::throwOOB() {
  Object e(SystemLib::AllocOutOfBoundsExceptionObject(
    "Attempted to subscript a non-key"));
  throw e;
}

bool hit_string_key(const c_Map::Bucket* p, const char* k,
                    int len, int32 hash) ALWAYS_INLINE;
bool hit_string_key(const c_Map::Bucket* p, const char* k,
                    int len, int32 hash) {
  if (p->hasIntKey()) return false;
  const char* data = p->skey->data();
  return data == k || (p->hash() == hash &&
                       p->skey->size() == len &&
                       memcmp(data, k, len) == 0);
}

c_Map::Bucket* c_Map::find(int64 h) const {
  Bucket* p = fetchBucket(h & m_nLastSlot);
  if (LIKELY(p->validValue() && p->hasIntKey() && p->ikey == h)) {
    return p;
  }
  if (LIKELY(p->empty())) {
    return NULL;
  }
  size_t probeIndex = h;
  for (size_t i = 1;; ++i) {
    ASSERT(i <= m_nLastSlot);
    probeIndex = (probeIndex + i) & m_nLastSlot;
    ASSERT(((size_t(h)+((i + i*i) >> 1)) & m_nLastSlot) == probeIndex);
    p = fetchBucket(probeIndex);
    if (p->validValue() && p->hasIntKey() && p->ikey == h) {
      return p;
    }
    if (p->empty()) {
      return NULL;
    }
  }
}

c_Map::Bucket* c_Map::find(const char* k, int len, int64 prehash) const {
  int32 hash = c_Map::Bucket::encodeHash(prehash);
  Bucket* p = fetchBucket(prehash & m_nLastSlot);
  if (LIKELY(p->validValue() && hit_string_key(p, k, len, hash))) {
    return p;
  }
  if (p->empty()) {
    return NULL;
  }
  size_t probeIndex = prehash;
  for (size_t i = 1;; ++i) {
    ASSERT(i <= m_nLastSlot);
    probeIndex = (probeIndex + i) & m_nLastSlot;
    ASSERT(((size_t(prehash)+((i + i*i) >> 1)) & m_nLastSlot) == probeIndex);
    p = fetchBucket(probeIndex);
    if (LIKELY(p->validValue() && hit_string_key(p, k, len, hash))) {
      return p;
    }
    if (p->empty()) {
      return NULL;
    }
  }
}

c_Map::Bucket* c_Map::findForInsert(int64 h) const {
  Bucket* p = fetchBucket(h & m_nLastSlot);
  if (LIKELY((p->validValue() && p->hasIntKey() && p->ikey == h) ||
             p->empty())) {
    return p;
  }
  Bucket* ts = NULL;
  size_t probeIndex = h;
  for (size_t i = 1;; ++i) {
    if (UNLIKELY(p->tombstone() && !ts)) {
      ts = p;
    }
    ASSERT(i <= m_nLastSlot);
    probeIndex = (probeIndex + i) & m_nLastSlot;
    ASSERT(((size_t(h)+((i + i*i) >> 1)) & m_nLastSlot) == probeIndex);
    p = fetchBucket(probeIndex);
    if (LIKELY(p->validValue() && p->hasIntKey() && p->ikey == h)) {
      return p;
    }
    if (LIKELY(p->empty())) {
      if (LIKELY(!ts)) {
        return p;
      }
      return ts;
    }
  }
}

c_Map::Bucket* c_Map::findForInsert(const char* k, int len,
                                    int64 prehash) const { 
  int32 hash = c_Map::Bucket::encodeHash(prehash);
  Bucket* p = fetchBucket(prehash & m_nLastSlot);
  if (LIKELY((p->validValue() && hit_string_key(p, k, len, hash)) ||
             p->empty())) {
    return p;
  }
  Bucket* ts = NULL;
  size_t probeIndex = prehash;
  for (size_t i = 1;; ++i) {
    if (UNLIKELY(p->tombstone() && !ts)) {
      ts = p;
    }
    ASSERT(i <= m_nLastSlot);
    probeIndex = (probeIndex + i) & m_nLastSlot;
    ASSERT(((size_t(prehash)+((i + i*i) >> 1)) & m_nLastSlot) == probeIndex);
    p = fetchBucket(probeIndex);
    if (LIKELY(p->validValue() && hit_string_key(p, k, len, hash))) {
      return p;
    }
    if (LIKELY(p->empty())) {
      if (LIKELY(!ts)) {
        return p;
      }
      return ts;
    }
  }
}

bool c_Map::update(int64 h, TypedValue* data) {
  ASSERT(data->m_type != KindOfRef);
  Bucket* p = findForInsert(h);
  ASSERT(p);
  if (p->validValue()) {
    tvRefcountedIncRef(data);
    tvRefcountedDecRef(&p->data);
    p->data.m_data.num = data->m_data.num;
    p->data.m_type = data->m_type;
    return true;
  }
  ++m_versionNumber;
  ++m_size;
  if (!p->tombstone()) {
    if (++m_load >= computeMaxLoad()) {
      resize();
      p = findForInsert(h);
      ASSERT(p);
    }
  }
  tvRefcountedIncRef(data);
  p->data.m_data.num = data->m_data.num;
  p->data.m_type = data->m_type;
  p->setIntKey(h);
  return true;
}

bool c_Map::update(StringData *key, TypedValue* data) {
  int64 h = key->hash();
  Bucket* p = findForInsert(key->data(), key->size(), h);
  ASSERT(p);
  if (p->validValue()) {
    tvRefcountedIncRef(data);
    tvRefcountedDecRef(&p->data);
    p->data.m_data.num = data->m_data.num;
    p->data.m_type = data->m_type;
    return true;
  }
  ++m_versionNumber;
  ++m_size;
  if (!p->tombstone()) {
    if (++m_load >= computeMaxLoad()) {
      resize();
      p = findForInsert(key->data(), key->size(), h);
      ASSERT(p);
    }
  }
  tvRefcountedIncRef(data);
  p->data.m_data.num = data->m_data.num;
  p->data.m_type = data->m_type;
  p->setStrKey(key, h);
  return true;
}

void c_Map::erase(Bucket* p) {
  if (p == NULL) {
    return;
  }
  if (p->validValue()) {
    m_size--;
    tvRefcountedDecRef(&p->data);
    if (p->hasStrKey() && p->skey->decRefCount() == 0) {
      DELETE(StringData)(p->skey);
    }
    p->data.m_type = (DataType)KindOfTombstone;
    if (m_size < computeMinElements() && m_size) {
      resize();
    }
  }
}

void c_Map::resize() {
  if (m_nLastSlot == 0) {
    ASSERT(m_data == (Bucket*)emptyMapSlot);
    m_nLastSlot = 3;
    m_data = (Bucket*)calloc(numSlots(), sizeof(Bucket));
    return;
  }
  uint oldNumSlots = numSlots();
  ASSERT(m_size > 0);
  m_nLastSlot = Util::roundUpToPowerOfTwo(m_size << 1) - 1;
  m_load = m_size;
  Bucket* oldBuckets = m_data;
  m_data = (Bucket*)calloc(numSlots(), sizeof(Bucket));
  for (uint i = 0; i < oldNumSlots; ++i) {
    Bucket* p = &oldBuckets[i];
    if (p->validValue()) {
      Bucket* np;
      if (p->hasIntKey()) {
        np = findForInsert((int64)p->ikey);
      } else {
        np = findForInsert(p->skey->data(), p->skey->size(), p->skey->hash());
      }
      memcpy(np, p, sizeof(Bucket));
    }
  }
  free(oldBuckets);
}

ssize_t c_Map::iter_begin() const {
  if (!m_size) return 0;
  for (uint i = 0; i <= m_nLastSlot; ++i) {
    Bucket* p = fetchBucket(i);
    if (p->validValue()) {
      return reinterpret_cast<ssize_t>(p);
    }
  }
  return 0;
}

ssize_t c_Map::iter_next(ssize_t pos) const {
  if (pos == 0) {
    return 0;
  }
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  Bucket* pLast = fetchBucket(m_nLastSlot);
  ++p;
  while (p <= pLast) {
    if (p->validValue()) {
      return reinterpret_cast<ssize_t>(p);
    }
    ++p;
  }
  return 0;
}

ssize_t c_Map::iter_prev(ssize_t pos) const {
  if (pos == 0) {
    return 0;
  }
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  Bucket* pStart = m_data;
  --p;
  while (p >= pStart) {
    if (p->validValue()) {
      return reinterpret_cast<ssize_t>(p);
    }
    --p;
  }
  return 0;
}

Variant c_Map::iter_key(ssize_t pos) const {
  ASSERT(pos);
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  if (p->hasStrKey()) {
    return p->skey;
  }
  return (int64)p->ikey;
}

Variant c_Map::iter_value(ssize_t pos) const {
  ASSERT(pos);
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  return tvAsCVarRef(&p->data);
}

void c_Map::throwBadKeyType() {
  Object e(SystemLib::AllocInvalidArgumentExceptionObject(
    "Only integer keys and string keys may be used with Maps"));
  throw e;
}

void c_Map::Bucket::dump() {
  if (!validValue()) {
    printf("c_Map::Bucket: %s\n", (empty() ? "empty" : "tombstone"));
    return;
  }
  printf("c_Map::Bucket: %llx\n", hashKey());
  if (hasStrKey()) {
    skey->dump();
  }
  tvAsCVarRef(&data).dump();
}

TypedValue* c_Map::OffsetGet(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Map* mp = static_cast<c_Map*>(obj);
  if (key->m_type == KindOfInt64) {
    return mp->at(key->m_data.num);
  }
  if (IS_STRING_TYPE(key->m_type)) {
    return mp->at(key->m_data.pstr);
  }
  throwBadKeyType();
  return NULL;
}

void c_Map::OffsetSet(ObjectData* obj, TypedValue* key, TypedValue* val) {
  ASSERT(key->m_type != KindOfRef);
  ASSERT(val->m_type != KindOfRef);
  c_Map* mp = static_cast<c_Map*>(obj);
  if (key->m_type == KindOfInt64) {
    mp->put(key->m_data.num, val);
    return;
  }
  if (IS_STRING_TYPE(key->m_type)) {
    mp->put(key->m_data.pstr, val);
    return;
  }
  throwBadKeyType();
}

bool c_Map::OffsetIsset(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Map* mp = static_cast<c_Map*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = mp->get(key->m_data.num);
  } else if (IS_STRING_TYPE(key->m_type)) {
    result = mp->get(key->m_data.pstr);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? isset(tvAsCVarRef(result)) : false;
}

bool c_Map::OffsetEmpty(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Map* mp = static_cast<c_Map*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = mp->get(key->m_data.num);
  } else if (IS_STRING_TYPE(key->m_type)) {
    result = mp->get(key->m_data.pstr);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? empty(tvAsCVarRef(result)) : true;
}

void c_Map::OffsetAppend(ObjectData* obj, TypedValue* val) {
  Object e(SystemLib::AllocRuntimeExceptionObject(
    "[] operator not supported for Maps"));
  throw e;
}

void c_Map::OffsetUnset(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_Map* mp = static_cast<c_Map*>(obj);
  if (key->m_type == KindOfInt64) {
    mp->remove(key->m_data.num);
    return;
  }
  if (IS_STRING_TYPE(key->m_type)) {
    mp->remove(key->m_data.pstr);
    return;
  }
  throwBadKeyType();
}

c_MapIterator::c_MapIterator(const ObjectStaticCallbacks *cb) :
    ExtObjectData(cb) {
}

c_MapIterator::~c_MapIterator() {
}

void c_MapIterator::t___construct() {
}

Variant c_MapIterator::t_current() {
  INSTANCE_METHOD_INJECTION_BUILTIN(MapIterator, MapIterator::current);
  c_Map* mp = m_obj.get();
  if (UNLIKELY(m_versionNumber != mp->getVersionNumber())) {
    throw_collection_modified();
    return null;
  }
  if (!m_pos) {
    throw_iterator_not_valid();
    return null;
  }
  return mp->iter_value(m_pos);
}

Variant c_MapIterator::t_key() {
  INSTANCE_METHOD_INJECTION_BUILTIN(MapIterator, MapIterator::key);
  c_Map* mp = m_obj.get();
  if (UNLIKELY(m_versionNumber != mp->getVersionNumber())) {
    throw_collection_modified();
    return null;
  }
  if (!m_pos) {
    throw_iterator_not_valid();
    return null;
  }
  return mp->iter_key(m_pos);
}

bool c_MapIterator::t_valid() {
  INSTANCE_METHOD_INJECTION_BUILTIN(MapIterator, MapIterator::valid);
  return m_pos != 0;
}

void c_MapIterator::t_next() {
  INSTANCE_METHOD_INJECTION_BUILTIN(MapIterator, MapIterator::next);
  c_Map* mp = m_obj.get();
  if (UNLIKELY(m_versionNumber != mp->getVersionNumber())) {
    throw_collection_modified();
    return;
  }
  m_pos = mp->iter_next(m_pos);
}

void c_MapIterator::t_rewind() {
  INSTANCE_METHOD_INJECTION_BUILTIN(MapIterator, MapIterator::rewind);
  c_Map* mp = m_obj.get();
  if (UNLIKELY(m_versionNumber != mp->getVersionNumber())) {
    throw_collection_modified();
    return;
  }
  m_pos = mp->iter_begin();
}

///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS_CLS(c_StableMap, Bucket);

#define CONNECT_TO_GLOBAL_DLLIST(element)                               \
do {                                                                    \
  (element)->pListLast = m_pListTail;                                   \
  m_pListTail = (element);                                              \
  (element)->pListNext = NULL;                                          \
  if ((element)->pListLast != NULL) {                                   \
    (element)->pListLast->pListNext = (element);                        \
  }                                                                     \
  if (!m_pListHead) {                                                   \
    m_pListHead = (element);                                            \
  }                                                                     \
} while (false)

static const char emptyStableMapSlot[sizeof(c_StableMap::Bucket*)] = { 0 };

c_StableMap::c_StableMap(const ObjectStaticCallbacks *cb) :
    ExtObjectDataFlags<ObjectData::StableMapAttrInit>(cb), m_versionNumber(0),
    m_pListHead(NULL), m_pListTail(NULL) {
  m_size = 0;
  m_nTableSize = 0;
  m_nTableMask = 0;
  m_arBuckets = (Bucket**)emptyStableMapSlot;
}

c_StableMap::~c_StableMap() {
  deleteBuckets();
  sweep();
}

void c_StableMap::sweep() {
  if (m_arBuckets != (Bucket**)emptyStableMapSlot) {
    free(m_arBuckets);
  }
  m_arBuckets = (Bucket**)emptyStableMapSlot;
}

void c_StableMap::deleteBuckets() {
  Bucket* p = m_pListHead;
  while (p) {
    Bucket* q = p;
    p = p->pListNext;
    DELETE(Bucket)(q);
  }
}
  
void c_StableMap::t___construct() {
}

Variant c_StableMap::t___destruct() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::__destruct);
  return null;
}

ObjectData* c_StableMap::clone() {
  ObjectData* obj = ObjectData::clone();
  c_StableMap* target = static_cast<c_StableMap*>(obj);

  if (!m_size) return obj;

  target->m_size = m_size;
  target->m_nTableSize = m_nTableSize;
  target->m_nTableMask = m_nTableMask;
  target->m_arBuckets = (Bucket**)calloc(m_nTableSize, sizeof(Bucket*));

  Bucket *last = NULL;
  for (Bucket *p = m_pListHead; p; p = p->pListNext) {
    Bucket *np = NEW(Bucket)(Variant::noInit);
    np->data.constructValHelper(p->data);
    uint nIndex;
    if (p->hasIntKey()) {
      np->setIntKey(p->ikey);
      nIndex = p->ikey & target->m_nTableMask;
    } else {
      np->setStrKey(p->skey, p->hash());
      nIndex = p->hash() & target->m_nTableMask;
    }

    np->pNext = target->m_arBuckets[nIndex];
    target->m_arBuckets[nIndex] = np;

    if (last) {
      last->pListNext = np;
      np->pListLast = last;
    } else {
      target->m_pListHead = np;
      np->pListLast = NULL;
    }
    last = np;
  }
  if (last) last->pListNext = NULL;
  target->m_pListTail = last;

  return obj;
}

Object c_StableMap::t_clear() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::clear);
  deleteBuckets();
  sweep();
  m_pListHead = NULL;
  m_pListTail = NULL;
  m_size = 0;
  m_nTableSize = 0;
  m_nTableMask = 0;
  m_arBuckets = (Bucket**)emptyStableMapSlot;
  return this;
}

bool c_StableMap::t_isempty() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::isempty);
  return (m_size == 0);
}

int64 c_StableMap::t_count() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::count);
  return m_size;
}

Variant c_StableMap::t_at(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::at);
  if (key.isInteger()) {
    return at(key.toInt64());
  } else if (key.isString()) {
    return at(key.getStringData());
  }
  throwBadKeyType();
  return null;
}

Variant c_StableMap::t_get(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::get);
  if (key.isInteger()) {
    return get(key.toInt64());
  } else if (key.isString()) {
    return get(key.getStringData());
  }
  throwBadKeyType();
  return null;
}

Object c_StableMap::t_put(CVarRef key, CVarRef value) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::put);
  if (key.isInteger()) {
    update(key.toInt64(), value);
  } else if (key.isString()) {
    update(key.getStringData(), value);
  } else {
    throwBadKeyType();
  }
  return this;
}

bool c_StableMap::t_contains(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::contains);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    Bucket* p = find(key.toInt64());
    return (p != NULL);
  }
  if (IS_STRING_TYPE(t)) {
    StringData* sd = key.getStringData();
    Bucket* p = find(sd->data(), sd->size(), sd->hash());
    return (p != NULL);
  }
  return false;
}

Object c_StableMap::t_remove(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::remove);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    remove(key.toInt64());
  } else if (IS_STRING_TYPE(t)) {
    remove(key.getStringData());
  }
  return this;
}

Object c_StableMap::t_discard(CVarRef key) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::discard);
  DataType t = key.getType();
  if (t == KindOfInt64) {
    remove(key.toInt64());
  } else if (IS_STRING_TYPE(t)) {
    remove(key.getStringData());
  }
  return this;
}

Array c_StableMap::t_toarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::toarray);
  ArrayInit ai(m_size);
  Bucket* p = m_pListHead;
  while (p) {
    if (p->hasIntKey()) {
      ai.set((int64)p->ikey, p->data);
    } else {
      ai.set(*(const String*)(&p->skey), p->data);
    }
    p = p->pListNext;
  }
  return ai.create();
}

Array c_StableMap::t_copyasarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::copyasarray);
  return t_toarray();
}

Array c_StableMap::t_tokeysarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::tokeysarray);
  ArrayInit ai(m_size, ArrayInit::vectorInit);
  Bucket* p = m_pListHead;
  while (p) {
    if (p->hasIntKey()) {
      ai.set(*(const String*)(&p->skey));
    } else {
      ai.set((int64)p->ikey);
    }
    p = p->pListNext;
  }
  return ai.create();
}

Object c_StableMap::t_values() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::values);
  c_Vector* target;
  Object ret = target = NEWOBJ(c_Vector)();
  int64 sz = m_size;
  if (!sz) {
    return ret;
  }
  TypedValue* data;
  target->m_capacity = target->m_size = sz;
  target->m_data = data = (TypedValue*)malloc(sz * sizeof(TypedValue));
  Bucket* p = m_pListHead;
  for (int64 i = 0; i < sz; ++i) {
    ASSERT(p);
    tvAsUninitializedVariant(&data[i]).constructValHelper(p->data);
    p = p->pListNext;
  }
  return ret;
}

Array c_StableMap::t_tovaluesarray() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::tovaluesarray);
  ArrayInit ai(m_size, ArrayInit::vectorInit);
  Bucket* p = m_pListHead;
  while (p) {
    ai.set(p->data);
    p = p->pListNext;
  }
  return ai.create();
}

Object c_StableMap::t_updatefromarray(CVarRef arr) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::updatefromarray);
  if (!arr.isArray()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter arr must be an array"));
    throw e;
  }
  ArrayData* ad = arr.getArrayData();
  for (ssize_t pos = ad->iter_begin(); pos != ArrayData::invalid_index;
       pos = ad->iter_advance(pos)) {
    Variant k = ad->getKey(pos);
    if (k.isInteger()) {
      update(k.toInt64(), ad->getValue(pos));
    } else {
      ASSERT(k.isString());
      update(k.getStringData(), ad->getValue(pos));
    }
  }
  return this;
}

Object c_StableMap::t_updatefromiterable(CVarRef it) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::updatefromiterable);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  if (obj->getCollectionType() == Collection::StableMapType) {
    c_StableMap* smp = static_cast<c_StableMap*>(obj);
    c_StableMap::Bucket* p = smp->m_pListHead;
    while (p) {
      if (p->hasIntKey()) {
        update((int64)p->ikey, p->data);
      } else {
        update(p->skey, p->data);
      }
      p = p->pListNext;
    }
    return this;
  }
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    if (k.isInteger()) {
      update(k.toInt64(), iter.second());
    } else {
      ASSERT(k.isString());
      update(k.getStringData(), iter.second());
    }
  }
  return this;
}

Object c_StableMap::t_differencebykey(CVarRef it) {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::differencebykey);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  c_StableMap* target;
  Object ret = target = static_cast<c_StableMap*>(clone());
  if (obj->getCollectionType() == Collection::StableMapType) {
    c_StableMap* smp = static_cast<c_StableMap*>(obj);
    c_StableMap::Bucket* p = smp->m_pListHead;
    while (p) {
      if (p->hasIntKey()) {
        target->remove((int64)p->ikey);
      } else {
        target->remove(p->skey);
      }
      p = p->pListNext;
    }
  }
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    if (k.isInteger()) {
      target->remove(k.toInt64());
    } else {
      ASSERT(k.isString());
      target->remove(k.getStringData());
    }
  }
  return ret;
}

Object c_StableMap::t_getiterator() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::getiterator);
  c_StableMapIterator* it = NEWOBJ(c_StableMapIterator)();
  it->m_obj = this;
  it->m_pos = iter_begin();
  it->m_versionNumber = getVersionNumber();
  return it;
}

String c_StableMap::t___tostring() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMap, StableMap::__tostring);
  return "StableMap";
}

Variant c_StableMap::ti_fromarray(const char* cls, CVarRef arr) {
  STATIC_METHOD_INJECTION_BUILTIN(StableMap, StableMap::fromarray);
  if (!arr.isArray()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter arr must be an array"));
    throw e;
  }
  c_StableMap* smp;
  Object ret = smp = NEWOBJ(c_StableMap)();
  ArrayData* ad = arr.getArrayData();
  for (ssize_t pos = ad->iter_begin(); pos != ArrayData::invalid_index;
       pos = ad->iter_advance(pos)) {
    Variant k = ad->getKey(pos);
    Variant v = ad->getValue(pos);
    if (k.isInteger()) {
      smp->update(k.toInt64(), v);
    } else {
      ASSERT(k.isString());
      smp->update(k.getStringData(), v);
    }
  }
  return ret;
}

Variant c_StableMap::ti_fromiterable(const char* cls, CVarRef it) {
  STATIC_METHOD_INJECTION_BUILTIN(StableMap, StableMap::fromiterable);
  if (!it.isObject()) {
    Object e(SystemLib::AllocInvalidArgumentExceptionObject(
      "Parameter it must be an instance of Iterable"));
    throw e;
  }
  ObjectData* obj = it.getObjectData();
  Object ret;
  if (obj->getCollectionType() == Collection::StableMapType) {
    ret = obj->clone();
    return ret;
  } 
  c_StableMap* target;
  ret = target = NEWOBJ(c_StableMap)();
  for (ArrayIter iter = obj->begin(); iter; ++iter) {
    Variant k = iter.first();
    if (k.isInteger()) {
      target->update(k.toInt64(), iter.second());
    } else {
      ASSERT(k.isString());
      target->update(k.getStringData(), iter.second());
    }
  }
  return ret;
}

bool lm_hit_string_key(const c_StableMap::Bucket* p,
                       const char* k, int len, int32 hash) 
                       ALWAYS_INLINE;
bool lm_hit_string_key(const c_StableMap::Bucket* p,
                       const char* k, int len, int32 hash) {
  if (p->hasIntKey()) return false;
  const char* data = p->skey->data();
  return data == k || p->hash() == hash
                      && p->skey->size() == len &&
                      memcmp(data, k, len) == 0;
}

c_StableMap::Bucket* c_StableMap::find(int64 h) const {
  for (Bucket* p = m_arBuckets[h & m_nTableMask]; p; p = p->pNext) {
    if (p->hasIntKey() && p->ikey == h) {
      return p;
    }
  }
  return NULL;
}

c_StableMap::Bucket* c_StableMap::find(const char* k, int len,
                                       int64 prehash) const {
  int32 hash = c_StableMap::Bucket::encodeHash(prehash);
  for (Bucket* p = m_arBuckets[prehash & m_nTableMask]; p; p = p->pNext) {
    if (lm_hit_string_key(p, k, len, hash)) return p;
  }
  return NULL;
}

c_StableMap::Bucket** c_StableMap::findForErase(int64 h) const {
  Bucket** ret = &(m_arBuckets[h & m_nTableMask]);
  Bucket* p = *ret;
  while (p) {
    if (p->hasIntKey() && p->ikey == h) {
      return ret;
    }
    ret = &(p->pNext);
    p = *ret;
  }
  return NULL;
}

c_StableMap::Bucket** c_StableMap::findForErase(const char* k, int len,
                                                int64 prehash) const {
  Bucket** ret = &(m_arBuckets[prehash & m_nTableMask]);
  Bucket* p = *ret;
  int32 hash = c_StableMap::Bucket::encodeHash(prehash);
  while (p) {
    if (lm_hit_string_key(p, k, len, hash)) return ret;
    ret = &(p->pNext);
    p = *ret;
  }
  return NULL;
}

bool c_StableMap::update(int64 h, CVarRef data) {
  Bucket* p = find(h);
  if (p) {
    p->data.assignValHelper(data);
    return true;
  }
  ++m_versionNumber;
  if (++m_size > m_nTableSize) {
    resize();
  }
  p = NEW(Bucket)(data);
  p->setIntKey(h);
  uint nIndex = (h & m_nTableMask);
  p->pNext = m_arBuckets[nIndex];
  m_arBuckets[nIndex] = p;
  CONNECT_TO_GLOBAL_DLLIST(p);
  return true;
}

bool c_StableMap::update(StringData *key, CVarRef data) {
  int64 h = key->hash();
  Bucket* p = find(key->data(), key->size(), h);
  if (p) {
    p->data.assignValHelper(data);
    return true;
  }
  ++m_versionNumber;
  if (++m_size > m_nTableSize) {
    resize();
  }
  p = NEW(Bucket)(data);
  p->setStrKey(key, h);
  uint nIndex = (h & m_nTableMask);
  p->pNext = m_arBuckets[nIndex];
  m_arBuckets[nIndex] = p;
  CONNECT_TO_GLOBAL_DLLIST(p);
  return true;
}

void c_StableMap::erase(Bucket** prev) {
  if (prev == NULL) {
    return;
  }
  Bucket* p = *prev;
  if (p) {
    *prev = p->pNext;
    if (p->pListLast) {
      p->pListLast->pListNext = p->pListNext;
    } else {
      /* Deleting the head of the list */
      ASSERT(m_pListHead == p);
      m_pListHead = p->pListNext;
    }
    if (p->pListNext) {
      p->pListNext->pListLast = p->pListLast;
    } else {
      ASSERT(m_pListTail == p);
      m_pListTail = p->pListLast;
    }
    m_size--;
    DELETE(Bucket)(p);
  }
}

void c_StableMap::resize() {
  if (m_nTableSize == 0) {
    m_nTableSize = 4;
    m_nTableMask = m_nTableSize - 1;
    m_arBuckets = (Bucket**)calloc(m_nTableSize, sizeof(Bucket*));
  }
  m_nTableSize <<= 1;
  m_nTableMask = m_nTableSize - 1;
  free(m_arBuckets);
  m_arBuckets = (Bucket**)calloc(m_nTableSize, sizeof(Bucket*));
  for (Bucket* p = m_pListHead; p; p = p->pListNext) {
    uint nIndex = (p->hashKey() & m_nTableMask);
    p->pNext = m_arBuckets[nIndex];
    m_arBuckets[nIndex] = p;
  }
}

ssize_t c_StableMap::iter_begin() const {
  Bucket* p = m_pListHead;
  return reinterpret_cast<ssize_t>(p);
}

ssize_t c_StableMap::iter_next(ssize_t pos) const {
  if (pos == 0) {
    return 0;
  }
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  p = p->pListNext;
  return reinterpret_cast<ssize_t>(p);
}

ssize_t c_StableMap::iter_prev(ssize_t pos) const {
  if (pos == 0) {
    return 0;
  }
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  p = p->pListLast;
  return reinterpret_cast<ssize_t>(p);
}

Variant c_StableMap::iter_key(ssize_t pos) const {
  ASSERT(pos);
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  if (p->hasStrKey()) {
    return p->skey;
  }
  return (int64)p->ikey;
}

Variant c_StableMap::iter_value(ssize_t pos) const {
  ASSERT(pos);
  Bucket* p = reinterpret_cast<Bucket*>(pos);
  return p->data;
}

void c_StableMap::throwBadKeyType() {
  Object e(SystemLib::AllocInvalidArgumentExceptionObject(
    "Only integer keys and string keys may be used with StableMaps"));
  throw e;
}

#undef CONNECT_TO_GLOBAL_DLLIST

c_StableMap::Bucket::~Bucket() {
  if (hasStrKey() && skey->decRefCount() == 0) {
    DELETE(StringData)(skey);
  }
}

void c_StableMap::Bucket::dump() {
  printf("c_StableMap::Bucket: %llx, %p, %p, %p\n",
         hashKey(), pListNext, pListLast, pNext);
  if (hasStrKey()) {
    skey->dump();
  }
  data.dump();
}

TypedValue* c_StableMap::OffsetGet(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_StableMap* smp = static_cast<c_StableMap*>(obj);
  if (key->m_type == KindOfInt64) {
    return smp->at(key->m_data.num);
  }
  if (IS_STRING_TYPE(key->m_type)) {
    return smp->at(key->m_data.pstr);
  }
  throwBadKeyType();
  return NULL;
}

void c_StableMap::OffsetSet(ObjectData* obj, TypedValue* key, TypedValue* val) {
  ASSERT(key->m_type != KindOfRef);
  ASSERT(val->m_type != KindOfRef);
  c_StableMap* smp = static_cast<c_StableMap*>(obj);
  if (key->m_type == KindOfInt64) {
    smp->put(key->m_data.num, val);
    return;
  }
  if (IS_STRING_TYPE(key->m_type)) {
    smp->put(key->m_data.pstr, val);
    return;
  }
  throwBadKeyType();
}

bool c_StableMap::OffsetIsset(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_StableMap* smp = static_cast<c_StableMap*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = smp->get(key->m_data.num);
  } else if (IS_STRING_TYPE(key->m_type)) {
    result = smp->get(key->m_data.pstr);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? isset(tvAsCVarRef(result)) : false;
}

bool c_StableMap::OffsetEmpty(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_StableMap* smp = static_cast<c_StableMap*>(obj);
  TypedValue* result;
  if (key->m_type == KindOfInt64) {
    result = smp->get(key->m_data.num);
  } else if (IS_STRING_TYPE(key->m_type)) {
    result = smp->get(key->m_data.pstr);
  } else {
    throwBadKeyType();
    result = NULL;
  }
  return result ? empty(tvAsCVarRef(result)) : true;
}

void c_StableMap::OffsetAppend(ObjectData* obj, TypedValue* val) {
  Object e(SystemLib::AllocRuntimeExceptionObject(
    "[] operator not supported for StableMaps"));
  throw e;
}

void c_StableMap::OffsetUnset(ObjectData* obj, TypedValue* key) {
  ASSERT(key->m_type != KindOfRef);
  c_StableMap* smp = static_cast<c_StableMap*>(obj);
  if (key->m_type == KindOfInt64) {
    smp->remove(key->m_data.num);
    return;
  }
  if (IS_STRING_TYPE(key->m_type)) {
    smp->remove(key->m_data.pstr);
    return;
  }
  throwBadKeyType();
}

c_StableMapIterator::c_StableMapIterator(const ObjectStaticCallbacks *cb) :
    ExtObjectData(cb) {
}

c_StableMapIterator::~c_StableMapIterator() {
}

void c_StableMapIterator::t___construct() {
}

Variant c_StableMapIterator::t_current() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMapIterator, StableMapIterator::current);
  c_StableMap* smp = m_obj.get();
  if (UNLIKELY(m_versionNumber != smp->getVersionNumber())) {
    throw_collection_modified();
    return null;
  }
  if (!m_pos) {
    throw_iterator_not_valid();
    return null;
  }
  return smp->iter_value(m_pos);
}

Variant c_StableMapIterator::t_key() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMapIterator, StableMapIterator::key);
  c_StableMap* smp = m_obj.get();
  if (UNLIKELY(m_versionNumber != smp->getVersionNumber())) {
    throw_collection_modified();
    return null;
  }
  if (!m_pos) {
    throw_iterator_not_valid();
    return null;
  }
  return smp->iter_key(m_pos);
}

bool c_StableMapIterator::t_valid() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMapIterator, StableMapIterator::valid);
  return m_pos != 0;
}

void c_StableMapIterator::t_next() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMapIterator, StableMapIterator::next);
  c_StableMap* smp = m_obj.get();
  if (UNLIKELY(m_versionNumber != smp->getVersionNumber())) {
    throw_collection_modified();
    return;
  }
  m_pos = smp->iter_next(m_pos);
}

void c_StableMapIterator::t_rewind() {
  INSTANCE_METHOD_INJECTION_BUILTIN(StableMapIterator, StableMapIterator::rewind);
  c_StableMap* smp = m_obj.get();
  if (UNLIKELY(m_versionNumber != smp->getVersionNumber())) {
    throw_collection_modified();
    return;
  }
  m_pos = smp->iter_begin();
}

///////////////////////////////////////////////////////////////////////////////
}
