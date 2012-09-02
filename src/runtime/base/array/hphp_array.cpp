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

#define INLINE_VARIANT_HELPER 1

#include <runtime/base/array/hphp_array.h>
#include <runtime/base/array/array_init.h>
#include <runtime/base/array/array_iterator.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/runtime_error.h>
#include <runtime/base/variable_serializer.h>
#include <util/hash.h>
#include <util/lock.h>
#include <util/alloc.h>
#include <util/trace.h>
#include <util/util.h>
#include <runtime/base/tv_macros.h>
#include <runtime/base/execution_context.h>
#include <runtime/vm/stats.h>

// If PEDANTIC is defined, extra checks are performed to ensure correct
// function even as an array approaches 2^31 elements.  In practice this is
// just wasted effort though, since such an array would require on the order of
// 128 GiB of memory.
//#define PEDANTIC

namespace HPHP {

static const Trace::Module TRACEMOD = Trace::runtime;
///////////////////////////////////////////////////////////////////////////////

/*
 * Allocation of HphpArray buffers works like this: the smallest buffer
 * size is allocated inline in HphpArray.  The next group of ^2 sizes is
 * SmartAllocated, and big buffers are malloc'd.  HphpArray::m_allocMode
 * tracks the state as it progresses from
 *
 *   kInline -> kSmart -> kMalloc
 *
 * Hashtables never shrink, so the allocMode Never goes backwards.
 * If an array is pre-sized, we might skip directly to kSmart or kMalloc.
 * If an array is created via nonSmartCopy(), we skip kSmart.
 *
 * For kInline, we use space in HphpArray defined as InlineSlots.
 * The next couple size classes are declared below, using SlotsImpl as
 * a helper.  Each concrete class just needs to instantiate the smart
 * allocator members.
 *
 * Since size reallocations always follow a known sequence, each concrete
 * Slots class's alloc() method takes care of copying data from the next
 * size-class down and freeing it without any indirection.
 *
 * Finally we have allocSlots() and freeSlots() which take care of
 * using the proper concrete class, with minimum fuss and boilerplate.
 *
 * SlotsImpl declares space for both the Elm slots and the ElmInd
 * hashtable.  For small and medium-sized tables, the hashtable
 * still fits in-line in HphpArray even when the slots don't.
 * We handle that in the template by declaring hash[0], and
 * HphpArray::allocData/reallocData point m_hash to the inline space
 * instead of the space in the Slots class.
 *
 * For larger smart-allocated tables, m_hash will point to the hash[]
 * table declared here, just like we do for malloc.
 */

typedef HphpArray::Elm Elm;
typedef HphpArray::ElmInd ElmInd;
typedef HphpArray::InlineSlots InlineSlots;

/*
 * This is the implementation guts for each smart-allocated buffer;
 * size is compile-time constant.
 */
template <int Size, class Self, class Half>
struct SlotsImpl {
  static const uint HashCap = Size * sizeof(ElmInd) <= sizeof(InlineSlots) ?
                              0 : Size;
  static const uint Cap = Size - Size / HphpArray::LoadScale;
  Elm slots[Cap];
  ElmInd hash[HashCap];
  void dump() {}

  // allocate an instance of Self, copy data from the given instance
  // of Half, then free Half if necessary.
  static Elm* alloc(Elm* old_data) {
    Elm* data = (NEW(Self)())->slots;
    if (old_data) {
      memcpy(data, old_data, sizeof(Half::slots));
      Half::rel(old_data);
    }
    return data;
  }

