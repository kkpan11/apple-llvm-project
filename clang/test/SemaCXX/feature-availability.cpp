// RUN: %clang_cc1 -std=c++20 -triple arm64-apple-macosx15 -fblocks -fsyntax-only -verify -fexceptions -fcxx-exceptions -x c++ %s

#include <availability_domain.h>

#define AVAIL 0
#define UNAVAIL 1

CLANG_ENABLED_AVAILABILITY_DOMAIN(feature1);
CLANG_DISABLED_AVAILABILITY_DOMAIN(feature2);

// constexpr and consteval functions.
__attribute__((availability(domain:feature1, AVAIL))) constexpr int constexpr_func() { return 1; }
__attribute__((availability(domain:feature2, AVAIL))) constexpr int constexpr_func2() { return 1; }
__attribute__((availability(domain:feature1, AVAIL))) consteval int consteval_func() { return 1; }

// Class and enum types.
struct __attribute__((availability(domain:feature1, AVAIL))) Annotated {
  Annotated();
  explicit Annotated(int);
  ~Annotated();
};

// Annotating a variable with the same feature attribute guards the type use.
Annotated gType_guarded __attribute__((availability(domain:feature1, AVAIL)));
Annotated gType_guarded_init __attribute__((availability(domain:feature1, AVAIL))) = Annotated(1);
Annotated gType_unguarded;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gType_unguarded' with the 'feature1' domain availability attribute to silence this error}}
decltype(Annotated{}) gDecltypeBrace_unguarded;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gDecltypeBrace_unguarded' with the 'feature1' domain availability attribute to silence this error}}
decltype(Annotated{}) gDecltypeBrace_guarded __attribute__((availability(domain:feature1, AVAIL)));
decltype(gType_guarded) gDecltypeVar_unguarded;
// expected-error@-1 {{cannot use 'gType_guarded' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gDecltypeVar_unguarded' with the 'feature1' domain availability attribute to silence this error}}
decltype(gType_guarded) gDecltypeVar_guarded __attribute__((availability(domain:feature1, AVAIL)));

// sizeof is an unevaluated context but the type use is still diagnosed.
int gSizeof_unguarded = sizeof(Annotated);
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gSizeof_unguarded' with the 'feature1' domain availability attribute to silence this error}}
int gSizeof_guarded __attribute__((availability(domain:feature1, AVAIL))) = sizeof(Annotated);

// Non-virtual member functions, constructors, destructor, conversion operator,
// overloaded operator, static member function, static data member.
// Data member (field) annotation is rejected; same restriction as C struct members.
struct WithAnnotatedMembers {
  __attribute__((availability(domain:feature1, AVAIL))) void method();
  __attribute__((availability(domain:feature1, AVAIL))) WithAnnotatedMembers();
  __attribute__((availability(domain:feature1, AVAIL))) ~WithAnnotatedMembers();
  __attribute__((availability(domain:feature1, AVAIL))) operator int() const;
  __attribute__((availability(domain:feature1, AVAIL))) WithAnnotatedMembers operator+(const WithAnnotatedMembers &) const;
  __attribute__((availability(domain:feature1, AVAIL))) static void static_method();
  __attribute__((availability(domain:feature1, AVAIL))) static int static_i;
  int i __attribute__((availability(domain:feature1, AVAIL))); // expected-error {{feature attributes cannot be applied to struct members}}
};

// Out-of-class definition of annotated static data member: no diagnostic
// expected; the definition is not a use of the declaration.
int WithAnnotatedMembers::static_i = 42;

// Virtual member functions: not supported in v1 (vtable fallback codegen
// is deferred).
struct WithVirtualMembers {
  virtual void v0() __attribute__((availability(domain:feature1, AVAIL)));     // expected-error {{feature attributes cannot be applied to virtual functions}}
  virtual void v1() __attribute__((availability(domain:feature1, AVAIL))) = 0; // expected-error {{feature attributes cannot be applied to virtual functions}}
};

// Non-static data member initializer (NSDMI).
// FIXME: global NSDMI should be diagnosed but is currently missed because
// the use is deferred to the RAV, which never runs for file-scope structs.
// Platform availability (-Wunguarded-availability) has the same bug.
struct NSDMIFileScope {
  int x = constexpr_func();
};

