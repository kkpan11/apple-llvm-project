// RUN: %clang_cc1 -triple arm64-apple-macosx -fblocks -ffeature-availability=feature1:on -ffeature-availability=feature2:off -ffeature-availability=feature3:on -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple arm64-apple-macosx -fblocks -emit-llvm -o - -DUSE_DOMAIN %s | FileCheck %s

#define AVAIL 0

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
static struct Domain feature3 __attribute__((availability_domain(feature3))) = {On, 0};
#endif

// CHECK: @"OBJC_IVAR_$_C0._prop0" = hidden global i32 8, section "__DATA, __objc_ivar", align 4
// CHECK-NEXT: @_objc_empty_cache = external global %struct._objc_cache
// CHECK-NEXT: @_objc_empty_vtable = external global ptr
// CHECK-NEXT: @"OBJC_CLASS_$_C0" = global %struct._class_t { ptr @"OBJC_METACLASS_$_C0", ptr null, ptr @_objc_empty_cache, ptr @_objc_empty_vtable, ptr @"_OBJC_CLASS_RO_$_C0" }, section "__DATA, __objc_data", align 8
// CHECK-NEXT: @"OBJC_METACLASS_$_C0" = global %struct._class_t { ptr @"OBJC_METACLASS_$_C0", ptr @"OBJC_CLASS_$_C0", ptr @_objc_empty_cache, ptr @_objc_empty_vtable, ptr @"_OBJC_METACLASS_RO_$_C0" }, section "__DATA, __objc_data", align 8
// CHECK-NEXT: @OBJC_CLASS_NAME_ = private unnamed_addr constant [3 x i8] c"C0\00", section "__TEXT,__objc_classname,cstring_literals", align 1
// CHECK-NEXT: @"_OBJC_METACLASS_RO_$_C0" = internal global %struct._class_ro_t { i32 3, i32 40, i32 40, ptr null, ptr @OBJC_CLASS_NAME_, ptr null, ptr null, ptr null, ptr null, ptr null }, section "__DATA, __objc_const", align 8
// CHECK-NEXT: @OBJC_METH_VAR_NAME_ = private unnamed_addr constant [3 x i8] c"m0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_TYPE_ = private unnamed_addr constant [8 x i8] c"v16@0:8\00", section "__TEXT,__objc_methtype,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_NAME_.1 = private unnamed_addr constant [3 x i8] c"m1\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_NAME_.2 = private unnamed_addr constant [6 x i8] c"prop0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_TYPE_.3 = private unnamed_addr constant [8 x i8] c"i16@0:8\00", section "__TEXT,__objc_methtype,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_NAME_.4 = private unnamed_addr constant [10 x i8] c"setProp0:\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_TYPE_.5 = private unnamed_addr constant [11 x i8] c"v20@0:8i16\00", section "__TEXT,__objc_methtype,cstring_literals", align 1
// CHECK-NEXT: @"_OBJC_$_INSTANCE_METHODS_C0" = internal global { i32, i32, [4 x %struct._objc_method] } { i32 24, i32 7, [4 x %struct._objc_method] [%struct._objc_method { ptr @OBJC_METH_VAR_NAME_, ptr @OBJC_METH_VAR_TYPE_, ptr @"\01-[C0 m0]" }, %struct._objc_method { ptr @OBJC_METH_VAR_NAME_.1, ptr @OBJC_METH_VAR_TYPE_, ptr @"\01-[C0 m1]" }, %struct._objc_method { ptr @OBJC_METH_VAR_NAME_.2, ptr @OBJC_METH_VAR_TYPE_.3, ptr @"\01-[C0 prop0]" }, %struct._objc_method { ptr @OBJC_METH_VAR_NAME_.4, ptr @OBJC_METH_VAR_TYPE_.5, ptr @"\01-[C0 setProp0:]" }] }, section "__DATA, __objc_const", align 8
// CHECK-NEXT: @"OBJC_IVAR_$_C0.ivar0" = global i32 0, section "__DATA, __objc_ivar", align 4
// CHECK-NEXT: @OBJC_METH_VAR_NAME_.6 = private unnamed_addr constant [6 x i8] c"ivar0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_TYPE_.7 = private unnamed_addr constant [2 x i8] c"i\00", section "__TEXT,__objc_methtype,cstring_literals", align 1
// CHECK-NEXT: @OBJC_METH_VAR_NAME_.8 = private unnamed_addr constant [7 x i8] c"_prop0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @"_OBJC_$_INSTANCE_VARIABLES_C0" = internal global { i32, i32, [2 x %struct._ivar_t] } { i32 32, i32 2, [2 x %struct._ivar_t] [%struct._ivar_t { ptr @"OBJC_IVAR_$_C0.ivar0", ptr @OBJC_METH_VAR_NAME_.6, ptr @OBJC_METH_VAR_TYPE_.7, i32 2, i32 4 }, %struct._ivar_t { ptr @"OBJC_IVAR_$_C0._prop0", ptr @OBJC_METH_VAR_NAME_.8, ptr @OBJC_METH_VAR_TYPE_.7, i32 2, i32 4 }] }, section "__DATA, __objc_const", align 8
// CHECK-NEXT: @OBJC_PROP_NAME_ATTR_ = private unnamed_addr constant [6 x i8] c"prop0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @OBJC_PROP_NAME_ATTR_.9 = private unnamed_addr constant [11 x i8] c"Ti,V_prop0\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-NEXT: @"_OBJC_$_PROP_LIST_C0" = internal global { i32, i32, [1 x %struct._prop_t] } { i32 16, i32 1, [1 x %struct._prop_t] [%struct._prop_t { ptr @OBJC_PROP_NAME_ATTR_, ptr @OBJC_PROP_NAME_ATTR_.9 }] }, section "__DATA, __objc_const", align 8
// CHECK-NEXT: @"_OBJC_CLASS_RO_$_C0" = internal global %struct._class_ro_t { i32 2, i32 0, i32 16, ptr null, ptr @OBJC_CLASS_NAME_, ptr @"_OBJC_$_INSTANCE_METHODS_C0", ptr null, ptr @"_OBJC_$_INSTANCE_VARIABLES_C0", ptr null, ptr @"_OBJC_$_PROP_LIST_C0" }, section "__DATA, __objc_const", align 8
// CHECK-NEXT: @OBJC_CLASS_NAME_.10 = private unnamed_addr constant [5 x i8] c"Cat0\00", section "__TEXT,__objc_classname,cstring_literals", align 1
// CHECK-NEXT: @"OBJC_LABEL_CLASS_$" = private global [1 x ptr] [ptr @"OBJC_CLASS_$_C0"], section "__DATA,__objc_classlist,regular,no_dead_strip", align 8

