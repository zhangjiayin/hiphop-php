/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1998-2010 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
*/
#define INLINE_VARIANT_HELPER 1 // for selected inlining

#include <runtime/base/array/zend_array.h>
#include <runtime/base/array/array_init.h>
#include <runtime/base/array/array_iterator.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/runtime_error.h>
#include <runtime/base/externals.h>
#include <util/hash.h>
#include <util/lock.h>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS_CLS(ZendArray, Bucket);
IMPLEMENT_SMART_ALLOCATION_HOT(ZendArray, SmartAllocatorImpl::NeedSweep);

// append/insert/update

#define CONNECT_TO_BUCKET_LIST(element, list_head)                      \
  (element)->pNext = (list_head);                                       \

#define CONNECT_TO_GLOBAL_DLLIST_INIT(element)                          \
do {                                                                    \
  (element)->pListLast = m_pListTail;                                   \
  m_pListTail = (element);                                              \
  (element)->pListNext = NULL;                                          \
  if ((element)->pListLast != NULL) {                                   \
    (element)->pListLast->pListNext = (element);                        \
  }                                                                     \
} while (false)

#define CONNECT_TO_GLOBAL_DLLIST(element)                               \
do {                                                                    \
  CONNECT_TO_GLOBAL_DLLIST_INIT(element);                               \
  if (!m_pListHead) {                                                   \
    m_pListHead = (element);                                            \
  }                                                                     \
  if (m_pos == 0) {                                                     \
    m_pos = (ssize_t)(element);                                         \
  }                                                                     \
  /* If there could be any strong iterators that are past the end, */   \
  /* we need to a pass and update these iterators to point to the */    \
  /* newly added element. */                                            \
  if (m_flag & StrongIteratorPastEnd) {                                 \
    m_flag &= ~StrongIteratorPastEnd;                                   \
    int sz = m_strongIterators.size();                                  \
    bool shouldWarn = false;                                            \
    for (int i = 0; i < sz; ++i) {                                      \
      if (m_strongIterators.get(i)->pos == 0) {                         \
        m_strongIterators.get(i)->pos = (ssize_t)(element);             \
        shouldWarn = true;                                              \
      }                                                                 \
    }                                                                   \
    if (shouldWarn) {                                                   \
      raise_warning("An element was added to an array inside foreach "  \
                    "by reference when iterating over the last "        \
                    "element. This may lead to unexpeced results.");    \
    }                                                                   \
  }                                                                     \
} while (false)

#define SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p)                   \
do {                                                                    \
  m_arBuckets[nIndex] = (p);                                            \
} while (0)

///////////////////////////////////////////////////////////////////////////////
// static members

StaticEmptyZendArray StaticEmptyZendArray::s_theEmptyArray;

///////////////////////////////////////////////////////////////////////////////
// construction/destruciton

void ZendArray::init(uint nSize) {
  uint size = MinSize;
  if (nSize >= 0x80000000) {
    size = 0x80000000; // prevent overflow
  } else {
    while (size < nSize) size <<= 1;
  }
  m_nTableMask = size - 1;
  if (size <= MinSize) {
    m_arBuckets = m_inlineBuckets;
    memset(m_inlineBuckets, 0, MinSize * sizeof(Bucket*));
  } else {
    m_arBuckets = (Bucket **)calloc(tableSize(), sizeof(Bucket*));
  }
}

HOT_FUNC_HPHP
ZendArray::ZendArray(uint nSize) :
  m_flag(0), m_pListHead(NULL), m_pListTail(NULL), m_nNextFreeElement(0) {
  m_size = 0;
  init(nSize);
}

HOT_FUNC_HPHP
ZendArray::ZendArray(uint nSize, int64 n, Bucket *bkts[]) :
  m_flag(0), m_pListHead(bkts[0]), m_pListTail(0), m_nNextFreeElement(n) {
  m_pos = (ssize_t)(m_pListHead);
  m_size = nSize;
  init(nSize);
  for (Bucket **b = bkts; *b; b++) {
    Bucket *p = *b;
    uint nIndex = (p->hashKey() & m_nTableMask);
    CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
    m_arBuckets[nIndex] = p;
    CONNECT_TO_GLOBAL_DLLIST_INIT(p);
  }
}

HOT_FUNC_HPHP
ZendArray::~ZendArray() {
  Bucket *p = m_pListHead;
  while (p) {
    Bucket *q = p;
    p = p->pListNext;
    DELETE(Bucket)(q);
  }
  if (m_arBuckets != m_inlineBuckets) {
    free(m_arBuckets);
  }
}

ssize_t ZendArray::vsize() const { assert(false); }

///////////////////////////////////////////////////////////////////////////////
// iterations

HOT_FUNC_HPHP
ssize_t ZendArray::iter_begin() const {
  Bucket *p = m_pListHead;
  return p ? reinterpret_cast<ssize_t>(p) : ArrayData::invalid_index;
}

ssize_t ZendArray::iter_end() const {
  Bucket *p = m_pListTail;
  return p ? reinterpret_cast<ssize_t>(p) : ArrayData::invalid_index;
}

