// io.h - utilities for I/O
//
// TODO(amb): we should remove WriteFull(), but first Caladan's TCP stack must
// be fixed to never return less than the requested bytes. This is the correct
// POSIX behavior (ReadFull() is still needed).

#pragma once

extern "C" {
#include <base/stddef.h>
#include <sys/uio.h>
}

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <streambuf>
#include <type_traits>
#include <vector>

#include "junction/base/arch.h"
#include "junction/base/error.h"

namespace junction {

// Reader is a concept for the basic UNIX-style Read method
template <typename T>
concept Reader = requires(T t) {
  {
    t.Read(std::declval<std::span<std::byte>>())
  } -> std::same_as<Status<size_t>>;
};

// Writer is a concept for the basic UNIX-style Write method
template <typename T>
concept Writer = requires(T t) {
  {
    t.Write(std::declval<std::span<const std::byte>>())
  } -> std::same_as<Status<size_t>>;
};

// Cast an object as a const byte span (for use with Write())
template <typename T>
std::span<const std::byte, sizeof(T)> byte_view(const T &t) {
  return std::as_bytes(std::span<const T, 1>{std::addressof(t), 1});
}

// Cast an object as a mutable byte span (for use with Read())
template <typename T>
std::span<std::byte, sizeof(T)> writable_byte_view(T &t) {
  return std::as_writable_bytes(std::span<T, 1>{std::addressof(t), 1});
}

// Cast a legacy UNIX read buffer as a span.
inline std::span<std::byte> readable_span(char *buf, size_t len) {
  return {reinterpret_cast<std::byte *>(buf), len};
}

// Cast a legacy UNIX write buffer as a span.
inline std::span<const std::byte> writable_span(const char *buf, size_t len) {
  return {reinterpret_cast<const std::byte *>(buf), len};
}

// Reads the full span of bytes.
template <Reader T>
Status<void> ReadFull(T &t, std::span<std::byte> buf) {
  size_t n = 0;
  while (n < buf.size()) {
    Status<size_t> ret =
        t.Read({buf.begin() + static_cast<ssize_t>(n), buf.end()});
    if (!ret) return MakeError(ret);
    n += *ret;
  }
  assert(n == buf.size());
  return {};
}

// Writes the full span of bytes.
template <Writer T>
Status<void> WriteFull(T &t, std::span<const std::byte> buf) {
  size_t n = 0;
  while (n < buf.size()) {
    Status<size_t> ret =
        t.Write({buf.begin() + static_cast<ssize_t>(n), buf.end()});
    if (!ret) return MakeError(ret);
    n += *ret;
  }
  assert(n == buf.size());
  return {};
}

template <Reader T, ssize_t size = 16 * kPageSize>
class StreamBufferReader : public std::streambuf {
 public:
  StreamBufferReader(T &in) noexcept : in_(in) {
    buf_ = std::make_unique<std::byte[]>(size);
    char *ptr = reinterpret_cast<char *>(buf_.get());
    setg(ptr, ptr, ptr);
  }
  // disable copy.
  StreamBufferReader(const StreamBufferReader &r) = delete;
  StreamBufferReader &operator=(const StreamBufferReader &r) = delete;

  // allow move.
  StreamBufferReader(StreamBufferReader &&r) noexcept
      : in_(std::move(r.in_)), buf_(std::move(r.buf_)) {
    setg(r.eback(), r.gptr(), r.egptr());
  }
  StreamBufferReader &operator=(StreamBufferReader &&r) noexcept {
    in_ = r.in_;
    buf_ = std::move(r.buf_);
    setg(r.eback(), r.gptr(), r.egptr());
    return *this;
  }
  ~StreamBufferReader() = default;

 protected:
  // helpers
  inline std::byte *start() { return reinterpret_cast<std::byte *>(eback()); }
  inline std::byte *pos() { return reinterpret_cast<std::byte *>(gptr()); }
  inline std::byte *limit() { return reinterpret_cast<std::byte *>(egptr()); }
  inline void set_pos(size_t n) { setg(eback(), eback() + n, egptr()); }
  inline void inc_pos(int n) { gbump(n); }
  inline void set_limit(size_t n) { setg(eback(), gptr(), eback() + n); }
  inline std::streamsize bytes_left() {
    return static_cast<std::streamsize>(egptr() - gptr());
  }

  // xsgetn has ReadFull semantics
  //
  // From the docs:
  // "Retrieves characters from the controlled input sequence and stores them in
  // the array pointed by s, until either n characters have been extracted or
  // the end of the sequence is reached."
  //
  // This implementation translates all errors to EOF and returns the number of
  // bytes copied to dst before hitting EOF
  std::streamsize xsgetn(char *s, std::streamsize out_size) override {
    std::span<std::byte> dst = readable_span(s, out_size);
    std::streamsize n = 0;
    Status<size_t> ret;

    while (n < out_size) {
      if (bytes_left() == 0) {
        // fast path: copy to dst span if remaining size is >= the internal
        // buffer's size
        if (out_size - n >= size) {
          // reading directly to the dst span has ReadFull semantics
          while (n < out_size) {
            ret = in_.Read(dst.subspan(n));
            if (!ret) return n;
            n += *ret;
          }
        } else {
          ret = Fill();
          if (!ret) return n;
        }
      }

      // copy from internal buf to dst
      size_t copy_size = std::min(out_size - n, bytes_left());
      std::copy_n(pos(), copy_size, dst.begin() + n);
      inc_pos(static_cast<int>(copy_size));
      n += copy_size;
    }

    return out_size;
  }

