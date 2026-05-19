// RUN: %clang_cc1 -triple arm64-apple-macosx -std=c++17 -emit-llvm -o - %s | FileCheck %s

#include <availability_domain.h>

#define AVAIL 0

CLANG_ENABLED_AVAILABILITY_DOMAIN(feature1);
CLANG_DISABLED_AVAILABILITY_DOMAIN(feature2);

__attribute__((availability(domain:feature1, AVAIL))) int f1();
__attribute__((availability(domain:feature2, AVAIL))) int f2();

// Module-global guard variable for WithStaticLocal::avail's static local;
// must be matched before any function CHECK-LABEL because globals come first
// in the IR.
// CHECK: @_ZGVZN15WithStaticLocal5availEvE1n
// CHECK-NOT: @_ZGVZN15WithStaticLocal7unavailEvE1n

// Annotated member function.
struct S {
  __attribute__((availability(domain:feature1, AVAIL))) int m_avail();
  __attribute__((availability(domain:feature2, AVAIL))) int m_unavail();
};

// CHECK-LABEL: define {{.*}}@_Z16test_member_callP1S
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN1S7m_availEv
// CHECK-NOT: @_ZN1S9m_unavailEv
void test_member_call(S *p) {
  if (__builtin_available(domain:feature1))
    p->m_avail();
  if (__builtin_available(domain:feature2))
    p->m_unavail();
}

// Annotated class with a constructor and a destructor.
struct __attribute__((availability(domain:feature1, AVAIL))) Avail {
  Avail();
  ~Avail();
};

struct __attribute__((availability(domain:feature2, AVAIL))) Unavail {
  Unavail();
  ~Unavail();
};

// CHECK-LABEL: define {{.*}}@_Z19test_class_lifetimev
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN5AvailC1Ev
// CHECK: call {{.*}}@_ZN5AvailD1Ev
// CHECK-NOT: @_ZN7UnavailC1Ev
// CHECK-NOT: @_ZN7UnavailD1Ev
void test_class_lifetime() {
  if (__builtin_available(domain:feature1)) {
    Avail a;
  }
  if (__builtin_available(domain:feature2)) {
    Unavail u;
  }
}

// Constructor with a member initializer that references an annotated
// function. Exercises the init-list traversal in IssueDiagnostics: the
// ctor body must contain the call to f1.
struct WithInit {
  __attribute__((availability(domain:feature1, AVAIL)))
  WithInit() : x(f1()) {}
  int x;
};

// CHECK-LABEL: define {{.*}}@_Z18test_ctor_initlistv
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN8WithInitC1Ev
void test_ctor_initlist() {
  if (__builtin_available(domain:feature1)) {
    WithInit w;
  }
}

// Annotated class template + specialization. Exercises the new
// DiagnoseFeatureAvailabilityOfDecl call at instantiation time.
template <class T>
struct __attribute__((availability(domain:feature1, AVAIL))) AvailTmpl {
  T v;
  AvailTmpl() : v(0) {}
};

template <class T>
struct __attribute__((availability(domain:feature2, AVAIL))) UnavailTmpl {
  T v;
  UnavailTmpl() : v(0) {}
};

// CHECK-LABEL: define {{.*}}@_Z13test_templatev
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN9AvailTmplIiEC1Ev
// CHECK-NOT: @_ZN11UnavailTmplIiEC1Ev
void test_template() {
  if (__builtin_available(domain:feature1)) {
    AvailTmpl<int> t;
  }
  if (__builtin_available(domain:feature2)) {
    UnavailTmpl<int> t;
  }
}

// Default template argument referring to an annotated type. Exercises
// the ClassTemplateDecl -> getTemplatedDecl() redirect.
template <class T = AvailTmpl<int>>
struct __attribute__((availability(domain:feature1, AVAIL))) UsesDefault {
  T v;
};

// CHECK-LABEL: define {{.*}}@_Z21test_default_tmpl_argv
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN11UsesDefaultI9AvailTmplIiEEC1Ev
void test_default_tmpl_arg() {
  if (__builtin_available(domain:feature1)) {
    UsesDefault<> u;
  }
}

// Lambda inside an annotated function. The closure type's operator() is
// emitted as part of the enclosing function.
// CHECK-LABEL: define {{.*}}@_Z11test_lambdav
// CHECK: call {{.*}}@"_ZZ11test_lambdavENK{{.*}}clEv"
//
// CHECK-LABEL: define {{.*}}@"_ZZ11test_lambdavENK{{.*}}clEv"
// CHECK: call {{.*}}@_Z2f1v
__attribute__((availability(domain:feature1, AVAIL)))
int test_lambda() {
  auto lam = []() { return f1(); };
  return lam();
}

// Static local in an annotated method gets a guard variable only when
// the feature is on.
struct WithStaticLocal {
  __attribute__((availability(domain:feature1, AVAIL)))
  int avail() {
    static int n = f1();
    return n;
  }
  __attribute__((availability(domain:feature2, AVAIL)))
  int unavail() {
    static int n = f2();
    return n;
  }
};

// CHECK-LABEL: define {{.*}}@_Z17test_static_localP15WithStaticLocal
// CHECK-NOT: br
// CHECK: call {{.*}}@_ZN15WithStaticLocal5availEv
// CHECK-NOT: @_ZN15WithStaticLocal7unavailEv
void test_static_local(WithStaticLocal *p) {
  if (__builtin_available(domain:feature1))
    p->avail();
  if (__builtin_available(domain:feature2))
    p->unavail();
}

// The base-object constructor for WithInit holds the f1() call from the
// member initializer. Emitted late in the module, after test_static_local,
// so this CHECK must come last.
// CHECK-LABEL: define {{.*}}@_ZN8WithInitC2Ev
// CHECK: call {{.*}}@_Z2f1v