HOT_FUNC_HPHP
ssize_t ZendArray::iter_advance(ssize_t prev) const {
  if (prev == 0 || prev == ArrayData::invalid_index) {
    return ArrayData::invalid_index;
  }
  Bucket *p = reinterpret_cast<Bucket *>(prev);
  p = p->pListNext;
  return p ? reinterpret_cast<ssize_t>(p) : ArrayData::invalid_index;
}

ssize_t ZendArray::iter_rewind(ssize_t prev) const {
  if (prev == 0 || prev == ArrayData::invalid_index) {
    return ArrayData::invalid_index;
  }
  Bucket *p = reinterpret_cast<Bucket *>(prev);
  p = p->pListLast;
  return p ? reinterpret_cast<ssize_t>(p) : ArrayData::invalid_index;
}

HOT_FUNC_HPHP
Variant ZendArray::getKey(ssize_t pos) const {
  ASSERT(pos && pos != ArrayData::invalid_index);
  Bucket *p = reinterpret_cast<Bucket *>(pos);
  if (p->hasStrKey()) {
    return p->skey;
  }
  return (int64)p->ikey;
}

Variant ZendArray::getValue(ssize_t pos) const {
  ASSERT(pos && pos != ArrayData::invalid_index);
  Bucket *p = reinterpret_cast<Bucket *>(pos);
  return p->data;
}

HOT_FUNC_HPHP
CVarRef ZendArray::getValueRef(ssize_t pos) const {
  ASSERT(pos && pos != ArrayData::invalid_index);
  Bucket *p = reinterpret_cast<Bucket *>(pos);
  return p->data;
}

bool ZendArray::isVectorData() const {
  int64 index = 0;
  for (Bucket *p = m_pListHead; p; p = p->pListNext) {
    if (p->hasStrKey() || p->ikey != index++) return false;
  }
  return true;
}

Variant ZendArray::reset() {
  m_pos = (ssize_t)m_pListHead;
  if (m_pListHead) {
    return m_pListHead->data;
  }
  return false;
}

Variant ZendArray::prev() {
  if (m_pos) {
    Bucket *p = reinterpret_cast<Bucket *>(m_pos);
    p = p->pListLast;
    m_pos = (ssize_t)p;
    if (p) {
      return p->data;
    }
  }
  return false;
}

Variant ZendArray::next() {
  if (m_pos) {
    Bucket *p = reinterpret_cast<Bucket *>(m_pos);
    p = p->pListNext;
    m_pos = (ssize_t)p;
    if (p) {
      return p->data;
    }
  }
  return false;
}

Variant ZendArray::end() {
  m_pos = (ssize_t)m_pListTail;
  if (m_pListTail) {
    return m_pListTail->data;
  }
  return false;
}

Variant ZendArray::key() const {
  if (m_pos) {
    Bucket *p = reinterpret_cast<Bucket *>(m_pos);
    if (p->hasStrKey()) {
      return p->skey;
    }
    return (int64)p->ikey;
  }
  return null;
}

Variant ZendArray::value(ssize_t &pos) const {
  if (pos && pos != ArrayData::invalid_index) {
    Bucket *p = reinterpret_cast<Bucket *>(pos);
    return p->data;
  }
  return false;
}

Variant ZendArray::current() const {
  if (m_pos) {
    Bucket *p = reinterpret_cast<Bucket *>(m_pos);
    return p->data;
  }
  return false;
}

static StaticString s_value("value");
static StaticString s_key("key");