  int_type underflow() override {
    if (bytes_left() == 0) {
      Status<size_t> ret = Fill();
      if (!ret) return std::char_traits<char>::eof();
    }

    // return character at pos
    return std::char_traits<char>::not_eof(*gptr());
  }

  std::streamsize showmanyc() override { return bytes_left(); }

  int pbackfail(int c) override {
    if (eback() == gptr() || c != *gptr() || c == std::char_traits<char>::eof())
      return std::char_traits<char>::eof();

    inc_pos(-1);
    return std::char_traits<char>::not_eof(c);
  }

 private:
  // Fill() overwrites the contents of buf_ reading as much as possible
  // from the underlying Reader.
  //
  // Returns the number of bytes read, possibly less than the size of
  // buf_ if EOF is reached.
  //
  // Fill() returns an error if either:
  // 1. the underlying reader was at EOF before Fill() was called
  // 2. Read returned an error value not equal to 0.
  Status<size_t> Fill() {
    size_t n = 0;
    while (n < size) {
      Status<size_t> ret = in_.Read(std::span{start(), start() + size});
      if (!ret) {
        // check for EOF
        if (ret.error() == 0) break;
        return MakeError(ret);
      }
      n += *ret;
    }
    set_pos(0);
    set_limit(n);
    if (n == 0) return MakeError(0);
    return n;
  }

  T &in_;
  std::unique_ptr<std::byte[]> buf_;
};

template <Writer T>
class BufferedWriter {
 public:
  using WriterType = T;
  BufferedWriter(T &out, size_t len = kPageSize * 16) noexcept : out_(out) {
    buf_.reserve(len);
  }
  // copy
  BufferedWriter(const BufferedWriter &w) : out_(w.out_), buf_(w.buf_) {}
  // move
  BufferedWriter(BufferedWriter &&w) noexcept
      : out_(std::move(w.out_)), buf_(std::move(w.buf_)) {}
  // assignments
  BufferedWriter &operator=(const BufferedWriter &w) {
    out_ = w.out_;
    buf_ = w.buf_;
    return *this;
  }
  BufferedWriter &operator=(BufferedWriter &&w) noexcept {
    out_ = std::move(w.out_);
    buf_ = std::move(w.buf_);
    return *this;
  }
  ~BufferedWriter() { Flush(); }

  Status<size_t> Write(std::span<const std::byte> src) {
    size_t in_size = src.size();
    size_t n = 0;
    while (n < in_size) {
      // fast path: avoid extra copies if @src is already large enough
      if (buf_.empty() && (in_size - n) >= buf_.capacity()) {
        Status<void> ret = WriteFull(out_, src.subspan(n));
        if (unlikely(!ret)) return MakeError(ret);
        break;
      }

      // cold path: copy @src into the buffer
      size_t copy_size = std::min(in_size - n, buf_.capacity() - buf_.size());
      std::ranges::copy(src.begin() + static_cast<ssize_t>(n),
                        src.begin() + static_cast<ssize_t>(n + copy_size),
                        std::back_inserter(buf_));
      n += copy_size;

      if (buf_.size() == buf_.capacity()) {
        Status<void> ret = Flush();
        if (unlikely(!ret)) return MakeError(ret);
      }
    }

    return {in_size};
  }

  Status<void> Flush() {
    Status<void> ret = WriteFull(out_, buf_);
    buf_.clear();
    if (!ret) return MakeError(ret);
    return {};
  }

 private:
  T &out_;
  std::vector<std::byte> buf_;
};

// StreamBufferWriter provides interoperability with std::streambuf for writes
template <Writer T>
class StreamBufferWriter final : public std::streambuf {
 public:
  StreamBufferWriter(T &t) : out_(t) {}
  ~StreamBufferWriter() = default;

 protected:
  std::streamsize xsputn(const char *s, std::streamsize n) override {
    return xsputn(out_, s, n);
  }

  int_type overflow(int_type ch) override {
    if (ch != std::char_traits<char>::eof()) {
      char_type val = std::char_traits<char>::to_char_type(ch);
      Status<size_t> ret = out_.Write(writable_byte_view(val));
      if (!ret) return std::char_traits<char>::eof();
      assert(*ret == sizeof(char));
    }
    return std::char_traits<char>::not_eof(ch);
  }

  int sync() override { return sync(out_); }

 private:
  template <Writer U>
  std::streamsize xsputn(U &out, const char *s, std::streamsize n) {
    Status<void> ret = WriteFull(out_, writable_span(s, n));
    return !ret ? 0 : n;
  }

  template <Writer U>
  std::streamsize xsputn(BufferedWriter<U> &out, const char *s,
                         std::streamsize n) {
    Status<size_t> ret = out.Write(writable_span(s, n));
    return !ret ? 0 : n;
  }

  template <Writer U>
  int sync(U &out) {
    return 0;
  }

  template <Writer U>
  int sync(BufferedWriter<U> &out) {
    Status<void> ret = out.Flush();
    return !ret ? -1 : 0;
  }

  T &out_;
};

// VectorIO is an interface for vector reads and writes.
class VectorIO {
 public:
  virtual ~VectorIO() = default;
  virtual Status<size_t> Readv(std::span<const iovec> iov) = 0;
  virtual Status<size_t> Writev(std::span<const iovec> iov) = 0;
};

Status<void> ReadvFull(VectorIO &io, std::span<const iovec> iov);
Status<void> WritevFull(VectorIO &io, std::span<const iovec> iov);

}  // namespace junction