  // Free an instance, given a pointer to its interior slots[] array.
  static void rel(Elm* data) {
    Self* p = (Self*)(uintptr_t(data) - offsetof(Self, slots));
    DELETE(Self)(p);
  }
};

struct Slots8: SlotsImpl<8, Slots8, InlineSlots> {
  DECLARE_SMART_ALLOCATION_NOCALLBACKS(Slots8);
};
struct Slots16: SlotsImpl<16, Slots16, Slots8> {
  DECLARE_SMART_ALLOCATION_NOCALLBACKS(Slots16);
};
struct Slots32: SlotsImpl<32, Slots32, Slots16> {
  DECLARE_SMART_ALLOCATION_NOCALLBACKS(Slots32);
};
struct Slots64: SlotsImpl<64, Slots64, Slots32> {
  DECLARE_SMART_ALLOCATION_NOCALLBACKS(Slots64);
};
const uint MaxSmartCap = Slots64::Cap;

Elm* allocSlots(uint cap, Elm* data) {
  ASSERT(cap <= MaxSmartCap);
  return cap <= Slots8::Cap  ? Slots8::alloc(data) :
         cap <= Slots16::Cap ? Slots16::alloc(data) :
         cap <= Slots32::Cap ? Slots32::alloc(data) :
                               Slots64::alloc(data);
}

void freeSlots(uint mask, Elm* data) {
  ASSERT(mask >= 7 && mask <= 63);
  switch (mask) {
    case 7:  Slots8::rel(data);  break;
    case 15: Slots16::rel(data); break;
    case 31: Slots32::rel(data); break;
    default: Slots64::rel(data); break;
  }
}

IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS(Slots8);
IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS(Slots16);
IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS(Slots32);
IMPLEMENT_SMART_ALLOCATION_NOCALLBACKS(Slots64);
IMPLEMENT_SMART_ALLOCATION(HphpArray, SmartAllocatorImpl::NeedSweep);
//=============================================================================
// Static members.

HphpArray HphpArray::s_theEmptyArray(StaticEmptyArray);

//=============================================================================
// Helpers.

static inline size_t computeTableSize(uint32 tableMask) {
  return size_t(tableMask) + size_t(1U);
}

static inline size_t computeMaxElms(uint32 tableMask) {
  return size_t(tableMask) - size_t(tableMask) / HphpArray::LoadScale;
}

static inline size_t computeDataSize(uint32 tableMask) {
  return computeTableSize(tableMask) * sizeof(HphpArray::ElmInd) +
    computeMaxElms(tableMask) * sizeof(HphpArray::Elm);
}

static inline void adjustUsageStats(size_t delta, bool refresh = false) {
  ThreadLocalNoCheck<MemoryManager>& mm = MemoryManager::TheMemoryManager();
  if (LIKELY(!mm.isNull())) {
    MemoryUsageStats& stats = mm->getStats();
    stats.alloc += delta;
    stats.usage += delta;
    JEMALLOC_STATS_ADJUST(&stats, delta);
    if (refresh) {
      mm->refreshStats();
    }
  }
}

static inline size_t computeMaskFromNumElms(uint32 numElms) {
  ASSERT(numElms <= 0x7fffffffU);
  size_t lgSize = HphpArray::MinLgTableSize;
  size_t maxElms = (size_t(3U)) << (lgSize-2);
  ASSERT(lgSize >= 2);
  while (maxElms < numElms) {
    ++lgSize;
    maxElms <<= 1;
  }
  ASSERT(lgSize <= 32);
  // return 2^lgSize - 1
  return ((size_t(1U)) << lgSize) - 1;
  static_assert(HphpArray::MinLgTableSize >= 2,
                "lower limit for 0.75 load factor");
}

static inline bool validElmInd(ssize_t /*HphpArray::ElmInd*/ ei) {
  return (ei > ssize_t(HphpArray::ElmIndEmpty));
}

static inline void initHash(HphpArray::ElmInd* hash, size_t tableSize) {
  ASSERT(HphpArray::ElmIndEmpty == -1);
  memset(hash, 0xffU, tableSize * sizeof(HphpArray::ElmInd));
}

static inline bool isIntegerKey(CVarRef v) __attribute__((always_inline));
static inline bool isIntegerKey(CVarRef v) {
  if (v.getRawType() <= KindOfInt64) return true;
  if (v.getRawType() != KindOfRef) return false;
  if (v.getRefData()->getRawType() <= KindOfInt64) return true;
  return false;
}

//=============================================================================
// Construction/destruction.

inline void HphpArray::init(uint size) {
  m_size = 0;
  m_tableMask = computeMaskFromNumElms(size);
  size_t tableSize = computeTableSize(m_tableMask);
  size_t maxElms = computeMaxElms(m_tableMask);
  allocData(maxElms, tableSize);
  initHash(m_hash, tableSize);
  m_pos = ArrayData::invalid_index;
}

HphpArray::HphpArray(uint size)
  : m_data(NULL), m_nextKI(0), m_hLoad(0), m_lastE(ElmIndEmpty),
    m_siPastEnd(false), m_nonsmart(false) {
#ifdef PEDANTIC
  if (size > 0x7fffffffU) {
    raise_error("Cannot create an array with more than 2^31 - 1 elements");
  }
#endif
  init(size);
}

HphpArray::HphpArray(EmptyMode)
  : m_data(NULL), m_nextKI(0), m_hLoad(0), m_lastE(ElmIndEmpty),
    m_siPastEnd(false), m_nonsmart(false) {
  init(0);
  setStatic();
}

// Empty constructor for internal use by nonSmartCopy() and copyImpl()
HphpArray::HphpArray(CopyMode mode) : m_nonsmart(mode == kNonSmartCopy) {
}

HOT_FUNC_VM
HphpArray::~HphpArray() {
  Elm* elms = m_data;
  ssize_t lastE = (ssize_t)m_lastE;
  for (ssize_t /*ElmInd*/ pos = 0; pos <= lastE; ++pos) {
    Elm* e = &elms[pos];
    if (e->data.m_type == KindOfTombstone) {
      continue;
    }
    if (e->hasStrKey()) {
      if (e->key->decRefCount() == 0) {
        e->key->release();
      }
    }
    TypedValue* tv = &e->data;
    if (IS_REFCOUNTED_TYPE(tv->m_type)) {
      tvDecRef(tv);
    }
  }
  if (m_allocMode == kSmart) {
    freeSlots(m_tableMask, m_data);
  } else if (m_allocMode == kMalloc) {
    free(m_data);
    adjustUsageStats(-computeDataSize(m_tableMask));
  }
}

ssize_t HphpArray::vsize() const {
  ASSERT(false && "vsize() called, but m_size should "
                  "never be -1 in HphpArray");
  return m_size;
}

void HphpArray::dumpDebugInfo() const {
  size_t maxElms = computeMaxElms(m_tableMask);
  size_t tableSize = computeTableSize(m_tableMask);

  fprintf(stderr,
          "--- dumpDebugInfo(this=0x%08zx) ----------------------------\n",
         uintptr_t(this));
  fprintf(stderr, "m_data = %p\tm_hash = %p\n"
         "m_tableMask = %u\tm_size = %d\tm_hLoad = %d\n"
         "m_nextKI = %lld\t\tm_lastE = %d\tm_pos = %zd\n",
         m_data, m_hash, m_tableMask, m_size, m_hLoad,
         m_nextKI, m_lastE, m_pos);
  fprintf(stderr, "Elements:\n");
  ssize_t lastE = m_lastE;
  Elm* elms = m_data;
  for (ssize_t /*ElmInd*/ i = 0; i <= lastE; ++i) {
    if (elms[i].data.m_type < KindOfTombstone) {
      Variant v = tvAsVariant(&elms[i].data);
      VariableSerializer vs(VariableSerializer::DebugDump);
      String s = vs.serialize(v, true);
      if (elms[i].hasStrKey()) {
        String k = Util::escapeStringForCPP(elms[i].key->data(),
                                            elms[i].key->size());
        fprintf(stderr, "  [%3d] hash=0x%016x key=\"%s\" data=(%.*s)\n",
               int(i), elms[i].hash, k.data(), s.size()-1, s.data());
      } else {
        fprintf(stderr, "  [%3d] ind=%lld data.m_type=(%.*s)\n", int(i),
               elms[i].ikey, s.size()-1, s.data());
      }
    } else {
      fprintf(stderr, "  [%3d] <tombstone>\n", int(i));
    }
  }
  if (size_t(m_lastE+1) < maxElms) {
    fprintf(stderr, "  [%3d..%-3zd] <uninitialized>\n", m_lastE+1, maxElms-1);
  }
  fprintf(stderr, "Hash table:");
  for (size_t i = 0; i < tableSize; ++i) {
    if ((i % 8) == 0) {
      fprintf(stderr, "\n  [%3zd..%-3zd]", i, i+7);
    }
    switch (m_hash[i]) {
    default: fprintf(stderr, "%12d", m_hash[i]); break;
    case ElmIndTombstone: fprintf(stderr, "%12s", "<tombstone>"); break;
    case ElmIndEmpty: fprintf(stderr, "%12s", "<empty>"); break;
    }
  }
  fprintf(stderr, "\n");
  fprintf(stderr,
          "---------------------------------------------------------------\n");
}

//=============================================================================
// Iteration.

inline /*ElmInd*/ ssize_t HphpArray::nextElm(Elm* elms,
                                             /*ElmInd*/ ssize_t ei) const {
  ASSERT(ei >= -1);
  ssize_t lastE = m_lastE;
  while (ei < lastE) {
    ++ei;
    if (elms[ei].data.m_type < KindOfTombstone) {
      return ei;
    }
  }
  return (ssize_t)ElmIndEmpty;
}

inline /*ElmInd*/ ssize_t HphpArray::prevElm(Elm* elms,
                                             /*ElmInd*/ ssize_t ei) const {
  ASSERT(ei <= (ssize_t)(m_lastE+1));
  while (ei > 0) {
    --ei;
    if (elms[ei].data.m_type < KindOfTombstone) {
      return ei;
    }
  }
  return (ssize_t)ElmIndEmpty;
}

ssize_t HphpArray::iter_begin() const {
  return nextElm(m_data, ElmIndEmpty);
}

ssize_t HphpArray::iter_end() const {
  return prevElm(m_data, (ssize_t)(m_lastE + 1));
}

ssize_t HphpArray::iter_advance(ssize_t pos) const {
  ssize_t lastE = m_lastE;
  ASSERT(ArrayData::invalid_index == -1);
  // Since lastE is always less than 2^32-1 and invalid_index == -1,
  // we can save a check by doing an unsigned comparison instead
  // of a signed comparison.
  if (size_t(pos) < size_t(lastE) &&
      m_data[pos + 1].data.m_type < KindOfTombstone) {
    return pos + 1;
  }
  return iter_advance_helper(pos);
}

ssize_t HphpArray::iter_advance_helper(ssize_t pos) const {
  Elm* elms = m_data;
  ssize_t lastE = m_lastE;
  // Since lastE is always less than 2^32-1 and invalid_index == -1,
  // we can save a check by doing an unsigned comparison instead of
  // a signed comparison.
  while (size_t(pos) < size_t(lastE)) {
    ++pos;
    if (elms[pos].data.m_type < KindOfTombstone) {
      return pos;
    }
  }
  return ArrayData::invalid_index;
}

ssize_t HphpArray::iter_rewind(ssize_t pos) const {
  if (pos == ArrayData::invalid_index) {
    return ArrayData::invalid_index;
  }
  return prevElm(m_data, pos);
}

Variant HphpArray::getKey(ssize_t pos) const {
  ASSERT(pos != ArrayData::invalid_index);
  Elm* e = &m_data[/*(ElmInd)*/pos];
  ASSERT(e->data.m_type != KindOfTombstone);
  if (e->hasStrKey()) {
    return e->key; // String key.
  }
  return e->ikey; // Integer key.
}

Variant HphpArray::getValue(ssize_t pos) const {
  ASSERT(pos != ArrayData::invalid_index);
  Elm* e = &m_data[/*(ElmInd)*/pos];
  ASSERT(e->data.m_type != KindOfTombstone);
  return tvAsCVarRef(&e->data);
}

CVarRef HphpArray::getValueRef(ssize_t pos) const {
  ASSERT(pos != ArrayData::invalid_index);
  Elm* e = &m_data[/*(ElmInd)*/pos];
  ASSERT(e->data.m_type != KindOfTombstone);
  return tvAsCVarRef(&e->data);
}

bool HphpArray::isVectorData() const {
  if (m_size == 0) {
    return true;
  }
  Elm* elms = m_data;
  int64 i = 0;
  for (ElmInd pos = 0; pos <= m_lastE; ++pos) {
    Elm* e = &elms[pos];
    if (e->data.m_type == KindOfTombstone) {
      continue;
    }
    if (e->hasStrKey() || e->ikey != i) {
      return false;
    }
    ++i;
  }
  return true;
}

Variant HphpArray::reset() {
  Elm* elms = m_data;
  m_pos = ssize_t(nextElm(elms, ElmIndEmpty));
  if (m_pos != ArrayData::invalid_index) {
    Elm* e = &elms[(ElmInd)m_pos];
    return tvAsCVarRef(&e->data);
  }
  m_pos = ArrayData::invalid_index;
  return false;
}

Variant HphpArray::prev() {
  if (m_pos != ArrayData::invalid_index) {
    Elm* elms = m_data;
    m_pos = prevElm(elms, m_pos);
    if (m_pos != ArrayData::invalid_index) {
      Elm* e = &elms[m_pos];
      return tvAsCVarRef(&e->data);
    }
  }
  return false;
}

Variant HphpArray::next() {
  if (m_pos != ArrayData::invalid_index) {
    Elm* elms = m_data;
    m_pos = nextElm(elms, m_pos);
    if (m_pos != ArrayData::invalid_index) {
      Elm* e = &elms[m_pos];
      ASSERT(e->data.m_type != KindOfTombstone);
      return tvAsCVarRef(&e->data);
    }
  }
  return false;
}

Variant HphpArray::end() {
  Elm* elms = m_data;
  m_pos = prevElm(elms, (ssize_t)(m_lastE+1));
  if (m_pos != ArrayData::invalid_index) {
    Elm* e = &elms[m_pos];
    ASSERT(e->data.m_type != KindOfTombstone);
    return tvAsCVarRef(&e->data);
  }
  return false;
}

Variant HphpArray::key() const {
  if (m_pos != ArrayData::invalid_index) {
    ASSERT(m_pos <= (ssize_t)m_lastE);
    Elm* e = &m_data[(ElmInd)m_pos];
    ASSERT(e->data.m_type != KindOfTombstone);
    if (e->hasStrKey()) {
      return e->key;
    }
    return e->ikey;
  }
  return null;
}

Variant HphpArray::value(ssize_t& pos) const {
  if (pos != ArrayData::invalid_index) {
    Elm* e = &m_data[pos];
    ASSERT(e->data.m_type != KindOfTombstone);
    return tvAsCVarRef(&e->data);
  }
  return false;
}

Variant HphpArray::current() const {
  if (m_pos != ArrayData::invalid_index) {
    Elm* e = &m_data[m_pos];
    ASSERT(e->data.m_type != KindOfTombstone);
    return tvAsCVarRef(&e->data);
  }
  return false;
}

static StaticString s_value("value");
static StaticString s_key("key");

Variant HphpArray::each() {
  if (m_pos != ArrayData::invalid_index) {
    ArrayInit init(4);
    Variant key = HphpArray::getKey(m_pos);
    Variant value = HphpArray::getValue(m_pos);
    init.set(int64(1), value);
    init.set(s_value, value, true);
    init.set(int64(0), key);
    init.set(s_key, key, true);
    m_pos = nextElm(m_data, m_pos);
    return Array(init.create());
  }
  return false;
}

//=============================================================================
// Lookup.

#define STRING_HASH(x)   (int32_t(x) | 0x80000000)

static bool hitStringKey(const HphpArray::Elm* e, const char* k, int len,
                         int32_t hash) {
  // hitStringKey() should only be called on an Elm that is referenced by a
  // hash table entry. HphpArray guarantees that when it adds a hash table
  // entry that it always sets it to refer to a valid element. Likewise when
  // it removes an element it always removes the corresponding hash entry.
  // Therefore the assertion below must hold.
  ASSERT(e->data.m_type != HphpArray::KindOfTombstone);

  if (!e->hasStrKey()) {
    return false;
  }
  const char* data = e->key->data();
  return data == k || ((e->hash == hash)
                       && e->key->size() == len
                       && (memcmp(data, k, len) == 0));
}

static bool hitIntKey(const HphpArray::Elm* e, int64 ki) {
  // hitIntKey() should only be called on an Elm that is referenced by a
  // hash table entry. HphpArray guarantees that when it adds a hash table
  // entry that it always sets it to refer to a valid element. Likewise when
  // it removes an element it always removes the corresponding hash entry.
  // Therefore the assertion below must hold.
  ASSERT(e->data.m_type != HphpArray::KindOfTombstone);
  return e->ikey == ki && e->hasIntKey();
}

// Quadratic probe is:
//
//   h(k, i) = (k + c1*i + c2*(i^2)) % tableSize
//
// Use 1/2 for c1 and c2.  In combination with a table size that is a power of
// 2, this guarantees a probe sequence of length tableSize that probes all
// table elements exactly once.

#define FIND_BODY(h0, hit) \
  size_t tableMask = m_tableMask; \
  size_t probeIndex = size_t(h0) & tableMask; \
  Elm* elms = m_data; \
  ssize_t /*ElmInd*/ pos = m_hash[probeIndex]; \
  if (LIKELY(pos == ssize_t(ElmIndEmpty) || (validElmInd(pos) && hit))) { \
    return pos; \
  } \
  /* Quadratic probe. */ \
  for (size_t i = 1;; ++i) { \
    ASSERT(i <= tableMask); \
    probeIndex = (probeIndex + i) & tableMask; \
    ASSERT(((size_t(h0)+((i + i*i) >> 1)) & tableMask) == probeIndex); \
    pos = m_hash[probeIndex]; \
    if (pos == ssize_t(ElmIndEmpty) || (validElmInd(pos) && hit)) { \
      return pos; \
    } \
  }

ssize_t /*ElmInd*/ HphpArray::find(int64 ki) const {
  FIND_BODY(ki, hitIntKey(&elms[pos], ki));
}

ssize_t /*ElmInd*/ HphpArray::find(const char* k, int len,
                                   strhash_t prehash) const {
  FIND_BODY(prehash, hitStringKey(&elms[pos], k, len, STRING_HASH(prehash)));
}
#undef FIND_BODY

#define FIND_FOR_INSERT_BODY(h0, hit) \
  ElmInd* ret = NULL; \
  size_t tableMask = m_tableMask; \
  size_t probeIndex = size_t(h0) & tableMask; \
  Elm* elms = m_data; \
  ElmInd* ei = &m_hash[probeIndex]; \
  ssize_t /*ElmInd*/ pos = *ei; \
  if (LIKELY(pos == ssize_t(ElmIndEmpty) || (validElmInd(pos) && hit))) { \
    return ei; \
  } \
  if (!validElmInd(pos)) ret = ei; \
  /* Quadratic probe. */ \
  for (size_t i = 1;; ++i) { \
    ASSERT(i <= tableMask); \
    probeIndex = (probeIndex + i) & tableMask; \
    ASSERT(((size_t(h0)+((i + i*i) >> 1)) & tableMask) == probeIndex); \
    ei = &m_hash[probeIndex]; \
    pos = ssize_t(*ei); \
    if (validElmInd(pos)) { \
      if (hit) { \
        ASSERT(m_hLoad <= computeMaxElms(tableMask)); \
        return ei; \
      } \
    } else { \
      if (ret == NULL) { \
        ret = ei; \
      } \
      if (pos == ElmIndEmpty) { \
        ASSERT(m_hLoad <= computeMaxElms(tableMask)); \
        return ret; \
      } \
    } \
  }

HphpArray::ElmInd* HphpArray::findForInsert(int64 ki) const {
  FIND_FOR_INSERT_BODY(ki, hitIntKey(&elms[pos], ki));
}

HphpArray::ElmInd* HphpArray::findForInsert(const char* k, int len,
                                            strhash_t prehash) const {
  FIND_FOR_INSERT_BODY(prehash, hitStringKey(&elms[pos], k, len,
                                             STRING_HASH(prehash)));
}
#undef FIND_FOR_INSERT_BODY

inline ALWAYS_INLINE
HphpArray::ElmInd* HphpArray::findForNewInsert(size_t h0) const {
  size_t tableMask = m_tableMask;
  size_t probeIndex = h0 & tableMask;
  ElmInd* ei = &m_hash[probeIndex];
  ssize_t /*ElmInd*/ pos = *ei;
  if (LIKELY(!validElmInd(pos))) {
    return ei;
  }
  /* Quadratic probe. */
  for (size_t i = 1;; ++i) {
    ASSERT(i <= tableMask);
    probeIndex = (probeIndex + i) & tableMask;
    ASSERT(((h0 + ((i + i * i) >> 1)) & tableMask) == probeIndex);
    ei = &m_hash[probeIndex];
    pos = ssize_t(*ei);
    if (!validElmInd(pos)) {
      return ei;
    }
  }
}

bool HphpArray::exists(int64 k) const {
  return find(k) != (ssize_t)ElmIndEmpty;
}

bool HphpArray::exists(litstr k) const {
  ssize_t /*ElmInd*/ pos = find(k, strlen(k), hash_string(k));
  return pos != ssize_t(ElmIndEmpty);
}

bool HphpArray::exists(CStrRef k) const {
  ssize_t /*ElmInd*/ pos = find(k.data(), k.size(), k->hash());
  return pos != ssize_t(ElmIndEmpty);
}

bool HphpArray::exists(CVarRef k) const {
  if (isIntegerKey(k)) {
    return find(k.toInt64()) != (ssize_t)ElmIndEmpty;
  }
  StringData* key = k.getStringData();
  ssize_t /*ElmInd*/ pos = find(key->data(), key->size(), key->hash());
  return pos != ssize_t(ElmIndEmpty);
}

CVarRef HphpArray::get(int64 k, bool error /* = false */) const {
  ElmInd pos = find(k);
  if (pos != ElmIndEmpty) {
    Elm* e = &m_data[pos];
    return tvAsCVarRef(&e->data);
  }
  if (error) {
    raise_notice("Undefined index: %lld", k);
  }
  return null_variant;
}

CVarRef HphpArray::get(litstr k, bool error /* = false */) const {
  int len = strlen(k);
  ElmInd pos = find(k, len, hash_string(k, len));
  if (pos != ElmIndEmpty) {
    Elm* e = &m_data[pos];
    return tvAsCVarRef(&e->data);
  }
  if (error) {
    raise_notice("Undefined index: %s", k);
  }
  return null_variant;
}

CVarRef HphpArray::get(CStrRef k, bool error /* = false */) const {
  StringData* key = k.get();
  strhash_t prehash = key->hash();
  ElmInd pos = find(key->data(), key->size(), prehash);
  if (pos != ElmIndEmpty) {
    Elm* e = &m_data[pos];
    return tvAsCVarRef(&e->data);
  }
  if (error) {
    raise_notice("Undefined index: %s", k.data());
  }
  return null_variant;
}

CVarRef HphpArray::get(CVarRef k, bool error /* = false */) const {
  ElmInd pos;
  if (isIntegerKey(k)) {
    pos = find(k.toInt64());
    if (pos != ElmIndEmpty) {
      Elm* e = &m_data[pos];
      return tvAsCVarRef(&e->data);
    }
  } else {
    StringData* strkey = k.getStringData();
    strhash_t prehash = strkey->hash();
    pos = find(strkey->data(), strkey->size(), prehash);
    if (pos != ElmIndEmpty) {
      Elm* e = &m_data[pos];
      return tvAsCVarRef(&e->data);
    }
  }
  if (error) {
    raise_notice("Undefined index: %s", k.toString().data());
  }
  return null_variant;
}

ssize_t HphpArray::getIndex(int64 k) const {
  return ssize_t(find(k));
}

ssize_t HphpArray::getIndex(litstr k) const {
  size_t len = strlen(k);
  return ssize_t(find(k, strlen(k), hash_string(k, len)));
}

ssize_t HphpArray::getIndex(CStrRef k) const {
  return ssize_t(find(k.data(), k.size(), k->hash()));
}

ssize_t HphpArray::getIndex(CVarRef k) const {
  if (isIntegerKey(k)) {
    return ssize_t(find(k.toInt64()));
  } else {
    StringData* key = k.getStringData();
    return ssize_t(find(key->data(), key->size(), key->hash()));
  }
}

//=============================================================================
// Append/insert/update.

inline ALWAYS_INLINE HphpArray::Elm* HphpArray::allocElm(ElmInd* ei) {
  ASSERT(!validElmInd(*ei));
  ASSERT(m_size != 0 || m_lastE == ElmIndEmpty);
#ifdef PEDANTIC
  if (m_size >= 0x7fffffffU) {
    raise_error("Cannot insert into array with 2^31 - 1 elements");
    return NULL;
  }
#endif
  // If we need to grow first before allocating another element,
  // return NULL to indicate that allocation failed.
  uint32 maxElms = computeMaxElms(m_tableMask);
  ASSERT(m_lastE == ElmIndEmpty || uint32(m_lastE)+1 <= maxElms);
  ASSERT(m_hLoad <= maxElms);
  if (uint32(m_lastE)+1 == maxElms || m_hLoad == maxElms) {
    return NULL;
  }
  ++m_size;
  m_hLoad += (*ei == ElmIndEmpty);
  ++m_lastE;
  (*ei) = m_lastE;
  Elm* e = &m_data[m_lastE];
  if (m_pos == ArrayData::invalid_index) {
    m_pos = ssize_t(m_lastE);
  }
  // If there could be any strong iterators that are past the end, we need to
  // do a pass and update these iterators to point to the newly added element.
  if (m_siPastEnd) {
    m_siPastEnd = false;
    int sz = m_strongIterators.size();
    bool shouldWarn = false;
    for (int i = 0; i < sz; ++i) {
      if (m_strongIterators.get(i)->pos == ssize_t(ElmIndEmpty)) {
        m_strongIterators.get(i)->pos = ssize_t(*ei);
        shouldWarn = true;
      }
    }
    if (shouldWarn) {
      raise_warning("An element was added to an array inside foreach "
                    "by reference when iterating over the last "
                    "element. This may lead to unexpeced results.");
    }
  }
  return e;
}

inline ALWAYS_INLINE
void HphpArray::initElm(Elm* e, size_t hki, StringData* key, CVarRef rhs,
                        bool isRef) {
  if (isRef) {
    tvAsUninitializedVariant(&e->data).constructRefHelper(rhs);
  } else {
    tvAsUninitializedVariant(&e->data).constructValHelper(rhs);
  }
  if (key) {
    e->setStrKey(key, hki);
    key->incRefCount();
  } else {
    e->setIntKey(hki);
  }
}

inline ALWAYS_INLINE
void HphpArray::allocNewElm(ElmInd* ei, size_t hki, StringData* key,
                            CVarRef data, bool byRef) {
  Elm* e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(hki);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }
  initElm(e, hki, key, data, byRef);
}