Variant ZendArray::each() {
  if (m_pos) {
    ArrayInit init(4);
    Bucket *p = reinterpret_cast<Bucket *>(m_pos);
    Variant key = getKey(m_pos);
    Variant value = getValue(m_pos);
    init.set(1, value);
    init.set(s_value, value, true);
    init.set(0, key);
    init.set(s_key, key, true);
    m_pos = (ssize_t)p->pListNext;
    return Array(init.create());
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////
// lookups

static bool hit_string_key(const ZendArray::Bucket *p, const char *k, int len,
                           int32_t hash) {
  if (!p->hasStrKey()) return false;
  const char *data = p->skey->data();
  return data == k || p->hash() == hash
                      && p->skey->size() == len &&
                      memcmp(data, k, len) == 0;
}

ZendArray::Bucket *ZendArray::find(int64 h) const {
  for (Bucket *p = m_arBuckets[h & m_nTableMask]; p; p = p->pNext) {
    if (!p->hasStrKey() && p->ikey == h) {
      return p;
    }
  }
  return NULL;
}

ZendArray::Bucket *ZendArray::find(const char *k, int len,
                                   strhash_t prehash) const {
  int32_t hash = ZendArray::Bucket::encodeHash(prehash);
  for (Bucket *p = m_arBuckets[prehash & m_nTableMask]; p; p = p->pNext) {
    if (hit_string_key(p, k, len, hash)) return p;
  }
  return NULL;
}

ZendArray::Bucket *ZendArray::findForInsert(int64 h) const {
  Bucket *p = m_arBuckets[h & m_nTableMask];
  if (UNLIKELY(!p)) return NULL;
  if (LIKELY(!p->hasStrKey() && p->ikey == h)) {
    return p;
  }
  p = p->pNext;
  if (UNLIKELY(!p)) return NULL;
  if (LIKELY(!p->hasStrKey() && p->ikey == h)) {
    return p;
  }
  p = p->pNext;
  int n = 2;
  while (p) {
    if (!p->hasStrKey() && p->ikey == h) {
      return p;
    }
    p = p->pNext;
    n++;
  }
  if (UNLIKELY(n > RuntimeOption::MaxArrayChain)) {
    raise_error("Array is too unbalanced (%d)", n);
  }
  return NULL;
}

ZendArray::Bucket *ZendArray::findForInsert(const char *k, int len,
                                            strhash_t prehash) const {
  int n = 0;
  int32_t hash = ZendArray::Bucket::encodeHash(prehash);
  for (Bucket *p = m_arBuckets[prehash & m_nTableMask]; p; p = p->pNext) {
    if (hit_string_key(p, k, len, hash)) return p;
    n++;
  }
  if (UNLIKELY(n > RuntimeOption::MaxArrayChain)) {
    raise_error("Array is too unbalanced (%d)", n);
  }
  return NULL;
}

ZendArray::Bucket ** ZendArray::findForErase(int64 h) const {
  Bucket ** ret = &(m_arBuckets[h & m_nTableMask]);
  Bucket * p = *ret;
  while (p) {
    if (!p->hasStrKey() && p->ikey == h) {
      return ret;
    }
    ret = &(p->pNext);
    p = *ret;
  }
  return NULL;
}

ZendArray::Bucket ** ZendArray::findForErase(const char *k, int len,
                                             strhash_t prehash) const {
  Bucket ** ret = &(m_arBuckets[prehash & m_nTableMask]);
  Bucket * p = *ret;
  int32_t hash = ZendArray::Bucket::encodeHash(prehash);
  while (p) {
    if (hit_string_key(p, k, len, hash)) return ret;
    ret = &(p->pNext);
    p = *ret;
  }
  return NULL;
}

ZendArray::Bucket ** ZendArray::findForErase(Bucket * bucketPtr) const {
  if (bucketPtr == NULL)
    return NULL;
  int64 h = bucketPtr->hashKey();
  Bucket ** ret = &(m_arBuckets[h & m_nTableMask]);
  Bucket * p = *ret;
  while (p) {
    if (p == bucketPtr) return ret;
    ret = &(p->pNext);
    p = *ret;
  }
  return NULL;
}

bool ZendArray::exists(int64 k) const {
  return find(k);
}

bool ZendArray::exists(litstr k) const {
  return find(k, strlen(k), hash_string(k));
}

HOT_FUNC_HPHP
bool ZendArray::exists(CStrRef k) const {
  return find(k.data(), k.size(), k->hash());
}

typedef Variant::TypedValueAccessor TypedValueAccessor;

inline static bool isIntKey(TypedValueAccessor tva) {
  return Variant::GetAccessorType(tva) <= KindOfInt64;
}

inline static int64 getIntKey(TypedValueAccessor tva) {
  return Variant::GetInt64(tva);
}

inline static StringData *getStringKey(TypedValueAccessor tva) {
  return Variant::GetStringData(tva);
}

HOT_FUNC_HPHP
bool ZendArray::exists(CVarRef k) const {
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) return find(getIntKey(tva));
  ASSERT(k.isString());
  StringData *key = getStringKey(tva);
  return find(key->data(), key->size(), key->hash());
}

HOT_FUNC_HPHP
CVarRef ZendArray::get(int64 k, bool error /* = false */) const {
  Bucket *p = find(k);
  if (p) {
    return p->data;
  }
  if (error) {
    raise_notice("Undefined index: %lld", k);
  }
  return null_variant;
}

CVarRef ZendArray::get(litstr k, bool error /* = false */) const {
  int len = strlen(k);
  Bucket *p = find(k, len, hash_string(k, len));
  if (p) {
    return p->data;
  }
  if (error) {
    raise_notice("Undefined index: %s", k);
  }
  return null_variant;
}

HOT_FUNC_HPHP
CVarRef ZendArray::get(CStrRef k, bool error /* = false */) const {
  StringData *key = k.get();
  strhash_t prehash = key->hash();
  Bucket *p = find(key->data(), key->size(), prehash);
  if (p) {
    return p->data;
  }
  if (error) {
    raise_notice("Undefined index: %s", k.data());
  }
  return null_variant;
}

HOT_FUNC_HPHP
CVarRef ZendArray::get(CVarRef k, bool error /* = false */) const {
  Bucket *p;
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    p = find(getIntKey(tva));
  } else {
    ASSERT(k.isString());
    StringData *strkey = getStringKey(tva);
    strhash_t prehash = strkey->hash();
    p = find(strkey->data(), strkey->size(), prehash);
  }
  if (p) {
    return p->data;
  }
  if (error) {
    raise_notice("Undefined index: %s", k.toString().data());
  }
  return null_variant;
}

ssize_t ZendArray::getIndex(int64 k) const {
  Bucket *p = find(k);
  if (p) {
    return (ssize_t)p;
  }
  return ArrayData::invalid_index;
}

