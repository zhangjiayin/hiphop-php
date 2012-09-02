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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <algorithm>
#include <runtime/base/util/string_buffer.h>
#include <util/alloc.h>
#include <runtime/base/file/file.h>
#include <runtime/base/zend/zend_functions.h>
#include <runtime/base/zend/utf8_decode.h>
#include <runtime/base/taint/taint_observer.h>
#include <runtime/ext/ext_json.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

StringBuffer::StringBuffer(int initialSize /* = 63 */)
  : m_initialSize(initialSize), m_maxBytes(kDefaultOutputLimit),
    m_size(initialSize), m_pos(0) {
  ASSERT(initialSize > 0);
  m_buffer = (char *)Util::safe_malloc(initialSize + 1);
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, dataIgnoreTaint());
}

StringBuffer::StringBuffer(const char *filename)
  : m_buffer(NULL), m_initialSize(1024), m_maxBytes(kDefaultOutputLimit),
    m_size(0), m_pos(0) {
  struct stat sb;
  if (stat(filename, &sb) == 0) {
    if (sb.st_size > m_maxBytes - 1) {
      std::ostringstream out;
      out << "file " << filename << " is too large";
      throw StringBufferLimitException(m_maxBytes,
                                       String(out.str().c_str()));
    }
    m_size = sb.st_size;
    m_buffer = (char *)Util::safe_malloc(m_size + 1);

    int fd = ::open(filename, O_RDONLY);
    if (fd != -1) {
      while (m_pos < m_size) {
        int buffer_size = m_size - m_pos;
        int len = ::read(fd, m_buffer + m_pos, buffer_size);
        if (len == -1 && errno == EINTR) continue;
        if (len <= 0) break;
        m_pos += len;
      }
      ::close(fd);
    }
  }
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, dataIgnoreTaint());
}

StringBuffer::StringBuffer(char *data, int len)
  : m_buffer(data), m_initialSize(1024), m_maxBytes(kDefaultOutputLimit),
    m_size(len), m_pos(len) {
  TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, dataIgnoreTaint());
}

StringBuffer::~StringBuffer() {
  if (m_buffer) {
    free(m_buffer);
  }
}

const char *StringBuffer::data() const {
  TAINT_OBSERVER_REGISTER_ACCESSED(m_taint_data);
  if (m_buffer && m_pos) {
    m_buffer[m_pos] = '\0'; // fixup
    return m_buffer;
  }
  return NULL;
}

const char *StringBuffer::dataIgnoreTaint() const {
  if (m_buffer && m_pos) {
    m_buffer[m_pos] = '\0'; // fixup
    return m_buffer;
  }
  return NULL;
}

char StringBuffer::charAt(int pos) const {
  ASSERT(pos >= 0 && pos < m_pos);
  if (m_buffer && pos >= 0 && pos < m_pos) {
    return m_buffer[pos];
  }
  return '\0';
}

char *StringBuffer::detach(int &size) {
  TAINT_OBSERVER_REGISTER_ACCESSED(m_taint_data);
#ifdef TAINTED
  m_taint_data.unsetTaint(TAINT_BIT_ALL);
#endif
  if (m_buffer) {
    if (m_pos) {
      m_buffer[m_pos] = '\0'; // fixup
      size = m_pos;
      char *ret = m_buffer;
      m_buffer = NULL;
      m_pos = 0;
      return ret;
    }
    size = 0;
  }
  return NULL;
}

String StringBuffer::detachImpl() {
  TAINT_OBSERVER_REGISTER_ACCESSED(m_taint_data);
#ifdef TAINTED
  m_taint_data.unsetTaint(TAINT_BIT_ALL);
#endif

  if (m_buffer && m_pos) {
    m_buffer[m_pos] = '\0'; // fixup
    String ret(m_buffer, m_pos, AttachString);
    m_buffer = NULL;
    m_pos = 0;
    return ret;
  }
  return String("");
}

String StringBuffer::copy() {
  // REGISTER_ACCESSED() is called by data()
  String r = String(data(), size(), CopyString);
  return r;
}

String StringBuffer::copyWithTaint() {
  TAINT_OBSERVER(TAINT_BIT_NONE, TAINT_BIT_NONE);
  String r = String(data(), size(), CopyString);
  return r;
}

