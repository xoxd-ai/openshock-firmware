#pragma once

#include "Common.h"
#include "Logging.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span.h>
#include <type_traits>
#include <utility>

template<typename T, typename SizeType = uint32_t>
class TinyVec {
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>, "TinyVec requires a trivially copyable & destructible type (POD).");

  DISABLE_COPY(TinyVec);

public:
  TinyVec() noexcept = default;

  TinyVec(SizeType count)
  {
    if (count == 0) return;
    reserve(count);
    _len = count;
  }
  TinyVec(const T* src, SizeType count)
  {
    if (!src || count == 0) return;
    reserve(count);
    memcpy(_data, src, size_t(count) * sizeof(T));
    _len = count;
  }

  ~TinyVec() { free(_data); }

  TinyVec(TinyVec&& other) noexcept
  {
    _data       = other._data;
    _len        = other._len;
    _cap        = other._cap;
    other._data = nullptr;
    other._len = other._cap = 0;
  }

  TinyVec& operator=(TinyVec&& other) noexcept
  {
    if (this != &other) {
      free(_data);
      _data       = other._data;
      _len        = other._len;
      _cap        = other._cap;
      other._data = nullptr;
      other._len = other._cap = 0;
    }
    return *this;
  }

  [[nodiscard]] SizeType size() const noexcept { return _len; }
  [[nodiscard]] SizeType capacity() const noexcept { return _cap; }
  [[nodiscard]] bool empty() const noexcept { return _len == 0; }
  [[nodiscard]] T* data() noexcept { return _data; }
  [[nodiscard]] const T* data() const noexcept { return _data; }

  T& operator[](SizeType i) noexcept { return _data[i]; }
  const T& operator[](SizeType i) const noexcept { return _data[i]; }

  void reserve(SizeType new_cap)
  {
    if (new_cap <= _cap) return;
    if (sizeof(T) && new_cap > std::numeric_limits<SizeType>::max() / sizeof(T)) OS_PANIC_INSTANT("TVEC", "Cannot allocate a buffer of given size!");

    void* newbuf = malloc(size_t(new_cap) * sizeof(T));
    if (!newbuf) OS_PANIC_INSTANT("TVEC", "Could not allocate a buffer of given size!");
    if (_data) {
      memcpy(newbuf, _data, size_t(_len) * sizeof(T));
      free(_data);
    }

    _data = static_cast<T*>(newbuf);
    _cap  = new_cap;
  }

  /// Like `reserve`, but returns false on an allocation failure instead of restarting the hub. For input a remote
  /// client controls.
  [[nodiscard]] bool try_reserve(SizeType new_cap) noexcept
  {
    if (new_cap <= _cap) return true;
    if (sizeof(T) && new_cap > std::numeric_limits<SizeType>::max() / sizeof(T)) return false;

    void* newbuf = malloc(size_t(new_cap) * sizeof(T));
    if (!newbuf) return false;
    if (_data) {
      memcpy(newbuf, _data, size_t(_len) * sizeof(T));
      free(_data);
    }

    _data = static_cast<T*>(newbuf);
    _cap  = new_cap;
    return true;
  }

  void resize(SizeType new_len)
  {
    if (new_len > _cap) reserve(next_cap(new_len));

    if (new_len > _len) {
      memset(_data + _len, 0, size_t(new_len - _len) * sizeof(T));
    }

    _len = new_len;
  }

  void append(const T* src, SizeType count)
  {
    if (!src || count == 0) return;
    SizeType new_len = _len + count;
    if (new_len > _cap) reserve(next_cap(new_len));
    memcpy(_data + _len, src, size_t(count) * sizeof(T));
    _len = new_len;
  }

  /// Like `append`, but returns false and changes nothing on an overflow or allocation failure.
  [[nodiscard]] bool try_append(const T* src, SizeType count) noexcept
  {
    if (!src || count == 0) return true;
    if (count > std::numeric_limits<SizeType>::max() - _len) return false;
    SizeType new_len = _len + count;
    if (new_len > _cap && !try_reserve(next_cap(new_len))) return false;
    memcpy(_data + _len, src, size_t(count) * sizeof(T));
    _len = new_len;
    return true;
  }

  void assign(const T* src, SizeType count)
  {
    if (!src || count == 0) {
      _len = 0;
      return;
    }
    if (count > _cap) reserve(next_cap(count));
    memcpy(_data, src, size_t(count) * sizeof(T));
    _len = count;
  }

  void clear() noexcept { _len = 0; }

private:
  SizeType next_cap(SizeType min_needed) const noexcept
  {
    SizeType newcap = _cap ? (_cap + _cap / 2 + 1) : 4;
    if (newcap < min_needed) newcap = min_needed;
    return newcap;
  }

  T* _data      = nullptr;
  SizeType _len = 0;
  SizeType _cap = 0;
};