void HphpArray::allocData(size_t maxElms, size_t tableSize) {
  ASSERT(!m_data);
  if (maxElms <= SmallSize) {
    m_data = m_inline_data.slots;
    m_hash = m_inline_data.hash;
    m_allocMode = kInline;
    return;
  }
  size_t hashSize = tableSize * sizeof(ElmInd);
  size_t dataSize = maxElms * sizeof(Elm);
  if (maxElms <= MaxSmartCap && !m_nonsmart) {
    m_data = allocSlots(maxElms, 0);
    m_allocMode = kSmart;
  } else {
    size_t allocSize = hashSize <= sizeof(m_inline_hash) ? dataSize :
                       dataSize + hashSize;
    void* block = malloc(allocSize);
    if (!block) throw OutOfMemoryException(allocSize);
    m_data = (Elm*) block;
    m_allocMode = kMalloc;
    adjustUsageStats(allocSize);
  }
  m_hash = hashSize <= sizeof(m_inline_hash) ? m_inline_hash :
           (ElmInd*)(uintptr_t(m_data) + dataSize);
}

void HphpArray::reallocData(size_t maxElms, size_t tableSize, uint oldMask) {
  ASSERT(m_data && oldMask > 0 && maxElms > SmallSize);
  size_t hashSize = tableSize * sizeof(ElmInd);
  size_t dataSize = maxElms * sizeof(Elm);
  if (maxElms <= MaxSmartCap && !m_nonsmart) {
    m_data = allocSlots(maxElms, m_data);
    m_allocMode = kSmart;
  } else {
    size_t allocSize = hashSize <= sizeof(m_inline_hash) ? dataSize :
                       dataSize + hashSize;
    size_t oldDataSize = computeMaxElms(oldMask) * sizeof(Elm); // slots only.
    if (m_allocMode != kMalloc) {
      void* block = malloc(allocSize);
      if (block == NULL) throw OutOfMemoryException(allocSize);
      memcpy(block, m_data, oldDataSize);
      if (m_allocMode == kSmart) freeSlots(oldMask, m_data);
      m_data = (Elm*) block;
      m_allocMode = kMalloc;
      adjustUsageStats(allocSize);
    } else {
      void* block = realloc(m_data, allocSize);
      if (block == NULL) throw OutOfMemoryException(allocSize);
      m_data = (Elm*) block;
      size_t oldHashSize = computeTableSize(oldMask) * sizeof(ElmInd);
      size_t oldAllocSize = oldHashSize <= sizeof(m_inline_hash) ? oldDataSize :
                             oldDataSize + oldHashSize;
      adjustUsageStats(allocSize - oldAllocSize, true);
    }
  }
  m_hash = hashSize <= sizeof(m_inline_hash) ? m_inline_hash :
           (ElmInd*)(uintptr_t(m_data) + dataSize);
}

inline ALWAYS_INLINE void HphpArray::resizeIfNeeded() {
  uint32 maxElms = computeMaxElms(m_tableMask);
  ASSERT(m_lastE == ElmIndEmpty || uint32(m_lastE)+1 <= maxElms);
  ASSERT(m_hLoad <= maxElms);
  if (uint32(m_lastE)+1 == maxElms || m_hLoad == maxElms) {
    resize();
  }
}

void HphpArray::resize() {
  uint32 maxElms = computeMaxElms(m_tableMask);
  ASSERT(m_lastE == ElmIndEmpty || uint32(m_lastE)+1 <= maxElms);
  ASSERT(m_hLoad <= maxElms);
  // At a minimum, compaction is required.  If the load factor would be >0.5
  // even after compaction, grow instead, in order to avoid the possibility
  // of repeated compaction if the load factor were to hover at nearly 0.75.
  bool doGrow = (m_size > (maxElms >> 1));
#ifdef PEDANTIC
  if (m_tableMask > 0x7fffffffU && doGrow) {
    // If the hashtable is at its maximum size, we cannot grow
    doGrow = false;
    // Check if compaction would actually make room for at least one new
    // element. If not, raise an error.
    if (m_size >= 0x7fffffffU) {
      raise_error("Cannot grow an array with 2^31 - 1 elements");
      return;
    }
  }
#endif
  if (doGrow) {
    grow();
  } else {
    compact();
  }
}

