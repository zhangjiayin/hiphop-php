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

#ifndef __HPHP_ARRAY_DATA_H__
#define __HPHP_ARRAY_DATA_H__

#include <runtime/base/util/countable.h>
#include <runtime/base/types.h>
#include <runtime/base/macros.h>
#include <util/pointer_list.h>
#include <climits>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

class SharedVariant;

/**
 * Base class/interface for all types of specialized array data.
 */
class ArrayData : public Countable {
 public:
  enum ArrayOp {
    Plus,
    Merge,
  };

  static const ssize_t invalid_index = -1;

  ArrayData() : m_size(-1), m_pos(0) {}
  ArrayData(const ArrayData *src) : m_pos(src->m_pos) {}
  virtual ~ArrayData();

  /**
   * Create a new ArrayData with specified array element(s).
   */
  static ArrayData *Create();
  static ArrayData *Create(CVarRef value);
  static ArrayData *Create(CVarRef name, CVarRef value);
  static ArrayData *CreateRef(CVarRef value);
  static ArrayData *CreateRef(CVarRef name, CVarRef value);

  /**
   * Type conversion functions. All other types are handled inside Array class.
   */
  Object toObject() const;

  /**
   * Array interface functions.
   *
   * 1. For functions that return ArrayData pointers, these are the ones that
   *    can potentially escalate into a different ArrayData type. Return NULL
   *    if no escalation is needed.
   *
   * 2. All functions with a "key" parameter are type-specialized.
   */

  /**
   * For SmartAllocator.
   */
  virtual void release() = 0;

  /**
   * Whether this array has any element.
   */
  bool empty() const {
    return size() == 0;
  }

  /**
   * Number of elements this array has.
   */
  ssize_t size() const {
    if (UNLIKELY((int)m_size) < 0) return vsize();
    return m_size;
  }

  /**
   * Number of elements this array has.
   */
  virtual ssize_t vsize() const = 0;

  /**
   * For ArrayIter to work. Get key or value at position "pos".
   */
  virtual Variant getKey(ssize_t pos) const = 0;
  virtual Variant getValue(ssize_t pos) const = 0;

  /**
   * getValueRef() gets a reference to value at position "pos".
   */
  virtual CVarRef getValueRef(ssize_t pos) const = 0;

  /*
   * Return true for array types that don't have COW semantics.
   */
  virtual bool noCopyOnWrite() const { return false; }

  /*
   * Specific derived class type querying operators.
   */
  virtual bool isVectorArray() const { return false; }
  virtual bool isSharedMap() const { return false; }

  /*
   * Returns whether or not this array contains "vector-like" data.
   * I.e. all the keys are contiguous increasing integers.
   */
  virtual bool isVectorData() const;

  virtual SharedVariant *getSharedVariant() const { return NULL; }

  /**
   * Whether or not this array has a referenced Variant or Object appearing
   * twice. This is mainly for APC to decide whether to serialize an array.
   * Also used for detecting whether there is serializable object in the tree.
   */
  bool hasInternalReference(PointerSet &seen,
                            bool detectSerializable = false) const;

  /**
   * Position-based iterations.
   */
  virtual Variant reset();
  virtual Variant prev();
  virtual Variant current() const;
  virtual Variant next();
  virtual Variant end();
  virtual Variant key() const;
  virtual Variant value(ssize_t &pos) const;
  virtual Variant each();

  bool isHead()            const { return m_pos == iter_begin(); }
  bool isTail()            const { return m_pos == iter_end(); }
  virtual bool isInvalid() const { return m_pos == invalid_index; }

  /**
   * Testing whether a key exists.
   */
  virtual bool exists(int64   k) const = 0;
  virtual bool exists(litstr  k) const = 0;
  virtual bool exists(CStrRef k) const = 0;
  virtual bool exists(CVarRef k) const = 0;