ssize_t ZendArray::getIndex(litstr k) const {
  int len = strlen(k);
  Bucket *p = find(k, len, hash_string(k, len));
  if (p) {
    return (ssize_t)p;
  }
  return ArrayData::invalid_index;
}

ssize_t ZendArray::getIndex(CStrRef k) const {
  Bucket *p = find(k.data(), k.size(), k->hash());
  if (p) {
    return (ssize_t)p;
  }
  return ArrayData::invalid_index;
}

ssize_t ZendArray::getIndex(CVarRef k) const {
  Bucket *p;
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    p = find(getIntKey(tva));
  } else {
    ASSERT(k.isString());
    StringData *key = getStringKey(tva);
    p = find(key->data(), key->size(), key->hash());
  }
  if (p) {
    return (ssize_t)p;
  }
  return ArrayData::invalid_index;
}

HOT_FUNC_HPHP
void ZendArray::resize() {
  uint newSize = tableSize() << 1;
  // No need to use calloc() or memset(), as rehash() is going to clear
  // m_arBuckets any way.
  if (m_arBuckets == m_inlineBuckets) {
    m_arBuckets = (Bucket**)malloc(newSize * sizeof(Bucket*));
  } else {
    m_arBuckets = (Bucket **)realloc(m_arBuckets, newSize * sizeof(Bucket*));
  }
  m_nTableMask = newSize - 1;
  rehash();
}

void ZendArray::rehash() {
  memset(m_arBuckets, 0, tableSize() * sizeof(Bucket*));
  for (Bucket *p = m_pListHead; p; p = p->pListNext) {
    uint nIndex = (p->hashKey() & m_nTableMask);
    CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
    SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  }
}