void HphpArray::grow() {
  ASSERT(m_tableMask <= 0x7fffffffU);
  uint32 oldMask = m_tableMask;
  m_tableMask = (uint)(size_t(m_tableMask) + size_t(m_tableMask) + size_t(1));
  size_t tableSize = computeTableSize(m_tableMask);
  size_t maxElms = computeMaxElms(m_tableMask);
  reallocData(maxElms, tableSize, oldMask);

  // All the elements have been copied and their offsets from the base are
  // still the same, so we just need to build the new hash table.
  initHash(m_hash, tableSize);
#ifdef DEBUG
  // Wait to set m_hLoad to m_size until after rebuilding is complete,
  // in order to maintain invariants in findForNewInsert().
  m_hLoad = 0;
#else
  m_hLoad = m_size;
#endif

  if (m_size > 0) {
    Elm* elms = m_data;
    for (ElmInd pos = 0; pos <= m_lastE; ++pos) {
      Elm* e = &elms[pos];
      if (e->data.m_type == KindOfTombstone) {
        continue;
      }
      ElmInd* ei = findForNewInsert(e->hasIntKey() ? e->ikey : e->hash);
      *ei = pos;
    }
#ifdef DEBUG
    m_hLoad = m_size;
#endif
  }
}

void HphpArray::compact(bool renumber /* = false */) {
  struct ElmKey {
    int32       hash;
    union {
      int64 ikey;
      StringData* key;
    };
  };
  ElmKey mPos;
  if (m_pos != ArrayData::invalid_index) {
    // Cache key for element associated with m_pos in order to update m_pos
    // below.
    ASSERT(m_pos <= ssize_t(m_lastE));
    Elm* e = &(m_data[(ElmInd)m_pos]);
    mPos.hash = e->hasIntKey() ? 0 : e->hash;
    mPos.key = e->key;
  } else {
    // Silence compiler warnings.
    mPos.hash = 0;
    mPos.key = NULL;
  }
  int nsi = m_strongIterators.size();
  ElmKey* siKeys = NULL;
  if (nsi > 0) {
    Elm* elms = m_data;
    siKeys = (ElmKey*)malloc(nsi * sizeof(ElmKey));
    for (int i = 0; i < nsi; ++i) {
      ElmInd ei = (ElmInd)m_strongIterators.get(i)->pos;
      if (ei != ElmIndEmpty) {
        Elm* e = &elms[(ElmInd)m_strongIterators.get(i)->pos];
        siKeys[i].hash = e->hash;
        siKeys[i].key = e->key;
      }
    }
  }
  if (renumber) {
    m_nextKI = 0;
  }
  Elm* elms = m_data;
  size_t tableSize = computeTableSize(m_tableMask);
  initHash(m_hash, tableSize);
#ifdef DEBUG
  // Wait to set m_hLoad to m_size until after rebuilding is complete,
  // in order to maintain invariants in findForNewInsert().
  m_hLoad = 0;
#else
  m_hLoad = m_size;
#endif
  ElmInd frPos = 0;
  for (ElmInd toPos = 0; toPos < ElmInd(m_size); ++toPos) {
    Elm* frE = &elms[frPos];
    while (frE->data.m_type == KindOfTombstone) {
      ++frPos;
      ASSERT(frPos <= m_lastE);
      frE = &elms[frPos];
    }
    Elm* toE = &elms[toPos];
    if (toE != frE) {
      memcpy((void*)toE, (void*)frE, sizeof(Elm));
    }
    if (renumber && !toE->hasStrKey()) {
      toE->ikey = m_nextKI;
      ++m_nextKI;
    }
    ElmInd* ie = findForNewInsert(toE->hasIntKey() ? toE->ikey : toE->hash);
    *ie = toPos;
    ++frPos;
  }
  m_lastE = m_size - 1;
#ifdef DEBUG
  m_hLoad = m_size;
#endif
  if (m_pos != ArrayData::invalid_index) {
    // Update m_pos, now that compaction is complete.
    if (mPos.hash) {
      m_pos = ssize_t(find(mPos.key->data(), mPos.key->size(), mPos.hash));
    } else {
      m_pos = ssize_t(find(mPos.ikey));
    }
  }
  if (nsi > 0) {
    // Update strong iterators, now that compaction is complete.
    for (int i = 0; i < nsi; ++i) {
      ssize_t* siPos = &m_strongIterators.get(i)->pos;
      if (*siPos != ArrayData::invalid_index) {
        if (siKeys[i].hash) {
          *siPos = ssize_t(find(siKeys[i].key->data(),
                                siKeys[i].key->size(),
                                siKeys[i].hash));
        } else {
          *siPos = ssize_t(find(siKeys[i].ikey));
        }
      }
    }
    free(siKeys);
  }
}

#define ELEMENT_CONSTRUCT(fr, to) \
  if (fr->m_type == KindOfRef) fr = fr->m_data.ptv; \
  TV_DUP_CELL_NC(fr, to); \

bool HphpArray::nextInsert(CVarRef data) {
  if (UNLIKELY(m_nextKI < 0)) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return false;
  }
  resizeIfNeeded();
  int64 ki = m_nextKI;
  // The check above enforces an invariant that allows us to always
  // know that m_nextKI is not present in the array, so it is safe
  // to use findForNewInsert()
  ElmInd* ei = findForNewInsert(ki);
  ASSERT(!validElmInd(*ei));
  // Allocate a new element.
  Elm* e = allocElm(ei);
  ASSERT(e != NULL);
  initElm(e, ki, NULL, data);
  // Update next free element.
  ++m_nextKI;
  return true;
}

void HphpArray::nextInsertRef(CVarRef data) {
  if (UNLIKELY(m_nextKI < 0)) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return;
  }
  resizeIfNeeded();
  int64 ki = m_nextKI;
  // The check above enforces an invariant that allows us to always
  // know that m_nextKI is not present in the array, so it is safe
  // to use findForNewInsert()
  ElmInd* ei = findForNewInsert(ki);
  ASSERT(!validElmInd(*ei));
  // Allocate a new element.
  Elm* e = allocElm(ei);
  ASSERT(e != NULL);
  initElm(e, ki, NULL, data, true /*byRef*/);
  // Update next free element.
  ++m_nextKI;
}

void HphpArray::nextInsertWithRef(CVarRef data) {
  resizeIfNeeded();
  int64 ki = m_nextKI;
  ElmInd* ei = findForInsert(ki);
  ASSERT(!validElmInd(*ei));

  // Allocate a new element.
  Elm* e = allocElm(ei);
  TV_WRITE_NULL(&e->data);
  tvAsVariant(&e->data).setWithRef(data);
  // Set key.
  e->setIntKey(ki);
  // Update next free element.
  ++m_nextKI;
}

void HphpArray::addLvalImpl(int64 ki, Variant** pDest) {
  ASSERT(pDest != NULL);
  ElmInd* ei = findForInsert(ki);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    TypedValue* tv = &e->data;
    *pDest = &tvAsVariant(tv);
    return;
  }

  Elm* e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(ki);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }

  TV_WRITE_NULL(&e->data);
  e->setIntKey(ki);
  *pDest = &(tvAsVariant(&e->data));

  if (ki >= m_nextKI && m_nextKI >= 0) {
    m_nextKI = ki + 1;
  }
}

void HphpArray::addLvalImpl(StringData* key, int64 h, Variant** pDest) {
  ASSERT(key != NULL && pDest != NULL);
  ElmInd* ei = findForInsert(key->data(), key->size(), h);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    TypedValue* tv;
    tv = &e->data;
    *pDest = &tvAsVariant(tv);
    return;
  }

  Elm* e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(h);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }
  // Initialize element to null and store the address of the element into
  // *pDest.
  TV_WRITE_NULL(&e->data);
  // Set key. Do this after writing to e->data so _count is not overwritten
  e->setStrKey(key, h);
  e->key->incRefCount();
  *pDest = &(tvAsVariant(&e->data));
}

inline void HphpArray::addVal(int64 ki, CVarRef data) {
  ElmInd* ei = findForInsert(ki);
  Elm* e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(ki);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }
  TypedValue* fr = (TypedValue*)(&data);
  TypedValue* to = (TypedValue*)(&e->data);
  ELEMENT_CONSTRUCT(fr, to);
  e->setIntKey(ki);

  if (ki >= m_nextKI && m_nextKI >= 0) {
    m_nextKI = ki + 1;
  }
}

inline void HphpArray::addVal(StringData* key, CVarRef data) {
  strhash_t h = key->hash();
  ElmInd* ei = findForInsert(key->data(), key->size(), h);
  Elm *e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(h);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }

  // Set the element
  TypedValue* to = (TypedValue*)(&e->data);
  TypedValue* fr = (TypedValue*)(&data);
  ELEMENT_CONSTRUCT(fr, to);
  // Set the key after data is written
  e->setStrKey(key, h);
  e->key->incRefCount();
}

inline void HphpArray::addValWithRef(int64 ki, CVarRef data) {
  resizeIfNeeded();
  ElmInd* ei = findForInsert(ki);
  if (validElmInd(*ei)) {
    return;
  }

  Elm* e = allocElm(ei);

  TV_WRITE_NULL(&e->data);
  tvAsVariant(&e->data).setWithRef(data);

  e->setIntKey(ki);

  if (ki >= m_nextKI) {
    m_nextKI = ki + 1;
  }
}

inline void HphpArray::addValWithRef(StringData* key, CVarRef data) {
  resizeIfNeeded();
  strhash_t h = key->hash();
  ElmInd* ei = findForInsert(key->data(), key->size(), h);
  if (validElmInd(*ei)) {
    return;
  }

  Elm* e = allocElm(ei);

  TV_WRITE_NULL(&e->data);
  tvAsVariant(&e->data).setWithRef(data);

  e->setStrKey(key, h);
  e->key->incRefCount();
}

void HphpArray::update(int64 ki, CVarRef data) {
  ElmInd* ei = findForInsert(ki);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    tvAsVariant(&e->data).assignValHelper(data);
    return;
  }
  allocNewElm(ei, ki, NULL, data);
  if (ki >= m_nextKI && m_nextKI >= 0) {
    m_nextKI = ki + 1;
  }
}

HOT_FUNC_VM
void HphpArray::update(StringData* key, CVarRef data) {
  strhash_t h = key->hash();
  ElmInd* ei = findForInsert(key->data(), key->size(), h);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    Variant* to;
    to = &tvAsVariant(&e->data);
    to->assignValHelper(data);
    return;
  }
  allocNewElm(ei, h, key, data);
}

void HphpArray::updateRef(int64 ki, CVarRef data) {
  ElmInd* ei = findForInsert(ki);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    tvAsVariant(&e->data).assignRefHelper(data);
    return;
  }
  allocNewElm(ei, ki, NULL, data, true /*byRef*/);
  if (ki >= m_nextKI && m_nextKI >= 0) {
    m_nextKI = ki + 1;
  }
}

void HphpArray::updateRef(StringData* key, CVarRef data) {
  strhash_t h = key->hash();
  ElmInd* ei = findForInsert(key->data(), key->size(), h);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    tvAsVariant(&e->data).assignRefHelper(data);
    return;
  }
  allocNewElm(ei, h, key, data, true /*byRef*/);
}