void StringBuffer::absorb(StringBuffer &buf) {
  if (empty()) {
    TAINT_OBSERVER_REGISTER_ACCESSED(buf.getTaintDataRefConst());
    TAINT_OBSERVER_REGISTER_MUTATED(m_taint_data, dataIgnoreTaint());

    char *buffer = m_buffer;
    int size = m_size;

    m_buffer = buf.m_buffer;
    m_size = buf.m_size;
    m_pos = buf.m_pos;

    buf.m_buffer = buffer;
    buf.m_size = size;
    buf.reset();
  } else {
    // REGISTER_ACCESSED()/REGISTER_MUTATED() are called by append()/detach()
    append(buf.detach());
  }
}

void StringBuffer::reset() {
  m_pos = 0;
#ifdef TAINTED
  m_taint_data.unsetTaint(TAINT_BIT_ALL);
#endif
}

void StringBuffer::release() {
  if (m_buffer) {
    free(m_buffer);
    m_buffer = NULL;
  }
}

void StringBuffer::resize(int size) {
  ASSERT(size >= 0 && size < m_size);
  if (size >= 0 && size < m_size) {
    m_pos = size;
  }
}

char *StringBuffer::reserve(int size) {
  if (m_size - m_pos <  size) {
    m_size = m_pos + size;
    m_buffer = (char *)Util::safe_realloc(m_buffer, m_size + 1);
  } else if (m_buffer == NULL) {
    m_size = m_initialSize;
    m_buffer = (char *)Util::safe_malloc(m_size + 1);
  }
  return m_buffer + m_pos;
}

void StringBuffer::append(int n) {
  char buf[12];
  int is_negative;
  int len;
  const StringData *sd = String::GetIntegerStringData(n);
  char *p;
  if (!sd) {
    p = conv_10(n, &is_negative, buf + 12, &len);
  } else {
    p = (char *)sd->data();
    len = sd->size();
  }
  append(p, len);
}

void StringBuffer::append(int64 n) {
  char buf[21];
  int is_negative;
  int len;
  const StringData *sd = String::GetIntegerStringData(n);
  char *p;
  if (!sd) {
    p = conv_10(n, &is_negative, buf + 21, &len);
  } else {
    p = (char *)sd->data();
    len = sd->size();
  }
  append(p, len);
}

void StringBuffer::append(CVarRef v) {
  Variant::TypedValueAccessor tva = v.getTypedAccessor();
  if (Variant::IsString(tva)) {
    append(Variant::GetAsString(tva));
  } else if (IS_INT_TYPE(Variant::GetAccessorType(tva))) {
    append(Variant::GetInt64(tva));
  } else {
    append(v.toString());
  }
}

void StringBuffer::appendHelper(char ch) {
  if (m_buffer == NULL) {
    m_size = m_initialSize;
    m_buffer = (char *)Util::safe_malloc(m_size + 1);
  }

  if (m_pos == m_size) {
    growBy(1);
  }
  m_buffer[m_pos++] = ch;
}


void StringBuffer::append(CStrRef s) {
  // REGISTER_MUTATED() is called by data()
  append(s.data(), s.size());
}

void StringBuffer::appendHelper(const char *s, int len) {
  if (m_buffer == NULL) {
    m_size = m_initialSize;
    if (len > m_size) {
      m_size = len;
    }
    m_buffer = (char *)Util::safe_malloc(m_size + 1);
  }

  ASSERT(s);
  ASSERT(len >= 0);
  if (len <= 0) return;

  if (len > m_size - m_pos) {
    growBy(len);
  }
  memcpy(m_buffer + m_pos, s, len);
  m_pos += len;
}

#define REVERSE16(us)                                     \
  (((us & 0xf) << 12)      | (((us >> 4) & 0xf) << 8) |   \
  (((us >> 8) & 0xf) << 4) | ((us >> 12) & 0xf))          \