HOT_FUNC_HPHP
bool ZendArray::nextInsert(CVarRef data) {
  if (m_nNextFreeElement < 0) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return false;
  }
  int64 h = m_nNextFreeElement;
  Bucket * p = NEW(Bucket)(data);
  p->setIntKey(h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  m_nNextFreeElement = h + 1;
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

bool ZendArray::nextInsertRef(CVarRef data) {
  if (m_nNextFreeElement < 0) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return false;
  }
  int64 h = m_nNextFreeElement;
  Bucket * p = NEW(Bucket)(strongBind(data));
  p->setIntKey(h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  m_nNextFreeElement = h + 1;
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

bool ZendArray::nextInsertWithRef(CVarRef data) {
  int64 h = m_nNextFreeElement;
  Bucket * p = NEW(Bucket)(withRefBind(data));
  p->setIntKey(h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  m_nNextFreeElement = h + 1;
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::addLvalImpl(int64 h, Variant **pDest,
                            bool doFind /* = true */) {
  ASSERT(pDest != NULL);
  Bucket *p;
  if (doFind) {
    p = findForInsert(h);
    if (p) {
      *pDest = &p->data;
      return false;
    }
  }
  p = NEW(Bucket)();
  p->setIntKey(h);
  if (pDest) {
    *pDest = &p->data;
  }
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (h >= m_nNextFreeElement && m_nNextFreeElement >= 0) {
    m_nNextFreeElement = h + 1;
  }
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::addLvalImpl(StringData *key, strhash_t h, Variant **pDest,
                            bool doFind /* = true */) {
  ASSERT(key != NULL && pDest != NULL);
  Bucket *p;
  if (doFind) {
    p = findForInsert(key->data(), key->size(), h);
    if (p) {
      *pDest = &p->data;
      return false;
    }
  }
  p = NEW(Bucket)();
  p->setStrKey(key, h);
  *pDest = &p->data;
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::addValWithRef(int64 h, CVarRef data) {
  Bucket *p = findForInsert(h);
  if (p) {
    return false;
  }
  p = NEW(Bucket)(withRefBind(data));
  p->setIntKey(h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (h >= m_nNextFreeElement && m_nNextFreeElement >= 0) {
    m_nNextFreeElement = h + 1;
  }
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::addValWithRef(StringData *key, CVarRef data) {
  strhash_t h = key->hash();
  Bucket *p = findForInsert(key->data(), key->size(), h);
  if (p) {
    return false;
  }
  p = NEW(Bucket)(withRefBind(data));
  p->setStrKey(key, h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::update(int64 h, CVarRef data) {
  Bucket *p = findForInsert(h);
  if (p) {
    p->data.assignValHelper(data);
    return true;
  }

  p = NEW(Bucket)(data);
  p->setIntKey(h);

  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);

  if (h >= m_nNextFreeElement && m_nNextFreeElement >= 0) {
    m_nNextFreeElement = h + 1;
  }
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

HOT_FUNC_HPHP
bool ZendArray::update(StringData *key, CVarRef data) {
  strhash_t h = key->hash();
  Bucket *p = findForInsert(key->data(), key->size(), h);
  if (p) {
    p->data.assignValHelper(data);
    return true;
  }

  p = NEW(Bucket)(data);
  p->setStrKey(key, h);

  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);

  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

bool ZendArray::updateRef(int64 h, CVarRef data) {
  Bucket *p = findForInsert(h);
  if (p) {
    p->data.assignRefHelper(data);
    return true;
  }

  p = NEW(Bucket)(strongBind(data));
  p->setIntKey(h);

  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);

  if (h >= m_nNextFreeElement && m_nNextFreeElement >= 0) {
    m_nNextFreeElement = h + 1;
  }
  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

bool ZendArray::updateRef(StringData *key, CVarRef data) {
  strhash_t h = key->hash();
  Bucket *p = findForInsert(key->data(), key->size(), h);
  if (p) {
    p->data.assignRefHelper(data);
    return true;
  }

  p = NEW(Bucket)(strongBind(data));
  p->setStrKey(key, h);

  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);

  if (++m_size > tableSize()) {
    resize();
  }
  return true;
}

ArrayData *ZendArray::lval(int64 k, Variant *&ret, bool copy,
                           bool checkExist /* = false */) {
  if (!copy) {
    addLvalImpl(k, &ret);
    return NULL;
  }
  if (!checkExist) {
    ZendArray *a = copyImpl();
    a->addLvalImpl(k, &ret);
    return a;
  }
  Bucket *p = findForInsert(k);
  if (p &&
      (p->data.isReferenced() || p->data.isObject())) {
    ret = &p->data;
    return NULL;
  }
  ZendArray *a = copyImpl();
  a->addLvalImpl(k, &ret, p);
  return a;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::lval(CStrRef k, Variant *&ret, bool copy,
                           bool checkExist /* = false */) {
  StringData *key = k.get();
  strhash_t prehash = key->hash();
  if (!copy) {
    addLvalImpl(key, prehash, &ret);
    return NULL;
  }
  if (!checkExist) {
    ZendArray *a = copyImpl();
    a->addLvalImpl(key, prehash, &ret);
    return a;
  }
  Bucket *p = findForInsert(key->data(), key->size(), prehash);
  if (p &&
      (p->data.isReferenced() || p->data.isObject())) {
    ret = &p->data;
    return NULL;
  }
  ZendArray *a = copyImpl();
  a->addLvalImpl(key, prehash, &ret, p);
  return a;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::lvalPtr(CStrRef k, Variant *&ret, bool copy,
                              bool create) {
  StringData *key = k.get();
  strhash_t prehash = key->hash();
  ZendArray *a = 0, *t = this;
  if (UNLIKELY(copy)) {
    a = t = copyImpl();
  }

  if (create) {
    t->addLvalImpl(key, prehash, &ret);
  } else {
    Bucket *p = t->findForInsert(key->data(), key->size(), prehash);
    if (p) {
      ret = &p->data;
    } else {
      ret = NULL;
    }
  }
  return a;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::lvalPtr(int64 k, Variant *&ret, bool copy,
                              bool create) {
  ZendArray *a = 0, *t = this;
  if (UNLIKELY(copy)) {
    a = t = copyImpl();
  }

  if (create) {
    t->addLvalImpl(k, &ret);
  } else {
    Bucket *p = t->findForInsert(k);
    if (p) {
      ret = &p->data;
    } else {
      ret = NULL;
    }
  }
  return a;
}

ArrayData *ZendArray::lval(litstr k, Variant *&ret, bool copy,
                           bool checkExist /* = false */) {
  String s(k, AttachLiteral);
  return lval(s, ret, copy, checkExist);
}

HOT_FUNC_HPHP
ArrayData *ZendArray::lval(CVarRef k, Variant *&ret, bool copy,
                           bool checkExist /* = false */) {
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    return lval(getIntKey(tva), ret, copy, checkExist);
  } else {
    ASSERT(k.isString());
    return lval(k.toStrNR(), ret, copy, checkExist);
  }
}

ArrayData *ZendArray::lvalNew(Variant *&ret, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    if (!a->nextInsert(null)) {
      ret = &(Variant::lvalBlackHole());
      return a;
    }
    ASSERT(a->m_pListTail);
    ret = &a->m_pListTail->data;
    return a;
  }
  if (!nextInsert(null)) {
    ret = &(Variant::lvalBlackHole());
    return NULL;
  }
  ASSERT(m_pListTail);
  ret = &m_pListTail->data;
  return NULL;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::set(int64 k, CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->update(k, v);
    return a;
  }
  update(k, v);
  return NULL;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::set(CStrRef k, CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->update(k.get(), v);
    return a;
  }
  update(k.get(), v);
  return NULL;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::set(CVarRef k, CVarRef v, bool copy) {
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->update(getIntKey(tva), v);
      return a;
    }
    update(getIntKey(tva), v);
    return NULL;
  } else {
    ASSERT(k.isString());
    StringData *sd = getStringKey(tva);
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->update(sd, v);
      return a;
    }
    update(sd, v);
    return NULL;
  }
}

ArrayData *ZendArray::setRef(int64 k, CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->updateRef(k, v);
    return a;
  }
  updateRef(k, v);
  return NULL;
}

ArrayData *ZendArray::setRef(CStrRef k, CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->updateRef(k.get(), v);
    return a;
  }
  updateRef(k.get(), v);
  return NULL;
}

ArrayData *ZendArray::setRef(CVarRef k, CVarRef v, bool copy) {
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->updateRef(getIntKey(tva), v);
      return a;
    }
    updateRef(getIntKey(tva), v);
    return NULL;
  } else {
    ASSERT(k.isString());
    StringData *sd = getStringKey(tva);
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->updateRef(sd, v);
      return a;
    }
    updateRef(sd, v);
    return NULL;
  }
}

HOT_FUNC_HPHP
ArrayData *ZendArray::add(int64 k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  if (UNLIKELY(copy)) {
    ZendArray *result = copyImpl();
    result->add(k, v, false);
    return result;
  }
  Bucket *p = NEW(Bucket)(v);
  p->setIntKey(k);
  uint nIndex = (k & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (k >= m_nNextFreeElement && m_nNextFreeElement >= 0) {
    m_nNextFreeElement = k + 1;
  }
  if (++m_size > tableSize()) {
    resize();
  }
  return NULL;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::add(CStrRef k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  if (UNLIKELY(copy)) {
    ZendArray *result = copyImpl();
    result->add(k, v, false);
    return result;
  }
  strhash_t h = k->hash();
  Bucket *p = NEW(Bucket)(v);
  p->setStrKey(k.get(), h);
  uint nIndex = (h & m_nTableMask);
  CONNECT_TO_BUCKET_LIST(p, m_arBuckets[nIndex]);
  SET_ARRAY_BUCKET_HEAD(m_arBuckets, nIndex, p);
  CONNECT_TO_GLOBAL_DLLIST(p);
  if (++m_size > tableSize()) {
    resize();
  }
  return NULL;
}

ArrayData *ZendArray::add(CVarRef k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) return add(getIntKey(tva), v, copy);
  ASSERT(k.isString());
  return add(k.toStrNR(), v, copy);
}

ArrayData *ZendArray::addLval(int64 k, Variant *&ret, bool copy) {
  ASSERT(!exists(k));
  if (UNLIKELY(copy)) {
    ZendArray *result = copyImpl();
    result->addLvalImpl(k, &ret, false);
    return result;
  }
  addLvalImpl(k, &ret, false);
  return NULL;
}

ArrayData *ZendArray::addLval(CStrRef k, Variant *&ret, bool copy) {
  ASSERT(!exists(k));
  if (UNLIKELY(copy)) {
    ZendArray *result = copyImpl();
    result->addLvalImpl(k.get(), k->hash(), &ret, false);
    return result;
  }
  addLvalImpl(k.get(), k->hash(), &ret, false);
  return NULL;
}

ArrayData *ZendArray::addLval(CVarRef k, Variant *&ret, bool copy) {
  ASSERT(!exists(k));
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) return addLval(getIntKey(tva), ret, copy);
  ASSERT(k.isString());
  return addLval(k.toStrNR(), ret, copy);
}

///////////////////////////////////////////////////////////////////////////////
// delete

HOT_FUNC_HPHP
void ZendArray::erase(Bucket ** prev, bool updateNext /* = false */) {
  if (prev == NULL)
    return;
  Bucket * p = *prev;
  bool nextElementUnsetInsideForeachByReference = false;
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
    if (m_pos == (ssize_t)p) {
      m_pos = (ssize_t)p->pListNext;
    }
    int sz = m_strongIterators.size();
    for (int i = 0; i < sz; ++i) {
      if (m_strongIterators.get(i)->pos == (ssize_t)p) {
        nextElementUnsetInsideForeachByReference = true;
        m_strongIterators.get(i)->pos = (ssize_t)p->pListNext;
        if (!(m_strongIterators.get(i)->pos)) {
          // Record that there is a strong iterator out there
          // that is past the end
          m_flag |= StrongIteratorPastEnd;
        }
      }
    }
    m_size--;
    // Match PHP 5.3.1 semantics
    if ((uint64)p->ikey == (uint64)(m_nNextFreeElement - 1) &&
        (p->ikey == 0x7fffffffffffffffLL || updateNext)) {
      --m_nNextFreeElement;
    }
    DELETE(Bucket)(p);
  }
  if (nextElementUnsetInsideForeachByReference) {
    if (RuntimeOption::EnableHipHopErrors) {
      raise_warning("The next element was unset inside foreach by reference. "
                    "This may lead to unexpeced results.");
    }
  }
}

HOT_FUNC_HPHP
ArrayData *ZendArray::remove(int64 k, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->erase(a->findForErase(k));
    return a;
  }
  erase(findForErase(k));
  return NULL;
}

ArrayData *ZendArray::remove(CStrRef k, bool copy) {
  strhash_t prehash = k->hash();
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->erase(a->findForErase(k.data(), k.size(), prehash));
    return a;
  }
  erase(findForErase(k.data(), k.size(), prehash));
  return NULL;
}