ArrayData* HphpArray::lval(int64 k, Variant*& ret, bool copy,
                           bool checkExist /* = false */) {
  if (!copy) {
    addLvalImpl(k, &ret);
    return NULL;
  }
  if (!checkExist) {
    HphpArray* a = copyImpl();
    a->addLvalImpl(k, &ret);
    return a;
  }
  ssize_t /*ElmInd*/ pos = find(k);
  if (pos != (ssize_t)ElmIndEmpty) {
    Elm* e = &m_data[pos];
    if (tvAsVariant(&e->data).isReferenced() ||
        tvAsVariant(&e->data).isObject()) {
      ret = &tvAsVariant(&e->data);
      return NULL;
    }
  }
  HphpArray* a = copyImpl();
  a->addLvalImpl(k, &ret);
  return a;
}

ArrayData* HphpArray::lval(litstr k, Variant*& ret, bool copy,
                           bool checkExist /* = false */) {
  String s(k, AttachLiteral);
  return HphpArray::lval(s, ret, copy, checkExist);
}

ArrayData* HphpArray::lval(CStrRef k, Variant*& ret, bool copy,
                           bool checkExist /* = false */) {
  StringData* key = k.get();
  strhash_t prehash = key->hash();
  if (!copy) {
    addLvalImpl(key, prehash, &ret);
    return NULL;
  }
  if (!checkExist) {
    HphpArray* a = copyImpl();
    a->addLvalImpl(key, prehash, &ret);
    return a;
  }
  ssize_t /*ElmInd*/ pos = find(key->data(), key->size(), prehash);
  if (pos != (ssize_t)ElmIndEmpty) {
    Elm* e = &m_data[pos];
    TypedValue* tv = &e->data;
    if (tvAsVariant(tv).isReferenced() ||
        tvAsVariant(tv).isObject()) {
      ret = &tvAsVariant(tv);
      return NULL;
    }
  }
  HphpArray* a = copyImpl();
  a->addLvalImpl(key, prehash, &ret);
  return a;
}

ArrayData* HphpArray::lval(CVarRef k, Variant*& ret, bool copy,
                           bool checkExist /* = false */) {
  if (isIntegerKey(k)) {
    return HphpArray::lval(k.toInt64(), ret, copy, checkExist);
  }
  return HphpArray::lval(k.toString(), ret, copy, checkExist);
}

ArrayData *HphpArray::lvalPtr(CStrRef k, Variant*& ret, bool copy,
                              bool create) {
  StringData* key = k.get();
  strhash_t prehash = key->hash();
  HphpArray* a = 0;
  HphpArray* t = this;
  if (copy) {
    a = t = copyImpl();
  }
  if (create) {
    t->addLvalImpl(key, prehash, &ret);
  } else {
    ssize_t /*ElmInd*/ pos = t->find(key->data(), key->size(), prehash);
    if (pos != (ssize_t)ElmIndEmpty) {
      Elm* e = &t->m_data[pos];
      ret = &tvAsVariant(&e->data);
    } else {
      ret = NULL;
    }
  }
  return a;
}

ArrayData *HphpArray::lvalPtr(int64 k, Variant*& ret, bool copy,
                              bool create) {
  HphpArray* a = 0;
  HphpArray* t = this;
  if (copy) {
    a = t = copyImpl();
  }

  if (create) {
    t->addLvalImpl(k, &ret);
  } else {
    ElmInd pos = t->find(k);
    if (pos != ElmIndEmpty) {
      Elm* e = &t->m_data[pos];
      ret = &tvAsVariant(&e->data);
    } else {
      ret = NULL;
    }
  }
  return a;
}

ArrayData* HphpArray::lvalNew(Variant*& ret, bool copy) {
  TypedValue* tv;
  ArrayData* a = nvNew(tv, copy);
  if (tv == NULL) {
    ret = &(Variant::lvalBlackHole());
  } else {
    ret = &tvAsVariant(tv);
  }
  return a;
}

ArrayData* HphpArray::set(int64 k, CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->update(k, v);
    return a;
  }
  update(k, v);
  return NULL;
}

ArrayData* HphpArray::set(CStrRef k, CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->update(k.get(), v);
    return a;
  }
  update(k.get(), v);
  return NULL;
}

ArrayData* HphpArray::set(CVarRef k, CVarRef v, bool copy) {
  if (isIntegerKey(k)) {
    if (copy) {
      HphpArray* a = copyImpl();
      a->update(k.toInt64(), v);
      return a;
    }
    update(k.toInt64(), v);
    return NULL;
  }
  StringData* sd = k.getStringData();
  if (copy) {
    HphpArray* a = copyImpl();
    a->update(sd, v);
    return a;
  }
  update(sd, v);
  return NULL;
}

ArrayData* HphpArray::setRef(int64 k, CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->updateRef(k, v);
    return a;
  }
  updateRef(k, v);
  return NULL;
}

ArrayData* HphpArray::setRef(CStrRef k, CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->updateRef(k.get(), v);
    return a;
  }
  updateRef(k.get(), v);
  return NULL;
}

ArrayData* HphpArray::setRef(CVarRef k, CVarRef v, bool copy) {
  if (isIntegerKey(k)) {
    if (copy) {
      HphpArray* a = copyImpl();
      a->updateRef(k.toInt64(), v);
      return a;
    }
    updateRef(k.toInt64(), v);
    return NULL;
  }
  StringData* sd = k.getStringData();
  if (copy) {
    HphpArray* a = copyImpl();
    a->updateRef(sd, v);
    return a;
  }
  updateRef(sd, v);
  return NULL;
}

ArrayData* HphpArray::add(int64 k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  if (copy) {
    HphpArray* result = copyImpl();
    result->addVal(k, v);
    return result;
  }
  addVal(k, v);
  return NULL;
}

ArrayData* HphpArray::add(CStrRef k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  if (copy) {
    HphpArray* result = copyImpl();
    result->addVal(k.get(), v);
    return result;
  }
  addVal(k.get(), v);
  return NULL;
}

ArrayData* HphpArray::add(CVarRef k, CVarRef v, bool copy) {
  ASSERT(!exists(k));
  if (isIntegerKey(k)) {
    return HphpArray::add(k.toInt64(), v, copy);
  }
  return HphpArray::add(k.toString(), v, copy);
}

ArrayData* HphpArray::addLval(int64 k, Variant*& ret, bool copy) {
  ASSERT(!exists(k));
  if (copy) {
    HphpArray* result = copyImpl();
    result->addLvalImpl(k, &ret);
    return result;
  }
  addLvalImpl(k, &ret);
  return NULL;
}

ArrayData* HphpArray::addLval(CStrRef k, Variant*& ret, bool copy) {
  ASSERT(!exists(k));
  if (copy) {
    HphpArray* result = copyImpl();
    result->addLvalImpl(k.get(), k->hash(), &ret);
    return result;
  }
  addLvalImpl(k.get(), k->hash(), &ret);
  return NULL;
}

ArrayData* HphpArray::addLval(CVarRef k, Variant*& ret, bool copy) {
  ASSERT(!exists(k));
  if (copy) {
    HphpArray* a = copyImpl();
    if (isIntegerKey(k)) {
      a->addLvalImpl(k.toInt64(), &ret);
    } else {
      StringData* sd = k.getStringData();
      a->addLvalImpl(sd, sd->hash(), &ret);
    }
    return a;
  }
  if (isIntegerKey(k)) {
    addLvalImpl(k.toInt64(), &ret);
  } else {
    StringData* sd = k.getStringData();
    addLvalImpl(sd, sd->hash(), &ret);
  }
  return NULL;
}

//=============================================================================
// Delete.

void HphpArray::erase(ElmInd* ei, bool updateNext /* = false */) {
  ElmInd pos = *ei;
  if (!validElmInd(pos)) {
    return;
  }

  Elm* elms = m_data;

  bool nextElementUnsetInsideForeachByReference = false;
  int nsi = m_strongIterators.size();
  ElmInd eINext = ElmIndTombstone;
  for (int i = 0; i < nsi; ++i) {
    if (m_strongIterators.get(i)->pos == ssize_t(pos)) {
      nextElementUnsetInsideForeachByReference = true;
      if (eINext == ElmIndTombstone) {
        // eINext will actually be used, so properly initialize it with the
        // next element past pos, or ElmIndEmpty if pos is the last element.
        eINext = nextElm(elms, pos);
        if (eINext == ElmIndEmpty) {
          // Record that there is a strong iterator out there that is past the
          // end.
          m_siPastEnd = true;
        }
      }
      m_strongIterators.get(i)->pos = ssize_t(eINext);
    }
  }

  // If the internal pointer points to this element, advance it.
  if (m_pos == ssize_t(pos)) {
    if (eINext == ElmIndTombstone) {
      eINext = nextElm(elms, pos);
    }
    m_pos = ssize_t(eINext);
  }

  Elm* e = &elms[pos];
  // Free the value if necessary and mark it as a tombstone.
  TypedValue* tv = &e->data;
  tvRefcountedDecRef(tv);
  tv->m_type = KindOfTombstone;
  // Free the key if necessary, and clear the h and key fields in order to
  // increase the chances that subsequent searches will quickly/safely fail
  // when encountering tombstones, even though checking for KindOfTombstone is
  // the last validation step during search.
  if (e->hasStrKey()) {
    if (e->key->decRefCount() == 0) {
      e->key->release();
    }
    e->setIntKey(0);
  } else {
    // Match PHP 5.3.1 semantics
    // Hacky: don't removed the unsigned cast, else g++ can optimize away
    // the check for == 0x7fff..., since there is no signed int k
    // for which k-1 == 0x7fff...
    if ((uint64_t)e->ikey == (uint64_t)m_nextKI-1
          && (e->ikey == 0x7fffffffffffffffLL || updateNext)) {
      --m_nextKI;
    }
  }
  --m_size;
  // If this element was last, adjust m_lastE.
  if (pos == m_lastE) {
    do {
      --m_lastE;
    } while (m_lastE >= 0 && elms[m_lastE].data.m_type == KindOfTombstone);
  }
  // Mark the hash entry as "deleted".
  *ei = ElmIndTombstone;
  ASSERT(m_lastE == ElmIndEmpty ||
         uint32(m_lastE)+1 <= computeMaxElms(m_tableMask));
  ASSERT(m_hLoad <= computeMaxElms(m_tableMask));
  if (m_size < (uint32_t)((m_lastE+1) >> 1)) {
    // Compact in order to keep elms from being overly sparse.
    compact();
  }

  if (nextElementUnsetInsideForeachByReference) {
    if (RuntimeOption::EnableHipHopErrors) {
      raise_warning("The next element was unset inside foreach by reference. "
                    "This may lead to unexpeced results.");
    }
  }
}

ArrayData* HphpArray::remove(int64 k, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->erase(a->findForInsert(k));
    return a;
  }
  erase(findForInsert(k));
  return NULL;
}

ArrayData* HphpArray::remove(CStrRef k, bool copy) {
  strhash_t prehash = k->hash();
  if (copy) {
    HphpArray* a = copyImpl();
    a->erase(a->findForInsert(k.data(), k.size(), prehash));
    return a;
  }
  erase(findForInsert(k.data(), k.size(), prehash));
  return NULL;
}

ArrayData* HphpArray::remove(CVarRef k, bool copy) {
  if (isIntegerKey(k)) {
    if (copy) {
      HphpArray* a = copyImpl();
      a->erase(a->findForInsert(k.toInt64()));
      return a;
    }
    erase(findForInsert(k.toInt64()));
    return NULL;
  } else {
    StringData* key = k.getStringData();
    strhash_t prehash = key->hash();
    if (copy) {
      HphpArray* a = copyImpl();
      a->erase(a->findForInsert(key->data(), key->size(), prehash));
      return a;
    }
    erase(findForInsert(key->data(), key->size(), prehash));
    return NULL;
  }
}