  /**
   * Getting value at specified key.
   */
  virtual CVarRef get(int64   k, bool error = false) const = 0;
  virtual CVarRef get(litstr  k, bool error = false) const = 0;
  virtual CVarRef get(CStrRef k, bool error = false) const = 0;
  virtual CVarRef get(CVarRef k, bool error = false) const = 0;

  /**
   * Get the numeric index for a key. Only these need to be
   * in ArrayData.
   */
  virtual ssize_t getIndex(int64 k) const = 0;
  virtual ssize_t getIndex(litstr k) const = 0;
  virtual ssize_t getIndex(CStrRef k) const = 0;
  virtual ssize_t getIndex(CVarRef k) const = 0;

  /**
   * Getting l-value (that Variant pointer) at specified key. Return NULL if
   * escalation is not needed, or an escalated array data.
   */
  virtual ArrayData *lval(int64   k, Variant *&ret, bool copy,
                          bool checkExist = false) = 0;
  virtual ArrayData *lval(litstr  k, Variant *&ret, bool copy,
                          bool checkExist = false) = 0;
  virtual ArrayData *lval(CStrRef k, Variant *&ret, bool copy,
                          bool checkExist = false) = 0;
  virtual ArrayData *lval(CVarRef k, Variant *&ret, bool copy,
                          bool checkExist = false) = 0;

  /**
   * Getting l-value (that Variant pointer) of a new element with the next
   * available integer key. Return NULL if escalation is not needed, or an
   * escalated array data. Note that adding a new element with the next
   * available integer key may fail, in which case ret is set to point to
   * the lval blackhole (see Variant::lvalBlackHole() for details).
   */
  virtual ArrayData *lvalNew(Variant *&ret, bool copy) = 0;

  /**
   * Helper functions used for getting a reference to elements of
   * the dynamic property array in ObjectData or the local cache array
   * in ShardMap.
   */
  virtual ArrayData *lvalPtr(CStrRef k, Variant *&ret, bool copy,
                             bool create);
  virtual ArrayData *lvalPtr(int64   k, Variant *&ret, bool copy,
                             bool create);

  /**
   * Setting a value at specified key. If "copy" is true, make a copy first
   * then set the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
  virtual ArrayData *set(int64   k, CVarRef v, bool copy) = 0;
  virtual ArrayData *set(CStrRef k, CVarRef v, bool copy) = 0;
  virtual ArrayData *set(CVarRef k, CVarRef v, bool copy) = 0;

  virtual ArrayData *setRef(int64   k, CVarRef v, bool copy) = 0;
  virtual ArrayData *setRef(CStrRef k, CVarRef v, bool copy) = 0;
  virtual ArrayData *setRef(CVarRef k, CVarRef v, bool copy) = 0;

  /**
   * The same as set(), but with the precondition that the key does
   * not already exist in this array.  (This is to allow more
   * efficient implementation of this case in some derived classes.)
   */
  virtual ArrayData *add(int64   k, CVarRef v, bool copy);
  virtual ArrayData *add(CStrRef k, CVarRef v, bool copy);
  virtual ArrayData *add(CVarRef k, CVarRef v, bool copy);

  /*
   * Same semantics as lval(), except with the precondition that the
   * key doesn't already exist in the array.
   */
  virtual ArrayData *addLval(int64   k, Variant *&ret, bool copy);
  virtual ArrayData *addLval(CStrRef k, Variant *&ret, bool copy);
  virtual ArrayData *addLval(CVarRef k, Variant *&ret, bool copy);

  /**
   * Remove a value at specified key. If "copy" is true, make a copy first
   * then remove the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
  virtual ArrayData *remove(int64   k, bool copy) = 0;
  virtual ArrayData *remove(CStrRef k, bool copy) = 0;
  virtual ArrayData *remove(CVarRef k, bool copy) = 0;

  /**
   * legacy overloads that are not used enough to justify optimizing
   */
  ArrayData *set(litstr  k, CVarRef v, bool copy);
  ArrayData *setRef(litstr  k, CVarRef v, bool copy);
  ArrayData *remove(litstr  k, bool copy);

