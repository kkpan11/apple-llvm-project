//===- AtomicUniquePointer.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_ATOMICUNIQUEPOINTER_H
#define LLVM_ADT_ATOMICUNIQUEPOINTER_H

#include <atomic>

namespace llvm {

/// Owning atomic pointer that's lock-free.
///
/// Underlying storage is \a std::atomic<T *>.
template <class T> class AtomicUniquePointer {
public:
  void store(std::unique_ptr<T> Value) {
    delete Storage.exchange(Value.release());
  }

  std::unique_ptr<T> exchange(std::unique_ptr<T> Value) {
    return std::unique_ptr<T>(Storage.exchange(Value.release()));
  }

  /// On success, the stored value is swapped with \p Value. Otherwise \p Value
  /// remains unchanged.
  bool compare_exchange_weak(T *&ExistingValue, std::unique_ptr<T> &Value) {
    if (!Storage.compare_exchange_weak(ExistingValue, Value.get()))
      return false;
    Value.release();
    Value.reset(ExistingValue);
    return true;
  }

  /// On success, the stored value is swapped with \p Value. Otherwise \p Value
  /// remains unchanged.
  bool compare_exchange_strong(T *&ExistingValue, std::unique_ptr<T> &Value) {
    if (!Storage.compare_exchange_strong(ExistingValue, Value.get()))
      return false;
    Value.release();
    Value.reset(ExistingValue);
    return true;
  }

  /// Return ownership of the stored pointer.
  std::unique_ptr<T> take() { return exchange(nullptr); }

  T *load() const { return Storage.load(); }

  explicit operator bool() const { return load(); }
  operator T *() const { return load(); }

  T &operator*() const {
    T *P = load();
    assert(P && "Unexpected null dereference");
    return *P;
  }
  T *operator->() const { return &operator*(); }

  AtomicUniquePointer() : Storage(nullptr) {}
  AtomicUniquePointer(nullptr_t) : Storage(nullptr) {}
  AtomicUniquePointer(std::unique_ptr<T> Value) : Storage(Value.release()) {}
  AtomicUniquePointer(AtomicUniquePointer &&RHS) : Storage(RHS.take()) {}
  AtomicUniquePointer(const AtomicUniquePointer &) = delete;

  /// Destroy the owned pointer.
  ~AtomicUniquePointer() { delete load(); }

  AtomicUniquePointer &operator=(nullptr_t) {
    store(nullptr);
    return *this;
  }
  AtomicUniquePointer &operator=(AtomicUniquePointer &&RHS) {
    store(RHS.take());
    return *this;
  }
  AtomicUniquePointer &operator=(const AtomicUniquePointer &) = delete;

protected:
  std::atomic<T *> Storage;
};

} // end namespace llvm

#endif // LLVM_ADT_ATOMICUNIQUEPOINTER_H
