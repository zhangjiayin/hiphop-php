/*
  +----------------------------------------------------------------------+
  | HipHop for PHP                                                       |
  +----------------------------------------------------------------------+
  | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
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

#pragma once


#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/bespoke/key-order.h"
#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/base/string-data.h"

namespace HPHP { namespace bespoke {

struct StructLayout;

/*
 * Hidden-class style layout for a dict/darray. Static string keys are stored
 * in the layout itself instead of in the array. The layout maps these keys to
 * to physical slots. Each array has space for all of its layout's slots.
 */
struct StructArray : public BespokeArray {
  static StructArray* MakeFromVanilla(ArrayData* ad,
                                      const StructLayout* layout);
  template<bool Static>
  static StructArray* MakeReserve(
      HeaderKind kind, bool legacy, const StructLayout* layout);

  static StructArray* MakeStructDArray(
      const StructLayout* layout, uint32_t size,
      const Slot* slots, const TypedValue* vals);
  static StructArray* MakeStructDict(
      const StructLayout* layout, uint32_t size,
      const Slot* slots, const TypedValue* vals);

  uint8_t sizeIndex() const;
  static size_t sizeFromLayout(const StructLayout*);

  static const StructArray* As(const ArrayData* ad);
  static StructArray* As(ArrayData* ad);

  static constexpr size_t kMaxKeyNum = KeyOrder::kMaxLen;
  static_assert(kMaxKeyNum <= std::numeric_limits<uint8_t>::max());

#define X(Return, Name, Args...) static Return Name(Args);
  BESPOKE_LAYOUT_FUNCTIONS(StructArray)
#undef X

private:
  static StructArray* MakeStructImpl(
      const StructLayout* layout, uint32_t size,
      const Slot* slots, const TypedValue* tvs, HeaderKind hk);

  const StructLayout* layout() const;

  size_t numFields() const;
  size_t typeOffset() const { return numFields(); }
  size_t valueOffsetInValueSize() const;
  const DataType* rawTypes() const;
  DataType* rawTypes();
  const Value* rawValues() const;
  Value* rawValues();
  const uint8_t* rawPositions() const;
  uint8_t* rawPositions();
  TypedValue typedValueUnchecked(Slot slot) const;

  ArrayData* escalateWithCapacity(size_t capacity, const char* reason) const;
  arr_lval elemImpl(StringData* k, bool throwOnMissing);
  StructArray* copy() const;
  void incRefValues();
  void decRefValues();

  void addNextSlot(Slot slot);
  void removeSlot(Slot slot);
  Slot getSlotInPos(size_t pos) const;
  bool checkInvariants() const;
};

/*
 * Layout data structure defining a hidden class. This data structure contains
 * two main pieces of data: a map from static string keys to physical slots,
 * and an array of field descriptors for each slot.
 *
 * Right now, the only data in the field descriptor is the field's string key.
 * However, in the future, we'll place two further constraints on fields which
 * will allow us to better-optimize JIT-ed code:
 *
 *  1. Type restrictions. Some fields will may only allow values of a certain
 *     data type (modulo countedness), saving us type checks on lookups.
 *
 *  2. "optional" vs. "required". Right now, all fields are optional. If we
 *     make some fields required, we can skip existence checks on lookup.
 */
struct StructLayout : public ConcreteLayout {
  struct Field { LowStringPtr key; };

  static LayoutIndex Index(uint8_t raw);
  static const StructLayout* GetLayout(const KeyOrder&, bool create);
  static const StructLayout* As(const Layout*);

  size_t numFields() const;
  size_t sizeIndex() const;
  Slot keySlot(const StringData* key) const;
  const Field& field(Slot slot) const;

  size_t typeOffset() const { return m_typeOff; }
  size_t valueOffset() const { return m_valueOff; }

private:
  // Callers must check whether the key is static before using one of these
  // wrapper types. The wrappers dispatch to the right hash/equal function.
  struct StaticKey { LowStringPtr key; };
  struct NonStaticKey { const StringData* key; };

  // Use heterogeneous lookup to optimize the lookup for static keys.
  struct Hash {
    using is_transparent = void;
    size_t operator()(const StaticKey& k) const { return k.key->hashStatic(); }
    size_t operator()(const NonStaticKey& k) const { return k.key->hash(); }
  };
  struct Equal {
    using is_transparent = void;
    bool operator()(const StaticKey& k1, const StaticKey& k2) const {
      return k1.key == k2.key;
    }
    bool operator()(const NonStaticKey& k1, const StaticKey& k2) const {
      return k1.key->same(k2.key);
    }
  };

  StructLayout(const KeyOrder&, const LayoutIndex&);

  size_t m_size_index;

  // Offsets of datatypes and values in a StructArray
  // from the end of the array header.
  size_t m_typeOff;
  size_t m_valueOff;

  folly::F14FastMap<StaticKey, Slot, Hash, Equal> m_key_to_slot;
  // Variable-size array field; must be last in this struct.
  Field m_fields[1];
};

}}