// Constructor initializer list: a use in the init list is diagnosed if the
// constructor itself isn't annotated; an annotation on the constructor guards
// uses in its init list (just as it would for uses in the body).
struct WithCtorInitList {
  int x;
  WithCtorInitList() : x(constexpr_func()) {}
  // expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{annotate 'WithCtorInitList' with the 'feature1' domain availability attribute to silence this error}}
  __attribute__((availability(domain:feature1, AVAIL))) WithCtorInitList(int i) : x(constexpr_func() + i) {}
};

struct __attribute__((availability(domain:feature1, AVAIL))) WithInlineMethod {
  void m0() {
    int i = constexpr_func();
    int i2 = constexpr_func2();
    // expected-error@-1 {{cannot use 'constexpr_func2' because feature 'feature2' is unavailable in this context}}
    // expected-note@-2 {{enclose 'constexpr_func2' in a __builtin_available check}}
  }

  void m1();
};

void WithInlineMethod::m1() {
  int i = constexpr_func();
  int i2 = constexpr_func2();
  // expected-error@-1 {{cannot use 'constexpr_func2' because feature 'feature2' is unavailable in this context}}
  // expected-note@-2 {{enclose 'constexpr_func2' in a __builtin_available check}}
}

struct alignas(Annotated) AlignasUnguarded {
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'AlignasUnguarded' with the 'feature1' domain availability attribute to silence this error}}
};

struct alignas(Annotated) __attribute__((availability(domain:feature1, AVAIL))) AlignasGuarded {
};

// Inheritance: deriving from a feature-gated base without annotation is an error.
struct Derived0 : Annotated {};
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'Derived0' with the 'feature1' domain availability attribute to silence this error}}

// Inheritance: matching feature annotation is OK.
struct __attribute__((availability(domain:feature1, AVAIL))) Derived1 : Annotated {};

// Inheritance: different feature annotation is an error.
struct __attribute__((availability(domain:feature2, AVAIL))) Derived2 : Annotated {};
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'Derived2' with the 'feature1' domain availability attribute to silence this error}}

struct __attribute__((availability(domain:feature1, AVAIL))) Annotated2 {};

struct Derived3 : Annotated, Annotated2 {};
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'Derived3' with the 'feature1' domain availability attribute to silence this error}}
// expected-error@-3 {{cannot use 'Annotated2' because feature 'feature1' is unavailable in this context}}
// expected-note@-4 {{annotate 'Derived3' with the 'feature1' domain availability attribute to silence this error}}

struct __attribute__((availability(domain:feature1, AVAIL))) Derived4 : Annotated, Annotated2 {};

struct __attribute__((availability(domain:feature2, AVAIL))) Derived5 : Annotated, Annotated2 {};
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'Derived5' with the 'feature1' domain availability attribute to silence this error}}
// expected-error@-3 {{cannot use 'Annotated2' because feature 'feature1' is unavailable in this context}}
// expected-note@-4 {{annotate 'Derived5' with the 'feature1' domain availability attribute to silence this error}}

struct WithAnnotatedField {
  Annotated s0;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{annotate 'WithAnnotatedField' with the 'feature1' domain availability attribute to silence this error}}
};

template <class T>
struct ClassTemplate0 {
};

ClassTemplate0<Annotated> gCT0_unguarded;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gCT0_unguarded' with the 'feature1' domain availability attribute to silence this error}}
ClassTemplate0<Annotated> gCT0_guarded __attribute__((availability(domain:feature1, AVAIL)));

template <class T>
struct __attribute__((availability(domain:feature1, AVAIL))) ClassTemplate1 {
};

struct Plain {};
struct __attribute__((availability(domain:feature2, AVAIL))) AnnotatedF2 {};

