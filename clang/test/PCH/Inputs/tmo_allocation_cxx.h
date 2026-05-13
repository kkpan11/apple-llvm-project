
#define _TYPED(rewrite_target, type_param_pos) __attribute__((typed_memory_operation(rewrite_target, type_param_pos)))

extern "C" {

void *typed_malloc(__SIZE_TYPE__ size, unsigned long long);
void *malloc(__SIZE_TYPE__ size) _TYPED(typed_malloc, 1);

struct S1 {
  void *p;
  int i;
  int j;
  void (*fptr)();
};

}

void *non_tmo_function(__SIZE_TYPE__);
void *in_pch_non_tmo_call() {
  return non_tmo_function(sizeof(S1));
}

inline void *in_pch_non_tmo_call_inline() {
  return non_tmo_function(sizeof(S1));
}

void in_pch_function() {
  void *iptr_failed_inference = malloc(1000); // #pch_failed_inference
  // expected-warning@#pch_failed_inference {{could not infer allocation type in call to 'malloc'}}
  // expected-note@#pch_failed_inference {{unable to infer allocation type from expression '1000'}}
  void *iptr1 = malloc(sizeof(int)); // #pch_iptr1
  // expected-remark@#pch_iptr1 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_iptr1 {{inferred 'int' from expression 'sizeof(int)'}}
  // expected-note@#pch_iptr1 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  int *iptr2 = (int *)malloc(sizeof(int)); // #pch_iptr2
  // expected-remark@#pch_iptr2 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_iptr2 {{inferred 'int' from expression 'sizeof(int)'}}
  // expected-note@#pch_iptr2 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  int *iptr3 = (int *)malloc(100); // #pch_iptr3
  // expected-remark@#pch_iptr3 {{passing TMO information for type 'int' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_iptr3 {{inferred 'int' from cast of result from call to '(int *)malloc(100)'}}
  // expected-note@#pch_iptr3 {{encoding 'int' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
  void *s1ptr1 = malloc(sizeof(S1)); // #pch_s1ptr1
  // expected-remark@#pch_s1ptr1 {{passing TMO information for type 'S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_s1ptr1 {{inferred 'S1' from expression 'sizeof(S1)'}}
  // expected-note@#pch_s1ptr1 {{encoding 'S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
  S1 *s1ptr2 = (S1 *)malloc(sizeof(S1)); // #pch_s1ptr2
  // expected-remark@#pch_s1ptr2 {{passing TMO information for type 'S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_s1ptr2 {{inferred 'S1' from expression 'sizeof(S1)'}}
  // expected-note@#pch_s1ptr2 {{encoding 'S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
  S1 *s1ptr3 = (S1 *)malloc(100); // #pch_s1ptr3
  // expected-remark@#pch_s1ptr3 {{passing TMO information for type 'S1' to 'typed_malloc' (retargeted from 'malloc')}}
  // expected-note@#pch_s1ptr3 {{inferred 'S1' from cast of result from call to '(S1 *)malloc(100)'}}
  // expected-note@#pch_s1ptr3 {{encoding 'S1' as 74309672738655766. { "Summary": { "LayoutSemantics": [ "AnonymousPointer", "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 4009135638 }}}
}

template <class T> void in_pch_template_function() {
  void *tptr_failed_inference = malloc(1000); // #template_failed_inference
  void *tptr1 = malloc(sizeof(T)); // #tptr1
  T *tptr2 = (T *)malloc(sizeof(T)); // #tptr2
  T *tptr3 = (T *)malloc(100); // #tptr3
  T *tptr4 = static_cast<T *>(malloc(100)); // #tptr4
  T *tptr5 = reinterpret_cast<T *>(malloc(100)); // #tptr5
}

template void in_pch_template_function<float>(); //#float_instantiation
// expected-note@#float_instantiation {{in instantiation of function template specialization 'in_pch_template_function<float>' requested here}}
// expected-warning@#template_failed_inference {{could not infer allocation type in call to 'malloc'}}
// expected-note@#template_failed_inference {{unable to infer allocation type from expression '1000'}}
// expected-remark@#tptr1 {{passing TMO information for type 'float' to 'typed_malloc' (retargeted from 'malloc')}}
// expected-note@#tptr1 {{inferred 'float' from expression 'sizeof(float)'}}
// expected-note@#tptr1 {{encoding 'float' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
// expected-remark@#tptr2 {{passing TMO information for type 'float' to 'typed_malloc' (retargeted from 'malloc')}}
// expected-note@#tptr2 {{inferred 'float' from expression 'sizeof(float)'}}
// expected-note@#tptr2 {{encoding 'float' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
// expected-remark@#tptr3 {{passing TMO information for type 'float' to 'typed_malloc' (retargeted from 'malloc')}}
// expected-note@#tptr3 {{inferred 'float' from cast of result from call to '(float *)malloc(100)'}}
// expected-note@#tptr3 {{encoding 'float' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
// expected-remark@#tptr4 {{passing TMO information for type 'float' to 'typed_malloc' (retargeted from 'malloc')}}
// expected-note@#tptr4 {{inferred 'float' from cast of result from call to 'static_cast<float *>(malloc(100))'}}
// expected-note@#tptr4 {{encoding 'float' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
// expected-remark@#tptr5 {{passing TMO information for type 'float' to 'typed_malloc' (retargeted from 'malloc')}}
// expected-note@#tptr5 {{inferred 'float' from cast of result from call to 'reinterpret_cast<float *>(malloc(100))'}}
// expected-note@#tptr5 {{encoding 'float' as 72057870300512784. { "Summary": { "LayoutSemantics": [ "GenericData" ], "TypeFlags": [ ], "TypeKind": "KindC", "CallsiteFlags": [ "FixedSize" ] }, "TypeHash": 1384677904 }}}