ArrayData *ZendArray::remove(CVarRef k, bool copy) {
  TypedValueAccessor tva = k.getTypedAccessor();
  if (isIntKey(tva)) {
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->erase(a->findForErase(getIntKey(tva)));
      return a;
    }
    erase(findForErase(getIntKey(tva)));
    return NULL;
  } else {
    ASSERT(k.isString());
    StringData *key = getStringKey(tva);
    strhash_t prehash = key->hash();
    if (UNLIKELY(copy)) {
      ZendArray *a = copyImpl();
      a->erase(a->findForErase(key->data(), key->size(), prehash));
      return a;
    }
    erase(findForErase(key->data(), key->size(), prehash));
    return NULL;
  }
}

ArrayData *ZendArray::copy() const {
  return copyImpl();
}

ArrayData *ZendArray::copyWithStrongIterators() const {
  ZendArray* copied = copyImpl();
  // Transfer strong iterators
  if (!m_strongIterators.empty()) {
    for (int k = 0; k < m_strongIterators.size(); ++k) {
      FullPos* fp = m_strongIterators.get(k);
      // Update fp.pos to point to the corresponding element in 'copied'
      Bucket* p = reinterpret_cast<Bucket*>(fp->pos);
      if (p) {
        Bucket* copiedP;
        if (p->hasStrKey()) {
          copiedP = copied->find(p->skey->data(), p->skey->size(),
                                 (strhash_t)p->hash());
        } else {
          copiedP = copied->find((int64)p->ikey);
        }
        fp->pos = (ssize_t)copiedP;
      }
      fp->container = copied;
      copied->m_strongIterators.push(fp);
    }
    // Copy the flags
    copied->m_flag |= (m_flag & StrongIteratorPastEnd);
    // Clear the strong iterator list and flags from the original array
    ZendArray* src = const_cast<ZendArray*>(this);
    src->m_strongIterators.clear();
    src->m_flag &= ~StrongIteratorPastEnd;
  }
  return copied;
}