ClassTemplate1<Plain> gCT1_plain_unguarded;
// expected-error@-1 {{cannot use 'ClassTemplate1<Plain>' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gCT1_plain_unguarded' with the 'feature1' domain availability attribute to silence this error}}
ClassTemplate1<Plain> gCT1_plain_guarded __attribute__((availability(domain:feature1, AVAIL)));
ClassTemplate1<AnnotatedF2> gCT1_f2_unguarded __attribute__((availability(domain:feature1, AVAIL)));
// expected-error@-1 {{cannot use 'AnnotatedF2' because feature 'feature2' is unavailable in this context}}
// expected-note@-2 {{annotate 'gCT1_f2_unguarded' with the 'feature2' domain availability attribute to silence this error}}
ClassTemplate1<AnnotatedF2> gCT1_f2_guarded __attribute__((availability(domain:feature1, AVAIL))) __attribute__((availability(domain:feature2, AVAIL)));

template <class T = Annotated>
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'ClassTemplate2' with the 'feature1' domain availability attribute to silence this error}}
struct ClassTemplate2 {
};

ClassTemplate2<> gCT2_default;

template <class T = Annotated>
struct __attribute__((availability(domain:feature1, AVAIL))) ClassTemplate3 {
};

ClassTemplate3<> gCT3_default_unguarded;
// expected-error@-1 {{cannot use 'ClassTemplate3<>' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gCT3_default_unguarded' with the 'feature1' domain availability attribute to silence this error}}

// Template with a hardcoded annotated member type.
// Error fires once at the template definition; instantiation sites do not warn.
template <class T>
struct ClassTemplate4 {
  Annotated member;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{annotate 'ClassTemplate4' with the 'feature1' domain availability attribute to silence this error}}
};

ClassTemplate4<int> gCT4_inst;  // no error.
ClassTemplate4<int> gCT4_inst_guarded __attribute__((availability(domain:feature1, AVAIL)));  // no error.

// Annotating the template suppresses the member error.
template <class T>
struct __attribute__((availability(domain:feature1, AVAIL))) ClassTemplate5 {
  Annotated member;  // no error.
};

template <class T>
int functionTemplate0() { return 1; }

int gFnTmpl_unguarded = functionTemplate0<Annotated>();
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gFnTmpl_unguarded' with the 'feature1' domain availability attribute to silence this error}}
int gFnTmpl_guarded __attribute__((availability(domain:feature1, AVAIL))) = functionTemplate0<Annotated>();

// Free functions and global variables (same as C).
__attribute__((availability(domain:feature1, AVAIL))) void free_func();
__attribute__((availability(domain:feature1, AVAIL))) int free_global;

// type alias.
using FeatureAlias __attribute__((availability(domain:feature1, AVAIL))) = int;
// FIXME: FeatureAlias2 is annotated with the same feature as FeatureAlias; no diagnostic should be emitted.
using FeatureAlias2 __attribute__((availability(domain:feature1, AVAIL))) = FeatureAlias; // expected-error {{cannot use 'FeatureAlias' because feature 'feature1' is unavailable in this context}}
using FeatureAlias3 = FeatureAlias; // expected-error {{cannot use 'FeatureAlias' because feature 'feature1' is unavailable in this context}}
// FIXME: note "annotate 'FeatureAlias3' with..." should be emitted but isn't because the context is TranslationUnitDecl (not a NamedDecl) when the diagnostic fires.

// FIXME: b1 uses Annotated without an availability annotation and should be diagnosed,
// but isn't; the delayed diagnostic on Annotated is consumed by a1's attribute.
Annotated a1 __attribute__((availability(domain:feature1, AVAIL))), b1;

// Free function: baseline, same as C.
void test_free_func() {
  free_func();
  // expected-error@-1 {{cannot use 'free_func' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'free_func' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    free_func();
}

// Non-virtual member function call and method pointer.
void test_member_func() {
  WithAnnotatedMembers s;
  // expected-error@-1 {{cannot use 'WithAnnotatedMembers' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedMembers' in a __builtin_available check}}
  s.method();
  // expected-error@-1 {{cannot use 'method' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'method' in a __builtin_available check}}

  auto mfp = &WithAnnotatedMembers::method;
  // expected-error@-1 {{cannot use 'method' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'method' in a __builtin_available check}}

  if (__builtin_available(domain:feature1)) {
    s.method();
    auto mfp2 = &WithAnnotatedMembers::method;
  }
}

// Type use.
void test_type_use() {
  Annotated *p = nullptr;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  if (__builtin_available(domain:feature1)) {
    Annotated *q = nullptr;
  }
}

