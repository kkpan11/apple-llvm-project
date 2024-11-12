// RUN: %clang_cc1 -fblocks -ffeature-availability=feature1:on -ffeature-availability=feature2:on -fsyntax-only -verify %s
// RUN: %clang_cc1 -fblocks -fsyntax-only -verify -DUSE_DOMAIN %s

#define AVAIL 0
#define UNAVAIL 1

#ifdef USE_DOMAIN
enum AvailabilityDomainKind {
  On, Off, Dynamic
};

struct Domain {
  const enum AvailabilityDomainKind kind;
  int (* const pred)(void);
};

int pred1(void);

static struct Domain feature1 __attribute__((availability_domain(feature1))) = {On, 0};
static struct Domain feature2 __attribute__((availability_domain(feature2))) = {Off, 0};
#endif

void func0();
__attribute__((availability(domain=feature1, AVAIL))) int func1();
__attribute__((availability(domain=feature2, AVAIL))) void func2();
__attribute__((availability(domain=feature1, UNAVAIL))) void func3();

__attribute__((availability(domain=feature1, AVAIL))) extern int g0;
__attribute__((availability(domain=feature1, AVAIL))) int g1;

struct S0 {
  __attribute__((availability(domain=feature1, AVAIL))) int d1;
};

struct __attribute__((availability(domain=feature1, AVAIL))) S1 {
  __attribute__((availability(domain=feature2, AVAIL))) int d1;
};

struct S2 {
  struct S1 s1; // expected-error{{use of 'S1' needs guard for feature feature1}}
};

__attribute__((availability(domain=feature1, AVAIL)))
@interface Base0
@end

@interface Derived0 : Base0 { // expected-error {{use of 'Base0' needs guard for feature feature1}}
  struct S1 *ivar0; // expected-error {{use of 'S1' needs guard for feature feature1}}
}
@property struct S1 *p0; // expected-error {{use of 'S1' needs guard for feature feature1}}
@property int p1 __attribute__((availability(domain=feature1, AVAIL)));
@end

@interface Derived0()
@property struct S1 *p0_Ext; // expected-error {{use of 'S1' needs guard for feature feature1}}
@end

@implementation Derived0
-(void)m0 {
  func1(); // expected-error {{use of 'func1' needs guard for feature feature1}}
  func3(); // expected-error {{use of 'func3' needs guard for feature feature1}}
}
-(void)m1 {
  self.p1 = 1; // expected-error {{use of 'setP1:' needs guard for feature feature1}}
}
@end

@interface Derived0(C0)
@property struct S1 *p0_C0; // expected-error {{use of 'S1' needs guard for feature feature1}}
@end

__attribute__((availability(domain=feature1, AVAIL)))
@interface Derived1 : Base0 {
  struct S1 *ivar0;
}
@property struct S1 *p0;
@end

@interface Derived1()
@property struct S1 *p0_Ext;
@end

@implementation Derived1
-(void)m0 {
  func1();
}
@end

@interface Derived1(C0)
@property struct S1 *p0_C0;
@end

@protocol P0
@property struct S1 *p0; // expected-error {{use of 'S1' needs guard for feature feature1}}
@end

void test0(struct S0 *s0) {
  func0();
  func1(); // expected-error{{use of 'func1' needs guard for feature feature1}}
  func3(); // expected-error{{use of 'func3' needs guard for feature feature1}}
  s0->d1 = 0; // expected-error{{use of 'd1' needs guard for feature feature1}}

  if (__builtin_available(domain=feature1)) {
    if (__builtin_available(domain=feature2))
      func1();
    func3(); // expected-error{{use of 'func3' needs guard for feature feature1}}
  } else {
    func1(); // expected-error{{use of 'func1' needs guard for feature feature1}}
    func3();
  }

  if (__builtin_available(domain=feature2))
    func1(); // expected-error{{use of 'func1' needs guard for feature feature1}}

  if (__builtin_available(domain=feature1))
    label0: // expected-error{{labels cannot appear in regions conditionally guarded by features}}
      ;

  if (__builtin_available(domain=feature1)) {
    ^{
      func1();
      func2(); // expected-error{{use of 'func2' needs guard for feature feature2}}
    }();
  }

  label1:
    ;
}

__attribute__((availability(domain=feature1, AVAIL)))
void test1(struct S0 *s0) {
  func0();
  func1();
  s0->d1 = 0;

  if (__builtin_available(domain=feature1))
    if (__builtin_available(domain=feature2))
      func1();

  if (__builtin_available(domain=feature2))
    func1();

  if (__builtin_available(domain=feature1))
    label0: // expected-error{{labels cannot appear in regions conditionally guarded by features}}
      ;

  if (__builtin_available(domain=feature1)) {
    ^{
      func1();
      func2(); // expected-error{{use of 'func2' needs guard for feature feature2}}
    }();
  }

  label1:
    ;
}

void test2(struct S1 *s1) { // expected-error{{use of 'S1' needs guard for feature feature1}}
  s1->d1 = 0; // expected-error{{use of 'd1' needs guard for feature feature2}}

  if (__builtin_available(domain=feature1)) {
    s1->d1 = 0; // expected-error{{use of 'd1' needs guard for feature feature2}}
  }

  if (__builtin_available(domain=feature2)) {
    s1->d1 = 0;
  }

  if (__builtin_available(domain=feature1))
    if (__builtin_available(domain=feature2)) {
      s1->d1 = 0;
    }
}