inline ALWAYS_INLINE ZendArray *ZendArray::copyImplHelper(bool sma) const {
  ZendArray *target = LIKELY(sma) ? NEW(ZendArray)(m_size)
                                  : new ZendArray(m_size);
  Bucket *last = NULL;
  for (Bucket *p = m_pListHead; p; p = p->pListNext) {
    Bucket *np = LIKELY(sma) ? NEW(Bucket)(Variant::noInit)
                             : new Bucket(Variant::noInit);
    np->data.constructWithRefHelper(p->data, this);
    uint nIndex;
    if (p->hasStrKey()) {
      np->setStrKey(p->skey, p->hash());
      nIndex = p->hash() & target->m_nTableMask;
    } else {
      np->setIntKey(p->ikey);
      nIndex = p->ikey & target->m_nTableMask;
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

  target->m_size = m_size;
  target->m_nNextFreeElement = m_nNextFreeElement;

  Bucket *p = reinterpret_cast<Bucket *>(m_pos);
  if (p == NULL) {
    target->m_pos = (ssize_t)0;
  } else if (p == m_pListHead) {
    target->m_pos = (ssize_t)target->m_pListHead;
  } else {
    if (p->hasStrKey()) {
      target->m_pos = (ssize_t)target->find(p->skey->data(),
                                            p->skey->size(),
                                            (strhash_t)p->hash());
    } else {
      target->m_pos = (ssize_t)target->find((int64)p->ikey);
    }
  }
  return target;
}

ArrayData *ZendArray::nonSmartCopy() const {
  return copyImplHelper(false);
}

HOT_FUNC_HPHP
ZendArray *ZendArray::copyImpl() const {
  return copyImplHelper(true);
}

HOT_FUNC_HPHP
ArrayData *ZendArray::append(CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->nextInsert(v);
    return a;
  }
  nextInsert(v);
  return NULL;
}

ArrayData *ZendArray::appendRef(CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->nextInsertRef(v);
    return a;
  }
  nextInsertRef(v);
  return NULL;
}

ArrayData *ZendArray::appendWithRef(CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->nextInsertWithRef(v);
    return a;
  }
  nextInsertWithRef(v);
  return NULL;
}

HOT_FUNC_HPHP
ArrayData *ZendArray::append(const ArrayData *elems, ArrayOp op, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->append(elems, op, false);
    return a;
  }

  if (op == Plus) {
    for (ArrayIter it(elems); !it.end(); it.next()) {
      Variant key = it.first();
      CVarRef value = it.secondRef();
      if (key.isNumeric()) {
        addValWithRef(key.toInt64(), value);
      } else {
        addValWithRef(key.getStringData(), value);
      }
    }
  } else {
    ASSERT(op == Merge);
    for (ArrayIter it(elems); !it.end(); it.next()) {
      Variant key = it.first();
      CVarRef value = it.secondRef();
      if (key.isNumeric()) {
        nextInsertWithRef(value);
      } else {
        Variant *p;
        StringData *sd = key.getStringData();
        addLvalImpl(sd, sd->hash(), &p, true);
        p->setWithRef(value);
      }
    }
  }
  return NULL;
}

