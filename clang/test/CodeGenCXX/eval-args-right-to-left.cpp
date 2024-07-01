// RUN: %clang_cc1 -triple x86_64-apple-macosx14 -emit-llvm -feval-args-right-to-left -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-windows-msvc -emit-llvm -feval-args-right-to-left -o - %s | FileCheck %s

// CHECK: %[[STRUCT_S123:.*]] = type { i32, i32 }

struct S123 {
  S123(int, int);
  int a, b;
  S123(const S123 &);
  ~S123();
};

int operator &&(S123 s, int b);
int getInt();
void func(int, int);

// CHECK: define {{(dso_local )?}}void @{{.*}}test0{{.*}}()
// CHECK: %[[CALL:.*]] = call noundef i32 @{{.*}}getInt{{.*}}()
// CHECK: %[[CALL1:.*]] = call noundef i32 @{{.*}}getInt{{.*}}()
// CHECK: call void @{{.*}}func{{.*}}(i32 noundef %[[CALL1]], i32 noundef %[[CALL]])
void test0() {
  func(getInt(), getInt());
}

// CHECK: define {{(dso_local )?}}void @{{.*}}test1{{.*}}()
// CHECK: %[[S:.*]] = alloca %[[STRUCT_S123]], align 4
// CHECK: %[[CALL:.*]] = call noundef i32 @{{.*}}getInt{{.*}}()
// CHECK: %[[CALL1:.*]] = call noundef i32 @{{.*}}getInt{{.*}}()
// CHECK: call {{.*}}@{{.*}}S123{{.*}}(ptr noundef nonnull align 4 dereferenceable(8) %[[S]], i32 noundef %[[CALL]], i32 noundef %[[CALL1]])
void test1() {
  S123 s{getInt(), getInt()};
}


// CHECK: define {{(dso_local )?}}void @{{.*}}test2{{.*}}()
// CHECK: %[[AGG_TMP:.*]] = alloca %[[STRUCT_S123]], align 4
// CHECK: call {{.*}}@{{.*}}S123{{.*}}(ptr noundef nonnull align 4 dereferenceable(8) %[[AGG_TMP]], i32 noundef 1, i32 noundef 2)
// CHECK: %[[CALL:.*]] = call noundef i32 @{{.*}}getInt{{.*}}()
// CHECK: call noundef i32 @{{.*}}S123{{.*}}(ptr noundef %[[AGG_TMP]], i32 noundef %[[CALL]])
void test2() {
  S123(1, 2) && getInt();
}