// Constructing a feature-gated type: Annotated is annotated so both the
// type reference and the constructor call are diagnosed. WithAnnotatedMembers is not annotated
// as a type but its constructor is, so only the constructor call is diagnosed.
void test_ctor() {
  Annotated obj;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  Annotated obj2(1);
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  WithAnnotatedMembers s;
  // expected-error@-1 {{cannot use 'WithAnnotatedMembers' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedMembers' in a __builtin_available check}}
  if (__builtin_available(domain:feature1)) {
    Annotated obj3;
    Annotated obj4(1);
    WithAnnotatedMembers s2;
  }
}

// constexpr/consteval call: error fired even in a constant-expression context.
void test_constexpr() {
  constexpr int i = constexpr_func();
  // expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'constexpr_func' in a __builtin_available check}}
  constexpr int j = consteval_func();
  // expected-error@-1 {{cannot use 'consteval_func' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'consteval_func' in a __builtin_available check}}
  if (__builtin_available(domain:feature1)) {
    constexpr int k = constexpr_func();
    constexpr int l = consteval_func();
  }
}

// static_assert: error fired (same behavior as platform availability;
// the checker does not special-case compile-time-only contexts).
static_assert(constexpr_func() == 1); // expected-error {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}

// Containing function carries the same feature annotation; all uses OK.
__attribute__((availability(domain:feature1, AVAIL)))
void test_same_feature_context() {
  free_func();
  Annotated obj;
  WithAnnotatedMembers s;
  s.method();
}

auto lambda0 = []() __attribute__((availability(domain:feature1, AVAIL))) { return 2; };

int gLambdaCall_unguarded = lambda0();
// expected-error@-1 {{cannot use 'operator()' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gLambdaCall_unguarded' with the 'feature1' domain availability attribute to silence this error}}

void test_lambda_call() {
  int i = lambda0();
  // expected-error@-1 {{cannot use 'operator()' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'operator()' in a __builtin_available check to silence this warning}}

  if (__builtin_available(domain:feature1))
    (void)lambda0();
}

int gLambdaInit_unguarded = []{ return constexpr_func(); }();
// expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{enclose 'constexpr_func' in a __builtin_available check}}
// FIXME: pre-existing bug shared with platform availability. gLambdaInit_unguarded_FIXME's
// annotation does not propagate into the lambda body because the lambda's
// DeclContext is the enclosing namespace/TU, not the VarDecl being initialized.
__attribute__((availability(domain:feature1, AVAIL))) int gLambdaInit_unguarded_FIXME = []{ return constexpr_func(); }();
// expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{enclose 'constexpr_func' in a __builtin_available check}}

// Lambda inside a feature-gated function inherits the enclosing function's
// feature context, so no diagnostic.
__attribute__((availability(domain:feature1, AVAIL)))
void test_lambda_in_guarded_func() {
  auto f = [&] {
    free_func();
  };
}

// Block expression at file scope: the block is not inside a function, so
// DiagnoseUnguardedFeatureAvailabilityViolations is called directly on the
// BlockDecl.
int gBlock_unguarded = ^int(void) {
  Annotated s;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check to silence this warning}}
  return 1;
}();

// if constexpr does not currently guard feature availability; only regular if
// does. This may be revisited in the future.
void test_if_constexpr() {
  if constexpr (__builtin_available(domain:feature1))
    free_func();
    // expected-error@-1 {{cannot use 'free_func' because feature 'feature1' is unavailable in this context}}
    // expected-note@-2 {{enclose 'free_func' in a __builtin_available check}}

  if constexpr (__builtin_available(domain:feature2))
    free_func();
    // expected-error@-1 {{cannot use 'free_func' because feature 'feature1' is unavailable in this context}}
    // expected-note@-2 {{enclose 'free_func' in a __builtin_available check}}
}

// Implicit copy/move construction: requires CXXConstructExpr handling.
struct WithAnnotatedCopyMove {
  WithAnnotatedCopyMove() = default;
  __attribute__((availability(domain:feature1, AVAIL))) WithAnnotatedCopyMove(const WithAnnotatedCopyMove &);
  __attribute__((availability(domain:feature1, AVAIL))) WithAnnotatedCopyMove(WithAnnotatedCopyMove &&);
};