ArrayData* HphpArray::copy() const {
  return copyImpl();
}

ArrayData* HphpArray::copyWithStrongIterators() const {
  HphpArray* copied = copyImpl();
  // Transfer strong iterators
  if (!m_strongIterators.empty()) {
    // Copy over all of the strong iterators, and update the iterators
    // to point to the new array
    for (int k = 0; k < m_strongIterators.size(); ++k) {
      FullPos* fp = m_strongIterators.get(k);
      fp->container = copied;
      copied->m_strongIterators.push(fp);
    }
    // Copy flags to new array
    copied->m_siPastEnd = m_siPastEnd;
    // Clear the strong iterator list and flags from the original array
    HphpArray* src = const_cast<HphpArray*>(this);
    src->m_strongIterators.clear();
    src->m_siPastEnd = 0;
  }
  return copied;
}

//=============================================================================
// non-variant interface

TypedValue* HphpArray::nvGetCell(int64 ki, bool error /* = false */) const {
  ElmInd pos = find(ki);
  if (LIKELY(pos != ElmIndEmpty)) {
    Elm* e = &m_data[pos];
    TypedValue* tv = &e->data;
    if (tv->m_type != KindOfRef) {
      return tv;
    } else {
      return tv->m_data.ptv;
    }
  }
  if (error) {
    raise_notice("Undefined index: %lld", ki);
  }
  return NULL;
}

inline TypedValue*
HphpArray::nvGetCell(const StringData* k, bool error /* = false */) const {
  ElmInd pos = find(k->data(), k->size(), k->hash());
  if (LIKELY(pos != ElmIndEmpty)) {
    Elm* e = &m_data[pos];
    TypedValue* tv = &e->data;
    if (tv->m_type < KindOfRef) {
      return tv;
    }
    if (LIKELY(tv->m_type == KindOfRef)) {
      return tv->m_data.ptv;
    }
  }
  if (error) {
    raise_notice("Undefined index: %s", k->data());
  }
  return NULL;
}

TypedValue* HphpArray::nvGet(int64 ki) const {
  ElmInd pos = find(ki);
  if (LIKELY(pos != ElmIndEmpty)) {
    Elm* e = &m_data[pos];
    return &e->data;
  }
  return NULL;
}

TypedValue*
HphpArray::nvGet(const StringData* k) const {
  ElmInd pos = find(k->data(), k->size(), k->hash());
  if (LIKELY(pos != ElmIndEmpty)) {
    Elm* e = &m_data[pos];
    return &e->data;
  }
  return NULL;
}

inline ArrayData* HphpArray::nvSet(int64 ki, int64 vi, bool copy) {
  HphpArray* a = this;
  ArrayData* retval = NULL;
  if (copy) {
    retval = a = copyImpl();
  }
  a->nvUpdate(ki, vi);
  return retval;
}

ArrayData* HphpArray::nvSet(int64 ki, const TypedValue* v, bool copy) {
  HphpArray* a = this;
  ArrayData* retval = NULL;
  if (copy) {
    retval = a = copyImpl();
  }
  a->update(ki, tvAsCVarRef(v));
  return retval;
}

ArrayData* HphpArray::nvSet(StringData* k, const TypedValue* v, bool copy) {
  HphpArray* a = this;
  ArrayData* retval = NULL;
  if (copy) {
    retval = a = copyImpl();
  }
  a->update(k, tvAsCVarRef(v));
  return retval;
}

void HphpArray::nvBind(int64 ki, const TypedValue* v) {
  updateRef(ki, tvAsCVarRef(v));
}

void HphpArray::nvBind(StringData* k, const TypedValue* v) {
  updateRef(k, tvAsCVarRef(v));
}

ArrayData* HphpArray::nvAppend(const TypedValue* v, bool copy) {
  HphpArray* a = this;
  ArrayData* retval = NULL;
  if (copy) {
    retval = a = copyImpl();
  }
  a->nextInsert(tvAsCVarRef(v));
  return retval;
}

void HphpArray::nvAppendWithRef(const TypedValue* v) {
  nextInsertWithRef(tvAsCVarRef(v));
}

ArrayData* HphpArray::nvNew(TypedValue*& ret, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    if (UNLIKELY(!a->nextInsert(null))) {
      ret = NULL;
      return a;
    }
    ASSERT(a->m_lastE != ElmIndEmpty);
    ssize_t lastE = (ssize_t)a->m_lastE;
    Elm* aElms = a->m_data;
    ret = &aElms[lastE].data;
    return a;
  }
  if (UNLIKELY(!nextInsert(null))) {
    ret = NULL;
    return NULL;
  }
  ASSERT(m_lastE != ElmIndEmpty);
  ssize_t lastE = (ssize_t)m_lastE;
  ret = &m_data[lastE].data;
  return NULL;
}

TypedValue* HphpArray::nvGetValueRef(ssize_t pos) {
  ASSERT(pos != ArrayData::invalid_index);
  Elm* e = &m_data[/*(ElmInd)*/pos];
  ASSERT(e->data.m_type != KindOfTombstone);
  return &e->data;
}

// nvGetKey does not touch out->_count, so can be used
// for inner or outer cells.
void HphpArray::nvGetKey(TypedValue* out, ssize_t pos) {
  ASSERT(pos != ArrayData::invalid_index);
  ASSERT(m_data[pos].data.m_type != KindOfTombstone);
  Elm* e = &m_data[/*(ElmInd)*/pos];
  if (e->hasIntKey()) {
    out->m_data.num = e->ikey;
    out->m_type = KindOfInt64;
    return;
  }
  out->m_data.pstr = e->key;
  out->m_type = KindOfString;
  e->key->incRefCount();
}

bool HphpArray::nvUpdate(int64 ki, int64 vi) {
  ElmInd* ei = findForInsert(ki);
  if (validElmInd(*ei)) {
    Elm* e = &m_data[*ei];
    TypedValue* to = (TypedValue*)(&e->data);
    if (to->m_type == KindOfRef) to = to->m_data.ptv;
    DataType oldType = to->m_type;
    uint64_t oldDatum = to->m_data.num;
    if (IS_REFCOUNTED_TYPE(oldType)) {
      tvDecRefHelper(oldType, oldDatum);
    }
    to->m_data.num = vi;
    to->m_type = KindOfInt64;
    return true;
  }
  Elm* e = allocElm(ei);
  if (UNLIKELY(e == NULL)) {
    resize();
    ei = findForNewInsert(ki);
    e = allocElm(ei);
    ASSERT(e != NULL);
  }
  TypedValue* to = (TypedValue*)(&e->data);
  to->m_data.num = vi;
  to->m_type = KindOfInt64;
  e->setIntKey(ki);

  if (ki >= m_nextKI && m_nextKI >= 0) {
    m_nextKI = ki + 1;
  }

  return true;
}

/*
 * Insert a new element with index k in to the array,
 * doing nothing and returning false if the element
 * already exists.
 */
bool HphpArray::nvInsert(StringData *k, TypedValue *data) {
  strhash_t h = k->hash();
  ElmInd* ei = findForInsert(k->data(), k->size(), h);
  if (validElmInd(*ei)) {
    return false;
  }
  allocNewElm(ei, h, k, tvAsVariant(data));
  return true;
}

ArrayData* HphpArray::nonSmartCopy() const {
  return copyImpl(new HphpArray(kNonSmartCopy));
}

HphpArray* HphpArray::copyImpl() const {
  return copyImpl(NEW(HphpArray)(kSmartCopy));
}

HphpArray* HphpArray::copyImpl(HphpArray* target) const {
  target->m_pos = m_pos;
  target->m_data = NULL;
  target->m_nextKI = m_nextKI;
  target->m_tableMask = m_tableMask;
  target->m_size = m_size;
  target->m_hLoad = m_hLoad;
  target->m_lastE = m_lastE;
  target->m_siPastEnd = false;
  size_t tableSize = computeTableSize(m_tableMask);
  size_t maxElms = computeMaxElms(m_tableMask);
  target->allocData(maxElms, tableSize);
  // Copy the hash.
  memcpy(target->m_hash, m_hash, tableSize * sizeof(ElmInd));
  // Copy the elements and bump up refcounts as needed.
  if (m_size > 0) {
    Elm* elms = m_data;
    Elm* targetElms = target->m_data;
    ssize_t lastE = (ssize_t)m_lastE;
    for (ssize_t /*ElmInd*/ pos = 0; pos <= lastE; ++pos) {
      Elm* e = &elms[pos];
      Elm* te = &targetElms[pos];
      if (e->data.m_type != KindOfTombstone) {
        te->hash = e->hash;
        te->key = e->key;
        TypedValue* fr;
        if (te->hasStrKey()) {
          fr = &e->data;
          te->key->incRefCount();
        } else {
          fr = &e->data;
        }
        TypedValue* to = &te->data;
        TV_DUP_FLATTEN_VARS(fr, to, this);
        te->hash = e->hash;
      } else {
        // Tombstone.
        te->setIntKey(0);
        te->data.m_type = KindOfTombstone;
      }
    }
    // It's possible that there were indirect elements at the end that were
    // converted to tombstones, so check if we should adjust target->m_lastE
    while (target->m_lastE >= 0) {
      Elm* te = &targetElms[target->m_lastE];
      if (te->data.m_type != KindOfTombstone) {
        break;
      }
      --(target->m_lastE);
    }
    // If the element density dropped below 50% due to indirect elements
    // being converted into tombstones, we should do a compaction
    if (target->m_size < (uint32_t)((target->m_lastE+1) >> 1)) {
      target->compact();
    }
  }
  return target;
}

ArrayData* HphpArray::append(CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->nextInsert(v);
    return a;
  }
  nextInsert(v);
  return NULL;
}

ArrayData* HphpArray::appendRef(CVarRef v, bool copy) {
  if (copy) {
    HphpArray* a = copyImpl();
    a->nextInsertRef(v);
    return a;
  }
  nextInsertRef(v);
  return NULL;
}

ArrayData *HphpArray::appendWithRef(CVarRef v, bool copy) {
  if (copy) {
    HphpArray *a = copyImpl();
    a->nextInsertWithRef(v);
    return a;
  }
  nextInsertWithRef(v);
  return NULL;
}

ArrayData* HphpArray::append(const ArrayData* elems, ArrayOp op, bool copy) {
  HphpArray* a = this;
  HphpArray* result = NULL;
  if (copy) {
    result = a = copyImpl();
  }

  if (op == Plus) {
    for (ArrayIter it(elems); !it.end(); it.next()) {
      Variant key = it.first();
      CVarRef value = it.secondRef();
      if (key.isNumeric()) {
        a->addValWithRef(key.toInt64(), value);
      } else {
        a->addValWithRef(key.getStringData(), value);
      }
    }
  } else {
    ASSERT(op == Merge);
    for (ArrayIter it(elems); !it.end(); it.next()) {
      Variant key = it.first();
      CVarRef value = it.secondRef();
      if (key.isNumeric()) {
        a->nextInsertWithRef(value);
      } else {
        Variant *p;
        StringData *sd = key.getStringData();
        a->addLvalImpl(sd, sd->hash(), &p);
        p->setWithRef(value);
      }
    }
  }
  return result;
}