ArrayData *ZendArray::pop(Variant &value) {
  if (getCount() > 1) {
    ZendArray *a = copyImpl();
    a->pop(value);
    return a;
  }
  if (m_pListTail) {
    value = m_pListTail->data;
    erase(findForErase(m_pListTail), true);
  } else {
    value = null;
  }
  // To match PHP-like semantics, the pop operation resets the array's
  // internal iterator
  m_pos = (ssize_t)m_pListHead;
  return NULL;
}

ArrayData *ZendArray::dequeue(Variant &value) {
  if (getCount() > 1) {
    ZendArray *a = copyImpl();
    a->dequeue(value);
    return a;
  }
  // To match PHP-like semantics, we invalidate all strong iterators
  // when an element is removed from the beginning of the array
  if (!m_strongIterators.empty()) {
    freeStrongIterators();
  }
  if (m_pListHead) {
    value = m_pListHead->data;
    erase(findForErase(m_pListHead));
    renumber();
  } else {
    value = null;
  }
  // To match PHP-like semantics, the dequeue operation resets the array's
  // internal iterator
  m_pos = (ssize_t)m_pListHead;
  return NULL;
}

ArrayData *ZendArray::prepend(CVarRef v, bool copy) {
  if (UNLIKELY(copy)) {
    ZendArray *a = copyImpl();
    a->prepend(v, false);
    return a;
  }
  // To match PHP-like semantics, we invalidate all strong iterators
  // when an element is added to the beginning of the array
  if (!m_strongIterators.empty()) {
    freeStrongIterators();
  }
  nextInsert(v);
  if (m_size == 1) {
    return NULL; // only element in array, no need to move it.
  }

  // Move the newly inserted element from the tail to the front.
  Bucket *p = m_pListHead;
  Bucket *new_elem = m_pListTail;

  // Remove from end of list
  m_pListTail = new_elem->pListLast;
  if (m_pListTail) {
    m_pListTail->pListNext = NULL;
  }

  // Insert before new position (p)
  new_elem->pListNext = p;
  new_elem->pListLast = p->pListLast;
  p->pListLast = new_elem;
  if (new_elem->pListLast) {
    new_elem->pListLast->pListNext = new_elem;
  } else {
    // no 'last' means we inserted at the front, so fix that pointer
    ASSERT(m_pListHead == p);
    m_pListHead = new_elem;
  }

  // Rewrite numeric keys to start from 0 and rehash
  renumber();

  // To match PHP-like semantics, the prepend operation resets the array's
  // internal iterator
  m_pos = (ssize_t)m_pListHead;

  return NULL;
}

void ZendArray::renumber() {
  int64 i = 0;
  Bucket* p = m_pListHead;
  for (; p; p = p->pListNext) {
    if (!p->hasStrKey()) {
      if (p->ikey != (int64)i) {
        goto rehashNeeded;
      }
      ++i;
    }
  }
  m_nNextFreeElement = i;
  return;

rehashNeeded:
  for (; p; p = p->pListNext) {
    if (!p->hasStrKey()) {
      p->ikey = i;
      ++i;
    }
  }
  m_nNextFreeElement = i;
  rehash();
}

void ZendArray::onSetEvalScalar() {
  for (Bucket *p = m_pListHead; p; p = p->pListNext) {
    StringData *key = p->skey;
    if (p->hasStrKey() && !key->isStatic()) {
      StringData *skey= StringData::GetStaticString(key);
      if (key && key->decRefCount() == 0) {
        DELETE(StringData)(key);
      }
      p->skey = skey;
    }
    p->data.setEvalScalar();
  }
}

void ZendArray::getFullPos(FullPos &fp) {
  ASSERT(fp.container == (ArrayData*)this);
  fp.pos = m_pos;
  if (!fp.pos) {
    // Record that there is a strong iterator out there
    // that is past the end
    m_flag |= StrongIteratorPastEnd;
  }
}

bool ZendArray::setFullPos(const FullPos &fp) {
  ASSERT(fp.container == (ArrayData*)this);
  if (fp.pos) {
    m_pos = fp.pos;
    return true;
  }
  return false;
}

CVarRef ZendArray::currentRef() {
  ASSERT(m_pos);
  Bucket *p = reinterpret_cast<Bucket *>(m_pos);
  return p->data;
}

CVarRef ZendArray::endRef() {
  ASSERT(m_pos);
  Bucket *p = reinterpret_cast<Bucket *>(m_pListTail);
  return p->data;
}

///////////////////////////////////////////////////////////////////////////////
// memory allocator methods.

void ZendArray::sweep() {
  if (m_arBuckets != m_inlineBuckets) {
    free(m_arBuckets);
    m_arBuckets = m_inlineBuckets;
  }
  m_strongIterators.clear();
}

///////////////////////////////////////////////////////////////////////////////
// class Bucket

HOT_FUNC_HPHP
ZendArray::Bucket::~Bucket() {
  if (hasStrKey() && skey->decRefCount() == 0) {
    DELETE(StringData)(skey);
  }
}

void ZendArray::Bucket::dump() {
  printf("ZendArray::Bucket: %llx, %p, %p, %p\n",
         hashKey(), pListNext, pListLast, pNext);
  if (hasStrKey()) {
    skey->dump();
  }
  data.dump();
}

///////////////////////////////////////////////////////////////////////////////
}