  virtual ssize_t iter_begin() const;
  virtual ssize_t iter_end() const;
  virtual ssize_t iter_advance(ssize_t prev) const;
  virtual ssize_t iter_rewind(ssize_t prev) const;

  void newFullPos(FullPos &fp);
  void freeFullPos(FullPos &fp);
  virtual void getFullPos(FullPos &fp);
  virtual bool setFullPos(const FullPos &fp);
  virtual CVarRef currentRef();
  virtual CVarRef endRef();

  /**
   * Make a copy of myself.
   *
   * The nonSmartCopy() version means not to use the smart allocator.
   * Is only implemented for array types that need to be able to go
   * into the static array list.
   */
  virtual ArrayData *copy() const = 0;
  virtual ArrayData *copyWithStrongIterators() const;
  virtual ArrayData *nonSmartCopy() const;

  /**
   * Append a value to the array. If "copy" is true, make a copy first
   * then append the value. Return NULL if escalation is not needed, or an
   * escalated array data.
   */
  virtual ArrayData *append(CVarRef v, bool copy) = 0;
  virtual ArrayData *appendRef(CVarRef v, bool copy) = 0;

  /**
   * Similar to append(v, copy), with reference in v preserved.
   */
  virtual ArrayData *appendWithRef(CVarRef v, bool copy) = 0;

  /**
   * Implementing array appending and merging. If "copy" is true, make a copy
   * first then append/merge arrays. Return NULL if escalation is not needed,
   * or an escalated array data.
   */
  virtual ArrayData *append(const ArrayData *elems, ArrayOp op, bool copy) = 0;

  /**
   * Stack function: pop the last item and return it.
   */
  virtual ArrayData *pop(Variant &value);

  /**
   * Queue function: remove the 1st item and return it.
   */
  virtual ArrayData *dequeue(Variant &value);

  /**
   * Array function: prepend a new item.
   */
  virtual ArrayData *prepend(CVarRef v, bool copy) = 0;

  /**
   * Only map classes need this. Re-index all numeric keys to start from 0.
   */
  virtual void renumber() {}

  virtual void onSetEvalScalar() { ASSERT(false);}

  /**
   * Serialize this array. We could have made this virtual function to ask
   * sub-classes to implement it specifically, but since this is not a critical
   * function to optimize, we implement it in a generic way in this base class.
   * Then all the sudden we find out all Zend HashTable functions are similar
   * to implementing array functions in this base class than utilizing a type
   * specialized implementation, which is normally more optimized.
   */
  void serialize(VariableSerializer *serializer,
                 bool skipNestCheck = false) const;

  virtual void dump();
  virtual void dump(std::string &out);
  virtual void dump(std::ostream &os);

  /**
   * Comparisons. Similar to serialize(), we implemented it here generically.
   */
  int compare(const ArrayData *v2) const;
  bool equal(const ArrayData *v2, bool strict) const;

  void setPosition(ssize_t p) { m_pos = p; }

  virtual ArrayData *escalate(bool mutableIteration = false) const {
    return const_cast<ArrayData *>(this);
  }
  PointerList<FullPos> &getStrongIterators() {
    return m_strongIterators;
  }

  static ArrayData *GetScalarArray(ArrayData *arr,
                                   const StringData *key = NULL);
 protected:
  uint m_size;
  ssize_t m_pos;
  PointerList<FullPos> m_strongIterators;

  void freeStrongIterators();

 private:
  void serializeImpl(VariableSerializer *serializer) const;

 private:
  static void compileTimeAssertions() {
    CT_ASSERT(offsetof(ArrayData, _count) == FAST_REFCOUNT_OFFSET);
  }
};

///////////////////////////////////////////////////////////////////////////////
}

#endif // __HPHP_ARRAY_DATA_H__