ArrayData* HphpArray::pop(Variant& value) {
  HphpArray* a = this;
  HphpArray* result = NULL;
  if (getCount() > 1) {
    result = a = copyImpl();
  }
  Elm* elms = a->m_data;
  ElmInd pos = a->HphpArray::iter_end();
  if (validElmInd(pos)) {
    Elm* e = &elms[pos];
    ASSERT(e->data.m_type != KindOfTombstone);
    value = tvAsCVarRef(&e->data);
    ElmInd* ei = e->hasStrKey()
        ? a->findForInsert(e->key->data(), e->key->size(), e->hash)
        : a->findForInsert(e->ikey);
    a->erase(ei, true);
  } else {
    value = null;
  }
  // To match PHP-like semantics, the pop operation resets the array's
  // internal iterator.
  a->m_pos = a->nextElm(elms, ElmIndEmpty);
  return result;
}

ArrayData* HphpArray::dequeue(Variant& value) {
  HphpArray* a = this;
  HphpArray* result = NULL;
  if (getCount() > 1) {
    result = a = copyImpl();
  }
  // To match PHP-like semantics, we invalidate all strong iterators when an
  // element is removed from the beginning of the array.
  if (!a->m_strongIterators.empty()) {
    a->freeStrongIterators();
  }
  Elm* elms = a->m_data;
  ElmInd pos = a->nextElm(elms, ElmIndEmpty);
  if (validElmInd(pos)) {
    Elm* e = &elms[pos];
    value = tvAsCVarRef(&e->data);
    a->erase(e->hasStrKey() ?
             a->findForInsert(e->key->data(), e->key->size(), e->hash) :
             a->findForInsert(e->ikey));
    a->compact(true);
  } else {
    value = null;
  }
  // To match PHP-like semantics, the dequeue operation resets the array's
  // internal iterator
  a->m_pos = ssize_t(a->nextElm(elms, ElmIndEmpty));
  return result;
}

ArrayData* HphpArray::prepend(CVarRef v, bool copy) {
  HphpArray* a = this;
  HphpArray* result = NULL;
  if (copy) {
    result = a = copyImpl();
  }
  // To match PHP-like semantics, we invalidate all strong iterators when an
  // element is added to the beginning of the array.
  if (!a->m_strongIterators.empty()) {
    a->freeStrongIterators();
  }

  Elm* elms = a->m_data;
  if (a->m_lastE == 0 || elms[0].data.m_type != KindOfTombstone) {
    // Make sure there is room to insert an element.
    a->resizeIfNeeded();
    // Reload elms, in case resizeIfNeeded() had side effects.
    elms = a->m_data;
    // Move the existing elements to make element 0 available.
    memmove(&elms[1], &elms[0], (a->m_lastE+1) * sizeof(Elm));
    ++a->m_lastE;
  }
  // Prepend.
  Elm* e = &elms[0];

  TypedValue* fr = (TypedValue*)(&v);
  TypedValue* to = (TypedValue*)(&e->data);
  ELEMENT_CONSTRUCT(fr, to);

  e->setIntKey(0);
  ++a->m_size;

  // Renumber.
  a->compact(true);
  // To match PHP-like semantics, the prepend operation resets the array's
  // internal iterator
  a->m_pos = ssize_t(a->nextElm(elms, ElmIndEmpty));
  return result;
}

void HphpArray::renumber() {
  compact(true);
}

void HphpArray::onSetEvalScalar() {
  Elm* elms = m_data;
  for (ElmInd pos = 0; pos <= m_lastE; ++pos) {
    Elm* e = &elms[pos];
    if (e->data.m_type != KindOfTombstone) {
      StringData *key = e->key;
      if (e->hasStrKey() && !key->isStatic()) {
        StringData *skey = StringData::GetStaticString(key);
        if (key->decRefCount() == 0) {
          DELETE(StringData)(key);
        }
        e->key = skey;
      }
      tvAsVariant(&e->data).setEvalScalar();
    }
  }
}

void HphpArray::getFullPos(FullPos& fp) {
  ASSERT(fp.container == (ArrayData*)this);
  fp.pos = m_pos;
  if (fp.pos == ssize_t(ElmIndEmpty)) {
    // Record that there is a strong iterator out there that is past the end.
    m_siPastEnd = true;
  }
}

bool HphpArray::setFullPos(const FullPos& fp) {
  ASSERT(fp.container == (ArrayData*)this);
  if (fp.pos != ssize_t(ElmIndEmpty)) {
    m_pos = fp.pos;
    return true;
  }
  return false;
}

CVarRef HphpArray::currentRef() {
  ASSERT(m_pos != ArrayData::invalid_index);
  Elm* e = &m_data[(ElmInd)m_pos];
  ASSERT(e->data.m_type != KindOfTombstone);
  return tvAsCVarRef(&e->data);
}

CVarRef HphpArray::endRef() {
  ASSERT(m_lastE != ElmIndEmpty);
  ElmInd pos = m_lastE;
  Elm* e = &m_data[pos];
  return tvAsCVarRef(&e->data);
}

//=============================================================================
// Memory allocator methods.

void HphpArray::sweep() {
  if (m_allocMode == kMalloc) free(m_data);
  m_strongIterators.clear();
  // Its okay to skip calling adjustUsageStats() in the sweep phase.
}

//=============================================================================
// VM runtime support functions.
namespace VM {

// Helpers for array_setm.
template<typename Value>
inline ArrayData* nv_set_with_integer_check(HphpArray* arr, StringData* key,
                                            Value value, bool copy) {
  int64 lval;
  if (UNLIKELY(key->isStrictlyInteger(lval))) {
    return arr->nvSet(lval, value, copy);
  } else {
    return arr->nvSet(key, value, copy);
  }
}

template<typename Value>
ArrayData*
nvCheckedSet(HphpArray* ha, StringData* sd, Value value, bool copy) {
  return nv_set_with_integer_check<Value>(ha, sd, value, copy);
}

template<typename Value>
ArrayData*
nvCheckedSet(HphpArray* ha, int64 key, Value value, bool copy) {
  return ha->nvSet(key, value, copy);
}

void setmDecRef(int64 i) { /* nop */ }
void setmDecRef(TypedValue* tv) { tvRefcountedDecRef(tv); }
void setmDecRef(StringData* sd) { if (sd->decRefCount() == 0) sd->release(); }

static inline HphpArray*
array_mutate_pre(ArrayData* ad) {
  ASSERT(ad);
  return (HphpArray*)ad;
}

VarNR toVar(int64 i)        { return VarNR(i); }
Variant &toVar(TypedValue* tv) { return tvCellAsVariant(tv); }

template<bool CheckInt>
inline ArrayData* adSet(ArrayData* ad, int64 key, CVarRef val, bool copy) {
  return ad->set(key, val, copy);
}

template<bool CheckInt>
inline ArrayData* adSet(ArrayData* ad, const StringData* key,
                        CVarRef val, bool copy) {
  return ad->set(StrNR(key), val, copy);
}

template<>
inline ArrayData* adSet<true>(ArrayData* ad, const StringData* key,
                       CVarRef val, bool copy) {
  int64 lval;
  if (UNLIKELY(key->isStrictlyInteger(lval))) {
    return ad->set(lval, val, copy);
  }
  return ad->set(StrNR(key), val, copy);
}

static inline ArrayData*
array_mutate_post(Cell *cell, ArrayData* old, ArrayData* retval) {
  if (NULL == retval) {
    return old;
  }
  retval->incRefCount();
  // TODO: It would be great if there were nvSet() methods that didn't
  // bump up the refcount so that we didn't have to decrement it here
  if (old->decRefCount() == 0) old->release();
  if (cell) cell->m_data.parr = retval;
  return retval;
}

template<typename Key, typename Value,
         bool DecRefValue, bool CheckInt, bool DecRefKey>
static inline
ArrayData*
array_setm(TypedValue* cell, ArrayData* ad, Key key, Value value) {
  ArrayData* retval;
  bool copy = ad->getCount() > 1;
  if (LIKELY(IsHphpArray(ad))) {
    HphpArray* ha = array_mutate_pre(ad);
    // nvSet will decRef any old value that may have been overwritten
    // if appropriate
    retval = CheckInt ?
      nvCheckedSet(ha, key, value, copy)
      : ha->nvSet(key, value, copy);
  } else {
    retval = adSet<CheckInt>(ad, key, toVar(value), copy);
  }
  if (DecRefKey) setmDecRef(key);
  if (DecRefValue) setmDecRef(value);
  return array_mutate_post(cell, ad, retval);
}

template<typename Value, bool DecRefValue>
ArrayData*
array_append(TypedValue* cell, ArrayData* ad, Value v) {
  HphpArray* ha = array_mutate_pre(ad);
  bool copy = ha->getCount() > 1;
  ArrayData* retval = ha->nvAppend(v, copy);
  if (DecRefValue) setmDecRef(v);
  return array_mutate_post(cell, ad, retval);
}

/**
 * Unary integer keys.
 *    array_setm_ik1_iv --
 *       Integer value.
 *
 *    array_setm_ik1_v --
 *       Polymorphic value.
 *
 *    array_setm_ik1_v0 --
 *       Don't count the array's reference to the polymorphic value.
 */
ArrayData*
array_setm_ik1_iv(TypedValue* cell, ArrayData* ad, int64 key, int64 value) {
  return
    array_setm<int64, int64, false, false, false>(cell, ad, key, value);
}

ArrayData*
array_setm_ik1_v(TypedValue* cell, ArrayData* ad, int64 key,
                 TypedValue* value) {
  return
    array_setm<int64, TypedValue*, false, false, false>(cell, ad, key, value);
}

ArrayData*
array_setm_ik1_v0(TypedValue* cell, ArrayData* ad, int64 key,
                  TypedValue* value) {
  return
    array_setm<int64, TypedValue*, true, false, false>(cell, ad, key, value);
}

/**
 * String keys.
 *
 *    array_setm_sk1_v --
 *      $a[$keyOfTypeString] = <polymorphic value>;
 *
 *    array_setm_sk1_v0 --
 *      Like above, but don't count the new reference.
 *
 *    array_setm_s0k1_v --
 *    array_setm_s0k1_v0 --
 *      As above, but dont decRef the key
 *
 *    array_setm_s0k1nc_v --
 *    array_setm_s0k1nc_v0 --
 *       Dont decRef the key, and skip the check for
 *       whether the key is really an integer.
 */
ArrayData* array_setm_sk1_v(TypedValue* cell, ArrayData* ad, StringData* key,
                            TypedValue* value) {
  return array_setm<StringData*, TypedValue*, false, true, true>(
    cell, ad, key, value);
}

ArrayData* array_setm_sk1_v0(TypedValue* cell, ArrayData* ad, StringData* key,
                             TypedValue* value) {
  return array_setm<StringData*, TypedValue*, true, true, true>(
    cell, ad, key, value);
}

ArrayData* array_setm_s0k1_v(TypedValue* cell, ArrayData* ad, StringData* key,
                             TypedValue* value) {
  return array_setm<StringData*, TypedValue*, false, true, false>(
    cell, ad, key, value);
}

ArrayData* array_setm_s0k1_v0(TypedValue* cell, ArrayData* ad, StringData* key,
                              TypedValue* value) {
  return array_setm<StringData*, TypedValue*, true, true, false>(
    cell, ad, key, value);
}

ArrayData* array_setm_s0k1nc_v(TypedValue* cell, ArrayData* ad, StringData* key,
                               TypedValue* value) {
  return array_setm<StringData*, TypedValue*, false, false, false>(
    cell, ad, key, value);
}

ArrayData* array_setm_s0k1nc_v0(TypedValue* cell, ArrayData* ad,
                                StringData* key, TypedValue* value) {
  return array_setm<StringData*, TypedValue*, true, false, false>(
    cell, ad, key, value);
}

/**
 * Append.
 *
 *   array_setm_wk1_v --
 *      $a[] = <polymorphic value>
 *   array_setm_wk1_v0 --
 *      ... but don't count the reference to the new value.
 */
ArrayData* array_setm_wk1_v(TypedValue* cell, ArrayData* ad,
                            TypedValue* value) {
  return array_append<TypedValue*, false>(cell, ad, value);
}

ArrayData* array_setm_wk1_v0(TypedValue* cell, ArrayData* ad,
                             TypedValue* value) {
  return array_append<TypedValue*, true>(cell, ad, value);
}


// Helpers for getm and friends.
inline TypedValue* nv_get_cell_with_integer_check(HphpArray* arr,
                                                  StringData* key) {
  int64 lval;
  if (UNLIKELY(key->isStrictlyInteger(lval))) {
    return arr->nvGetCell(lval, true /* error */);
  } else {
    return arr->nvGetCell(key, true /* error */);
  }
}

static void elem(ArrayData* ad, int64 k, TypedValue* dest) {
  // dest is uninitialized, no need to destroy it
  CVarRef value = ad->get(k, true /* error */);
  TypedValue *tvPtr = value.getTypedAccessor();
  tvDup(tvPtr, dest);
}

static void elem(ArrayData* ad, StringData* sd, TypedValue* dest) {
  TypedValue* tvPtr;
  int64 iKey;
  if (UNLIKELY(sd->isStrictlyInteger(iKey))) {
    tvPtr = ad->get(iKey, true /* error */).getTypedAccessor();
  } else {
    String k(sd);
    tvPtr = ad->get(k, true /* error */).getTypedAccessor();
  }

  tvDup(tvPtr, dest);
}

/**
 * Array runtime helpers. For code-sharing purposes, all of these handle as
 * much ref-counting machinery as possible. They differ by -arity, type
 * signature, and necessity of various costly checks.
 *
 * They return the array that was just passed in as a convenience to
 * callers, which may have "lost" the array in volatile registers before
 * calling.
 */
ArrayData*
array_getm_i(void* dptr, int64 key, TypedValue* out) {
  ASSERT(dptr);
  ArrayData* ad = (ArrayData*)dptr;
  if (UNLIKELY(!IsHphpArray(ad))) {
    elem(ad, key, out);
  } else {
    HphpArray *ha = (HphpArray*)dptr;
    TRACE(2, "array_getm_ik1: (%p) <- %p[%lld]\n", out, dptr, key);
    // Ref-counting the value is the translator's responsibility. We know out
    // pointed to uninitialized memory, so no need to dec it.
    TypedValue* ret = ha->nvGetCell(key, true /* error */);
    if (UNLIKELY(!ret)) {
      TV_WRITE_NULL(out);
    } else {
      tvDup(ret, out);
    }
  }
  return ad;
}

#define ARRAY_GETM_IMPL(ad, sd, out, body, drKey) do {    \
  if (UNLIKELY(!IsHphpArray(ad))) {                       \
    elem(ad, sd, out);                                    \
  } else {                                                \
    HphpArray* ha = (HphpArray*)(ad);                     \
    TypedValue* ret;                                      \
    ARRAY_GETM_TRACE();                                   \
    ret = (body);                                         \
    if (UNLIKELY(!ret)) {                                 \
      TV_WRITE_NULL(out);                                 \
    } else {                                              \
      tvDup((ret), (out));                                \
    }                                                     \
  }                                                       \
  if (drKey && sd->decRefCount() == 0) sd->release();     \
} while(0)                                                \


