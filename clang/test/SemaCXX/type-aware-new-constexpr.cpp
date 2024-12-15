// RUN: %clang_cc1 -fsyntax-only -verify %s -std=c++23 -fexperimental-cxx-type-aware-allocators -fexceptions
// RUN: %clang_cc1 -fsyntax-only -verify %s -std=c++23 -fexperimental-cxx-type-aware-allocators -fexceptions -fexperimental-new-constant-interpreter

namespace std {
  template <class T> struct type_identity {};
  enum class align_val_t : __SIZE_TYPE__ {};
  struct destroying_delete_t { explicit destroying_delete_t() = default; };
  template<typename T>
  struct allocator {
    constexpr T* allocate(__SIZE_TYPE__ n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    constexpr void deallocate(T* p, __SIZE_TYPE__) { ::operator delete(p); }
  };
}

using size_t = __SIZE_TYPE__;
void non_const_delete(void*);
void *non_const_alloc(size_t sz);

struct S1 {
  constexpr explicit S1() : i(5) {  }
  const int i;
};

void *operator new(std::type_identity<S1>, size_t sz); // #1
void operator delete(std::type_identity<S1>, void* ptr); // #2

constexpr int ensure_consteval_skips_typed_allocators() {
  // Verify we dont resolve typed allocators in const contexts
  auto * s = new S1();
  auto result = s->i;
  delete s;
  return result;
};

struct S2 {
  constexpr explicit S2() : i(5) {  }
  const int i;
};

void *operator new(std::type_identity<S2>, size_t sz) = delete; // #3
void operator delete(std::type_identity<S2>, void* ptr) = delete; // #4

constexpr int ensure_constexpr_retains_types_at_runtime() {
  // Verify we dont resolve typed allocators in const contexts
  S2 *s = new S2();
  // expected-error@-1 {{call to deleted function 'operator new'}}
  // expected-note@#1 {{candidate function not viable: no known conversion from 'type_identity<S2>' to 'type_identity<S1>' for 1st argument}}
  // expected-note@#3 {{candidate function has been explicitly deleted}}
  auto result = s->i;
  delete s;
  // expected-error@-1 {{attempt to use a deleted function}}
  // expected-note@#4 {{'operator delete' has been explicitly marked deleted here}}
  return result;
};


struct S3 {
  constexpr explicit S3() : i(5) {  }
  const int i;
  template <typename T> void* operator new(std::type_identity<T>, size_t sz) = delete; // #5
  template <typename T> void operator delete(std::type_identity<T>, void *) = delete; // #6
};

template <typename T> void* operator new(std::type_identity<T>, size_t sz) = delete; // #7
template <typename T> void operator delete(std::type_identity<T>, void *) = delete; // #8

constexpr int constexpr_vs_inclass_operators() {
  S3 *s;
  if consteval {
    s = ::new S3();
    // expected-error@-1 {{call to deleted function 'operator new'}}
    // expected-note@#1 {{candidate function not viable: no known conversion from 'type_identity<S3>' to 'type_identity<S1>' for 1st argument}}
    // expected-note@#3 {{candidate function not viable: no known conversion from 'type_identity<S3>' to 'type_identity<S2>' for 1st argument}}
    // expected-note@#7 {{candidate function [with T = S3] has been explicitly deleted}}
  } else {
    s = new S3();
    // expected-error@-1 {{call to deleted function 'operator new'}}
    // expected-note@#5 {{candidate function [with T = S3] has been explicitly deleted}}
  }
  auto result = s->i;
  if consteval {
    ::delete s;
    // expected-error@-1 {{attempt to use a deleted function}}
    // expected-note@#8 {{'operator delete<S3>' has been explicitly marked deleted here}}
  } else {
    delete s;
    // expected-error@-1 {{attempt to use a deleted function}}
    // expected-note@#6 {{'operator delete<S3>' has been explicitly marked deleted here}}
  }
  return result;
};

// Test a variety of valid constant evaluation paths
struct S4 {
  int i = 1;
  constexpr S4() __attribute__((noinline)) {}
};
template <typename T> struct Alloc {
   int header;
   T t;
};
void *non_const_alloc(size_t sz);
constexpr void* operator new(std::type_identity<S4>, size_t sz) {
  if consteval {
    return ::operator new(sz);
  } else {
    return non_const_alloc(sz);
  }
}
constexpr void operator delete(std::type_identity<S4>, void * ptr) {
  if consteval {
    ::operator delete(ptr, sizeof(S4));
    return;
  } else {
    non_const_delete(ptr);
  }
}
constexpr void* operator new[](std::type_identity<S4>, size_t sz) {
  if consteval {
    return ::operator new[](sz);
  } else {
    return non_const_alloc(sz);
  }
}
void non_const_delete(void*);
constexpr void operator delete[](std::type_identity<S4>, void * ptr) {
  if consteval {
    ::operator delete[](ptr);
    return;
  } else {
    non_const_delete(ptr);
  }
}
namespace std {
  using size_t = decltype(sizeof(0));
}

constexpr int do_dynamic_alloc(int n) {
  S4* s = new S4;
  int result = n * s->i;
  S4 *array = new S4[5];
  delete [] array;
  delete s;
  return 0;
}

template <int N> struct Tag {
};

static constexpr int force_do_dynamic_alloc = do_dynamic_alloc(5);

constexpr int test_consteval_calling_constexpr(int i) {
  if consteval {
    return do_dynamic_alloc(2 * i);
  }
  return do_dynamic_alloc(3 * i);
}

int test_consteval(int n,
                   Tag<test_consteval_calling_constexpr(2)>,
                   Tag<do_dynamic_alloc(3)>) {
  static constexpr int t1 = test_consteval_calling_constexpr(4);
  static constexpr int t2 = do_dynamic_alloc(5);
  int t3 = test_consteval_calling_constexpr(6);
  int t4 = do_dynamic_alloc(7);
  return t1 * t2 * t3 * t4;
}

struct S5 {
  int i;
};
constexpr void* operator new[](std::type_identity<S5>, size_t sz) { // #s5_array_new
  if consteval {
    // Intentionally allocate an incorrect array type
    return ::operator new[](sizeof(S5));
  } else {
    return non_const_alloc(sz);
  }
}
constexpr void operator delete[](std::type_identity<S5>, void * ptr) {
  if consteval {
    ::operator delete[](ptr);
    return;
  } else {
    non_const_delete(ptr);
  }
}

constexpr int failing_test1() {
  S5 *s = new S5[4]; // #incorrect_type
  // expected-error@-1 {{constexpr 'operator new[]' should have returned an allocation of type 'S5[4]' but returned 'S5[1]'}}
  // expected-new@#s5_array_new {{constexpr 'operator new[]' declared here}}
  delete [] s;
  return 0;
}

constexpr static int failed_test1 = failing_test1();
// expected-error@-1 {{constexpr variable 'failed_test1' must be initialized by a constant expression}}
// expected-note@-2 {{in call to 'failing_test1()'}}

struct S6 {
  int i;
};
constexpr void* operator new(std::type_identity<S6>, size_t sz) { // #s6_new
  if consteval {
    return new S6[1];
  } else {
    return non_const_alloc(sz);
  }
}
constexpr void operator delete(std::type_identity<S6>, void * ptr) {
  if consteval {
    ::operator delete(ptr);
    return;
  } else {
    non_const_delete(ptr);
  }
}

constexpr int failing_test2() {
  new S6;
  // expected-error@-1 {{constexpr 'operator new' should have returned an allocation of type 'S6' but returned 'S6[1]'}}
  // expected-note@#s6_new {{constexpr 'operator new' declared here}}
  return 0;
}
constexpr static int failed_test2 = failing_test2();
// expected-error@-1 {{constexpr variable 'failed_test2' must be initialized by a constant expression}}
// expected-note@-2 {{in call to 'failing_test2()'}}

struct S7 {
  int i;
};
struct S7_fake {
  int i;
  constexpr void *operator new(size_t sz) { return ::operator new(sz); }
  constexpr void operator delete(void *ptr) { ::operator delete(ptr); }
};
constexpr void* operator new(std::type_identity<S7>, size_t sz) {
  if consteval {
    // Intentionally allocate an incorrect type
    return new S7_fake; 
  } else {
    return non_const_alloc(sz);
  }
}
constexpr void operator delete(std::type_identity<S7>, void * ptr) {
  if consteval {
    ::operator delete(ptr);
    return;
  } else {
    non_const_delete(ptr);
  }
}

constexpr int failing_test3() {
  S7 *s = new S7;
  // expected-error@-1 {{constexpr 'operator new' should have returned an allocation of type 'S7' but returned 'S7_fake'}}
  delete s;
  return 0;
}
constexpr static int failed_test3 = failing_test3();
// expected-error@-1 {{constexpr variable 'failed_test3' must be initialized by a constant expression}}
// expected-note@-2 {{in call to 'failing_test3()'}}

struct S8 {
  int i;
};

constexpr void* operator new[](std::type_identity<S8>, size_t sz) {
  if consteval {
    // Intentionally allocate an incorrect array type
    return &static_cast<S8*>(::operator new[](sz + sizeof(S8)))[1];
  } else {
    return non_const_alloc(sz);
  }
}
constexpr void operator delete[](std::type_identity<S8>, void * ptr) {
  if consteval {
    ::operator delete[](static_cast<S8*>(ptr) - 1);
    return;
  } else {
    non_const_delete(ptr);
  }
}

constexpr int failing_test4() {
  S8 *s = new S8[10];
  // expected-error@-1 {{constexpr 'operator new[]' should have returned an allocation of type 'S8[10]' but returned 'S8[11]'}}
  delete [] s;
  return 0;
}
constexpr static int failed_test4 = failing_test4();
// expected-error@-1 {{constexpr variable 'failed_test4' must be initialized by a constant expression}}