void takes_annotated_copy_move_by_value(WithAnnotatedCopyMove);

void test_copy_move_ctor() {
  WithAnnotatedCopyMove a;
  WithAnnotatedCopyMove b = a;
  // expected-error@-1 {{cannot use 'WithAnnotatedCopyMove' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedCopyMove' in a __builtin_available check}}
  takes_annotated_copy_move_by_value(a);
  // expected-error@-1 {{cannot use 'WithAnnotatedCopyMove' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedCopyMove' in a __builtin_available check}}
  WithAnnotatedCopyMove m = static_cast<WithAnnotatedCopyMove &&>(a);
  // expected-error@-1 {{cannot use 'WithAnnotatedCopyMove' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedCopyMove' in a __builtin_available check}}
  if (__builtin_available(domain:feature1)) {
    WithAnnotatedCopyMove c = a;
    takes_annotated_copy_move_by_value(a);
    WithAnnotatedCopyMove n = static_cast<WithAnnotatedCopyMove &&>(a);
  }
}

// Implicit conversion operator.
void test_implicit_conversion() {
  WithAnnotatedMembers s;
  // expected-error@-1 {{cannot use 'WithAnnotatedMembers' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'WithAnnotatedMembers' in a __builtin_available check}}
  int x = s;
  // expected-error@-1 {{cannot use 'operator int' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'operator int' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    int y = s;
}

// new: type use diagnosed twice (once for the pointer type, once for the new
// expression), same behavior as platform availability. delete: destructor not
// separately annotated, no error.
void test_new_delete() {
  Annotated *p = new Annotated;
  // expected-error@-1 2 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 2 {{enclose 'Annotated' in a __builtin_available check}}
  delete p;
  if (__builtin_available(domain:feature1)) {
    Annotated *q = new Annotated;
    delete q;
  }
}

// alignof: unevaluated but type use is still diagnosed, same as sizeof.
int gAlignof_unguarded = alignof(Annotated);
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gAlignof_unguarded' with the 'feature1' domain availability attribute to silence this error}}
int gAlignof_guarded __attribute__((availability(domain:feature1, AVAIL))) = alignof(Annotated);

// Trailing return type.
auto trailing_ret_unguarded() -> Annotated;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'trailing_ret_unguarded' with the 'feature1' domain availability attribute to silence this error}}
__attribute__((availability(domain:feature1, AVAIL))) auto trailing_ret_guarded() -> Annotated;

// Array of feature-gated type.
void test_array() {
  Annotated arr[3];
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    Annotated arr2[3];
}

// Default function argument: error at the declaration (same behavior as
// platform availability).
void func_with_default(int x = constexpr_func());
// expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'func_with_default' with the 'feature1' domain availability attribute to silence this error}}
void test_default_arg() {
  func_with_default();
  if (__builtin_available(domain:feature1))
    func_with_default();
}

// throw / catch.
void test_throw() {
  throw Annotated();
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    throw Annotated();
}

void test_catch() {
  try {
  } catch (Annotated &) {
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  }
}

__attribute__((availability(domain:feature1, AVAIL)))
void test_catch_guarded() {
  try {
  } catch (Annotated &) { // OK: enclosing function carries matching feature annotation.
  }
}

// Lambda inside an unannotated function: uses inside are diagnosed.
void test_lambda_unguarded() {
  auto f = [&] {
    free_func();
    // expected-error@-1 {{cannot use 'free_func' because feature 'feature1' is unavailable in this context}}
    // expected-note@-2 {{enclose 'free_func' in a __builtin_available check}}
  };
  if (__builtin_available(domain:feature1)) {
    auto g = [&] {
      free_func();
    };
  }
}

void test_stmt_expr() {
  struct StmtExpr {
    int m0 = ({
      Annotated a;
      // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
      // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check to silence this warning}}
      if (__builtin_available(domain:feature1)) {
        Annotated b;
      }
      1;
    });
  };
  int m1 = ({
    Annotated a;
    // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
    // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check to silence this warning}}
    if (__builtin_available(domain:feature1)) {
      Annotated b;
    }
    1;
  });
}

