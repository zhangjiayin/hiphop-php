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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "assert.h"
#include "asm-x64.h"
#include "runtime/base/runtime_option.h"

namespace HPHP {
namespace x64 {

static void panic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  abort();
}

Address allocSlab(size_t size) {
  Address result = (Address)
    // XXX: ponder MAP_SHARED?
    mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (result == MAP_FAILED) {
    panic("%s:%d: (%s) map of %zu bytes failed (%s)\n",
          __FILE__, __LINE__, __func__, size, strerror(errno));
  }
  return result;
}

void freeSlab(Address addr, size_t size) {
  int result = munmap(addr, size);
  if (result != 0) {
    perror("freeSlab: munmap");
  }
}

void DataBlock::init() {
  base = frontier = allocSlab(size);
}

void DataBlock::free() {
  freeSlab(base, size);
  base = frontier = NULL;
  size = 0;
}

void DataBlock::init(Address start, size_t sz) {
  base = frontier = start;
  size = sz;
}

void DataBlock::makeExecable() {
  if (mprotect(base, size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
    panic("%s:%d (%s): mprotect @%p %zu bytes failed (%s)\n",
          __FILE__, __LINE__, __func__,
          base, (long)size, strerror(errno));
  }
}

void CodeBlock::initCodeBlock(size_t sz) {
  size = sz;
  init();
  makeExecable();
}

void CodeBlock::initCodeBlock(CodeAddress start, size_t sz) {
  base = frontier = start;
  size = sz;
  makeExecable();
}

void X64Assembler::init(size_t sz) {
  code.initCodeBlock(sz);
}

void X64Assembler::init(CodeAddress start, size_t sz) {
  code.initCodeBlock(start, sz);
}

} }