__attribute__((availability(domain=feature2, AVAIL))) int func1(void);
__attribute__((availability(domain=feature3, AVAIL))) int func3(void);

__attribute__((availability(domain=feature1, AVAIL)))
@interface C0 {
  int ivar0 __attribute__((availability(domain=feature3, AVAIL)));
  int ivar1 __attribute__((availability(domain=feature2, AVAIL)));
}
@property int prop0 __attribute__((availability(domain=feature3, AVAIL)));
@property int prop1 __attribute__((availability(domain=feature2, AVAIL)));
-(void)m0;
-(void)m1 __attribute__((availability(domain=feature3, AVAIL)));
-(void)m2 __attribute__((availability(domain=feature2, AVAIL)));
@end

// CHECK: define internal void @"\01-[C0 m0]"(
// CHECK: define internal void @"\01-[C0 m1]"(
// CHECK: call i32 @func3()
// CHECK-NOT: [C0 m2]

@implementation C0
-(void)m0 {
}
-(void)m1 {
  func3();
}
-(void)m2 {
  func1();
}
@end

@interface C0(Cat0)
@end

@implementation C0(Cat0)
@end

__attribute__((availability(domain=feature2, AVAIL)))
@interface C1
-(void)m1;
@end

// CHECK-NOT: [C1 m1]
@implementation C1
-(void)m1 {
}
@end

@interface C1(Cat1)
@end

@implementation C1(Cat1)
@end