void StringBuffer::appendJsonEscape(const char *s, int len, int options) {
  if (len == 0) {
    append("\"\"", 2);
  } else {
    static const char digits[] = "0123456789abcdef";

    int start = size();
    append('"');

    UTF8To16Decoder decoder(s, len, options & k_JSON_FB_LOOSE);
    for (;;) {
      int c = decoder.decode();
      if (c == UTF8_END) {
        append('"');
        break;
      }
      if (c == UTF8_ERROR) {
        // discard the part that has been already decoded.
        resize(start);
        append("null", 4);
        break;
      }
      ASSERT(c >= 0);
      unsigned short us = (unsigned short)c;
      switch (us) {
      case '"':
        if (options & k_JSON_HEX_QUOT) {
          append("\\u0022", 6);
        } else {
          append("\\\"", 2);
        }
        break;
      case '\\': append("\\\\", 2); break;
      case '/':
        if (options & k_JSON_UNESCAPED_SLASHES) {
          append('/');
        } else {
          append("\\/", 2);
        }
        break;
      case '\b': append("\\b", 2);  break;
      case '\f': append("\\f", 2);  break;
      case '\n': append("\\n", 2);  break;
      case '\r': append("\\r", 2);  break;
      case '\t': append("\\t", 2);  break;
      case '<':
        if (options & k_JSON_HEX_TAG || options & k_JSON_FB_EXTRA_ESCAPES) {
          append("\\u003C", 6);
        } else {
          append('<');
        }
        break;
      case '>':
        if (options & k_JSON_HEX_TAG) {
          append("\\u003E", 6);
        } else {
          append('>');
        }
        break;
      case '&':
        if (options & k_JSON_HEX_AMP) {
          append("\\u0026", 6);
        } else {
          append('&');
        }
        break;
      case '\'':
        if (options & k_JSON_HEX_APOS) {
          append("\\u0027", 6);
        } else {
          append('\'');
        }
        break;
      case '@':
        if (options & k_JSON_FB_EXTRA_ESCAPES) {
          append("\\u0040", 6);
        } else {
          append('@');
        }
        break;
      case '%':
       	if (options & k_JSON_FB_EXTRA_ESCAPES) {
          append("\\u0025", 6);
       	} else {
          append('%');
        }
        break;
      default:
        if (us >= ' ' && (us & 127) == us) {
          append((char)us);
        } else {
          append("\\u", 2);
          us = REVERSE16(us);
          append(digits[us & ((1 << 4) - 1)]); us >>= 4;
          append(digits[us & ((1 << 4) - 1)]); us >>= 4;
          append(digits[us & ((1 << 4) - 1)]); us >>= 4;
          append(digits[us & ((1 << 4) - 1)]);
        }
        break;
      }
    }
  }
}

void StringBuffer::printf(const char *format, ...) {
  va_list ap;
  va_start(ap, format);

  bool printed = false;
  for (int len = 1024; !printed; len <<= 1) {
    va_list v;
    va_copy(v, ap);

    char *buf = (char*)Util::safe_malloc(len);
    if (vsnprintf(buf, len, format, v) < len) {
      append(buf);
      printed = true;
    }
    free(buf);

    va_end(v);
  }

  va_end(ap);
}

void StringBuffer::read(FILE* in, int page_size /* = 1024 */) {
  ASSERT(in);
  ASSERT(page_size > 0);

  if (m_buffer == NULL) {
    m_size = m_initialSize;
    m_buffer = (char *)Util::safe_malloc(m_size + 1);
  }

  while (true) {
    int buffer_size = m_size - m_pos;
    if (buffer_size < page_size) {
      growBy(page_size);
      buffer_size = m_size - m_pos;
    }
    int len = fread(m_buffer + m_pos, 1, buffer_size, in);
    if (len == 0) break;
    m_pos += len;
  }
}

void StringBuffer::read(File* in, int page_size /* = 1024 */) {
  ASSERT(in);
  ASSERT(page_size > 0);

  if (m_buffer == NULL) {
    m_size = m_initialSize;
    m_buffer = (char *)Util::safe_malloc(m_size + 1);
  }

  while (true) {
    int buffer_size = m_size - m_pos;
    if (buffer_size < page_size) {
      growBy(page_size);
      buffer_size = m_size - m_pos;
    }
    int len = in->readImpl(m_buffer + m_pos, buffer_size);
    if (len == 0) break;
    m_pos += len;
  }
}

void StringBuffer::growBy(int spaceRequired) {
  /*
   * The default initial size is a power-of-two minus 1.
   * This doubling scheme keeps the total block size a
   * power of two, which should be good for memory allocators.
   * But note there is no guarantee either that the initial size
   * is power-of-two minus 1, or that it stays that way
   * (new_size < minSize below).
   */
  long new_size = m_size * 2L + 1;
  long minSize = m_size + (long)spaceRequired;
  if (new_size < minSize) {
    new_size = minSize;
  }

  if (m_maxBytes > 0 && new_size > m_maxBytes) {
    if (minSize > m_maxBytes) {
      throw StringBufferLimitException(m_maxBytes, detach());
    } else {
      new_size = m_maxBytes;
    }
  }

  char *new_buffer;
  new_buffer = (char *)Util::safe_realloc(m_buffer, new_size + 1);

  m_size = new_size;
  m_buffer = new_buffer;
}

///////////////////////////////////////////////////////////////////////////////
}