// Local class with NSDMI inside a function body: the NSDMI cannot be directly
// guarded, but the local class definition itself can be wrapped in a
// __builtin_available check.
void test_local_class_nsdmi() {
  struct Local {
    int x = constexpr_func();
    // expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
    // expected-note@-2 {{annotate 'Local' with the 'feature1' domain availability attribute to silence this error}}
  };
  if (__builtin_available(domain:feature1)) {
    struct GuardedLocal {
      int x = constexpr_func(); // OK.
    };
  }
}

// C++20 concepts.

// Concept body that references a feature-gated function. The body is checked
// at the concept definition, where the lexical context is the translation
// unit, so no "annotate ..." note is emitted (same shape as FeatureAlias3).
template <class T>
concept HasFeatureFunc = requires(T) { constexpr_func(); }; // expected-error {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}

// Concept body that references a feature-gated type.
template <class T>
concept UsesAnnotated = requires { sizeof(Annotated); }; // expected-error {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}

// Concept whose body does not reference any feature-gated decl.
template <class T>
concept Sized = sizeof(T) > 0;

// Concept-constrained variable template: instantiation with a feature-gated
// type at file scope diagnoses the unguarded type use.
template <Sized T>
constexpr int kSize = sizeof(T);

int gConcept_unguarded = kSize<Annotated>;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gConcept_unguarded' with the 'feature1' domain availability attribute to silence this error}}
int gConcept_guarded __attribute__((availability(domain:feature1, AVAIL))) = kSize<Annotated>;

// requires-clause that directly references a feature-gated function: the
// requires-clause is part of the function template declaration, so the
// diagnostic fires at the declaration with the function as the context.
template <class T> requires (constexpr_func() == 1)
// expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'requires_clause_unguarded' with the 'feature1' domain availability attribute to silence this error}}
void requires_clause_unguarded() {}

// Annotating the function template guards the trailing requires-clause use.
template <class T> __attribute__((availability(domain:feature1, AVAIL)))
void requires_clause_guarded() requires (constexpr_func() == 1) {}

// Default template argument that names a feature-gated type with a concept
// constraint: diagnosed at the template declaration.
template <Sized T = Annotated>
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'default_concept_arg' with the 'feature1' domain availability attribute to silence this error}}
void default_concept_arg() {}

// Annotated function template body: uses inside are guarded by the function's
// own annotation, even when the template parameter is concept-constrained.
template <Sized T>
__attribute__((availability(domain:feature1, AVAIL)))
void annotated_template_body(T) {
  constexpr_func();
  Annotated s;
}

// requires-expression inside an annotated function: inner uses are guarded.
__attribute__((availability(domain:feature1, AVAIL)))
void test_requires_guarded() {
  static_assert(requires { constexpr_func(); });
  static_assert(requires { Annotated{}; });
}

// requires-expression inside an unannotated function: inner uses diagnose.
void test_requires_unguarded() {
  static_assert(requires { constexpr_func(); });
  // expected-error@-1 {{cannot use 'constexpr_func' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'constexpr_func' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    static_assert(requires { constexpr_func(); });
}

// Class template constrained by a concept: instantiation with a feature-gated
// type without a guard diagnoses; with a __builtin_available guard, OK.
template <class T>
concept HasSize = requires { sizeof(T); };

template <HasSize T>
struct ConstrainedClass {
  T value;
};

ConstrainedClass<Annotated> gConstrained_unguarded;
// expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
// expected-note@-2 {{annotate 'gConstrained_unguarded' with the 'feature1' domain availability attribute to silence this error}}
ConstrainedClass<Annotated> gConstrained_guarded __attribute__((availability(domain:feature1, AVAIL)));

void test_constrained_class() {
  ConstrainedClass<Annotated> a;
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  if (__builtin_available(domain:feature1)) {
    ConstrainedClass<Annotated> b;
  }
}

// Concept-constrained function template called with a feature-gated argument
// inside an unannotated function is diagnosed.
template <Sized T>
void sized_template(T) {}

void test_sized_template() {
  sized_template(Annotated{});
  // expected-error@-1 {{cannot use 'Annotated' because feature 'feature1' is unavailable in this context}}
  // expected-note@-2 {{enclose 'Annotated' in a __builtin_available check}}
  if (__builtin_available(domain:feature1))
    sized_template(Annotated{});
}