#define ARRAY_GETM_BODY(dptr, sd, out, body, drKey) do {  \
  ArrayData* ad = (ArrayData*)dptr;                       \
  ARRAY_GETM_IMPL(ad, sd, out, body, drKey);              \
  return ad;                                              \
} while(0)

#define ARRAY_GETM_TRACE() do {                           \
  TRACE(2, "%s: (%p) <- %p[\"%s\"@sd%p]\n", __FUNCTION__, \
        (out), (dptr), (sd)->data(), (sd));               \
} while(0)

/**
 * array_getm_s: conservative unary string key.
 */
ArrayData*
array_getm_s(void* dptr, StringData* sd, TypedValue* out) {
  ARRAY_GETM_BODY(dptr, sd, out, nv_get_cell_with_integer_check(ha, sd),
                  true /* drKey */);
}

/**
 * array_getm_s0: unary string key where we know there is no need
 * to decRef the key.
 */
ArrayData*
array_getm_s0(void* dptr, StringData* sd, TypedValue* out) {
  ARRAY_GETM_BODY(dptr, sd, out, nv_get_cell_with_integer_check(ha, sd),
                  false /* drKey */);
}

/**
 * array_getm_s_fast:
 * array_getm_s0:
 *
 *   array_getm_s[0] but without the integer check on the key
 */
ArrayData*
array_getm_s_fast(void* dptr, StringData* sd, TypedValue* out) {
  ARRAY_GETM_BODY(dptr, sd, out, ha->nvGetCell(sd, true /* error */),
                  true /* drKey */);
}

ArrayData*
array_getm_s0_fast(void* dptr, StringData* sd, TypedValue* out) {
  ARRAY_GETM_BODY(dptr, sd, out, ha->nvGetCell(sd, true /* error */),
                  false /* drKey */);
}
#undef ARRAY_GETM_TRACE

template<DataType keyType, bool decRefBase>
inline void non_array_getm(TypedValue* base, int64 key, TypedValue* out) {
  ASSERT(base->m_type != KindOfRef);
  TypedValue keyTV;
  keyTV._count = 0;
  keyTV.m_type = keyType;
  keyTV.m_data.num = key;
  VMExecutionContext::getElem(base, &keyTV, out);
  if (decRefBase) {
    tvRefcountedDecRef(base);
  }
  if (IS_REFCOUNTED_TYPE(keyType)) {
    tvDecRef(&keyTV);
  }
}

void
non_array_getm_i(TypedValue* base, int64 key, TypedValue* out) {
  non_array_getm<KindOfInt64, true>(base, key, out);
}

void
non_array_getm_s(TypedValue* base, StringData* key, TypedValue* out) {
  non_array_getm<KindOfString, true>(base, (intptr_t)key, out);
}

#define ARRAY_GETM_TRACE() do {                           \
  TRACE(2, "%s: (%p) <- %p[%lld][\"%s\"@sd%p]\n",         \
        __FUNCTION__, (out), (ad), (ik), (sd)->data(),    \
        (sd));                                            \
} while(0)

template <bool decRefKey>
inline void
array_getm_is_impl(ArrayData* ad, int64 ik, StringData* sd, TypedValue* out) {
  TypedValue* base2;

  if (UNLIKELY(!IsHphpArray(ad))) {
    base2 = (TypedValue*)&ad->get(ik, true);
  } else {
    base2 = static_cast<HphpArray*>(ad)->nvGetCell(ik, true);
  }

  if (UNLIKELY(base2 == NULL || base2->m_type != KindOfArray)) {
    if (base2 == NULL) {
      base2 = (TypedValue*)&init_null_variant;
    }
    non_array_getm<KindOfString, false>(base2, (int64)sd, out);
  } else {
    ad = base2->m_data.parr;
    ARRAY_GETM_IMPL(ad, sd, out, nv_get_cell_with_integer_check(ha, sd),
                    decRefKey);
  }
}

/**
 * array_getm_is will increment the refcount of the return value if
 * appropriate and it will decrement the refcount of the string key
 */
void
array_getm_is(ArrayData* ad, int64 ik, StringData* sd, TypedValue* out) {
  array_getm_is_impl<true>(ad, ik, sd, out);
}

/**
 * array_getm_is0 will increment the refcount of the return value if
 * appropriate
 */
void
array_getm_is0(ArrayData* ad, int64 ik, StringData* sd, TypedValue* out) {
  array_getm_is_impl<false>(ad, ik, sd, out);
}

#undef ARRAY_GETM_TRACE
#undef ARRAY_GETM_BODY

// issetm's DNA.
static inline bool
issetMUnary(const void* dptr, StringData* sd, bool decRefKey, bool checkInt) {
  const ArrayData* ad = (const ArrayData*)dptr;
  bool retval;
  const Variant *cell;
  if (UNLIKELY(!IsHphpArray(ad))) {
    int64 keyAsInt;
    if (checkInt && sd->isStrictlyInteger(keyAsInt)) {
      cell = &ad->get(keyAsInt);
    } else {
      cell = &ad->get(StrNR(sd));
    }
  } else {
    const HphpArray* ha = (const HphpArray*)dptr;
    int64 keyAsInt;

    TypedValue* c;
    if (checkInt && sd->isStrictlyInteger(keyAsInt)) {
      c = ha->nvGet(keyAsInt);
    } else {
      c = ha->nvGet(sd);
    }
    cell = &tvAsVariant(c);
  }

  retval = cell && !cell->isNull();
  TRACE(2, "issetMUnary: %p[\"%s\"@sd%p] -> %d\n",
        dptr, sd->data(), sd, retval);

  if (decRefKey && sd->decRefCount() == 0) sd->release();
  return retval;
}

uint64 array_issetm_s(const void* dptr, StringData* sd)
{ return issetMUnary(dptr, sd, true  /*decRefKey*/, true  /*checkInt*/); }
uint64 array_issetm_s0(const void* dptr, StringData* sd)
{ return issetMUnary(dptr, sd, false /*decRefKey*/, true  /*checkInt*/); }
uint64 array_issetm_s_fast(const void* dptr, StringData* sd)
{ return issetMUnary(dptr, sd, true  /*decRefKey*/, false /*checkInt*/); }
uint64 array_issetm_s0_fast(const void* dptr, StringData* sd)
{ return issetMUnary(dptr, sd, false /*decRefKey*/, false /*checkInt*/); }

uint64 array_issetm_i(const void* dptr, int64_t key) {
  ArrayData* ad = (ArrayData*)dptr;
  if (UNLIKELY(!IsHphpArray(ad))) {
    return ad->exists(key);
  }
  HphpArray* ha = (HphpArray*)dptr;
  TypedValue* ret = ha->nvGetCell(key, false /* error */);
  return ret && !tvAsCVarRef(ret).isNull();
}

ArrayData* array_add(ArrayData* a1, ArrayData* a2) {
  if (!a2->empty()) {
    if (a1->empty()) {
      if (a1->decRefCount() == 0) a1->release();
      return a2;
    }
    if (a1 != a2) {
      ArrayData *escalated = a1->append(a2, ArrayData::Plus,
                                        a1->getCount() > 1);
      if (escalated) {
        escalated->incRefCount();
        if (a2->decRefCount() == 0) a2->release();
        if (a1->decRefCount() == 0) a1->release();
        return escalated;
      }
    }
  }
  if (a2->decRefCount() == 0) a2->release();
  return a1;
}

/**
 * array_unsetm_s --
 * array_unsetm_s0 --
 *
 *   String-key removal. Might trigger copy-on-write. _s0 doesn't decref
 *   the key.
 */
static inline ArrayData* array_unsetm_s_common(ArrayData *ad, StringData* sd,
                                               bool decRef) {
  ArrayData* retval = 0;
  int64 lval;
  bool copy = ad->getCount() > 1;
  bool isInt = sd->isStrictlyInteger(lval);
  retval = isInt ? ad->remove(lval, copy) : ad->remove(*(String*)&sd, copy);
  if (decRef && sd->decRefCount() == 0) sd->release();
  return array_mutate_post(NULL, ad, retval);
}

ArrayData* array_unsetm_s0(ArrayData *ad, StringData* sd) {
  return array_unsetm_s_common(ad, sd, false);
}

ArrayData* array_unsetm_s(ArrayData *ad, StringData* sd) {
  return array_unsetm_s_common(ad, sd, true);
}

}

//=============================================================================

///////////////////////////////////////////////////////////////////////////////
}
