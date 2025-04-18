// RUN: %clang_cc1 -triple arm64-apple-ios -std=c23 -fsyntax-only -verify -fptrauth-intrinsics %s
// RUN: %clang_cc1 -triple aarch64-linux-gnu -std=c23 -fsyntax-only -verify -fptrauth-intrinsics %s

_Static_assert(__has_extension(ptrauth_qualifier), "the ptrauth qualifier should be available");

#if __aarch64__
#define VALID_CODE_KEY 0
#define VALID_DATA_KEY 2
#define INVALID_KEY 200
#else
#error Provide these constants if you port this test
#endif


typedef int *intp;

int *__ptrauth(VALID_DATA_KEY, 1, 65535, "Foo") invalid13;       // expected-error{{unknown '__ptrauth' authentication option 'Foo'}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip", 41) invalid14; // expected-error{{'__ptrauth' qualifier must take between 1 and 4 arguments}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip,sign-and-strip") invalid15;     // expected-error{{repeated '__ptrauth' authentication mode}}
                                                                                // expected-note@-1{{previous '__ptrauth' authentication mode}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "isa-pointer,isa-pointer") invalid16;  // expected-error{{repeated '__ptrauth' authentication option}}
                                                                                // expected-note@-1{{previous '__ptrauth' authentication option}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "isa-pointer, isa-pointer") invalid17; // expected-error{{repeated '__ptrauth' authentication option}}
                                                                                // expected-note@-1{{previous '__ptrauth' authentication option}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip, , isa-pointer") invalid18;     // expected-error{{unexpected character ',' in '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip,") invalid19;                   // expected-error{{unexpected end of options parameter for __ptrauth}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, ",") invalid20;                        // expected-error{{unexpected character ',' in '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, ",,") invalid21;                       // expected-error{{unexpected character ',' in '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip isa-pointer") invalid22;        // expected-error{{missing comma between '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip\nisa-pointer") invalid23;       // expected-error{{missing comma between '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 65535, "strip"
                                         " isa-pointer") invalid24;              // expected-error{{missing comma between '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip,\n,isa-pointer") invalid25; // expected-error{{unexpected character ',' in '__ptrauth' options}}
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip,\t,isa-pointer") invalid26; // expected-error{{unexpected character ',' in '__ptrauth' options}}
void *__ptrauth(VALID_DATA_KEY, 1, 0, "authenticates-null-values") invalid27;    // expected-error{{globals with authenticated null values are currently unsupported}}
void *__ptrauth(VALID_DATA_KEY, 1, 0, "authenticates-null-values") invalid28 = 0; // expected-error{{globals with authenticated null values are currently unsupported}}

int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip") valid12;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip") valid13;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-auth") valid14;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "isa-pointer") valid15;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-auth,isa-pointer") valid15;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip,isa-pointer") valid16;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip,isa-pointer") valid17;
int *__ptrauth(VALID_DATA_KEY, 1, 0, " strip,isa-pointer") valid18;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip ,isa-pointer") valid19;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip, isa-pointer") valid20;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip,isa-pointer ") valid21;
int *__ptrauth(VALID_DATA_KEY, 1, 0, " strip") valid22;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "strip ") valid23;
int *__ptrauth(VALID_DATA_KEY, 1, 0, " strip ") valid24;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip,"
                                     "isa-pointer") valid25;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip"
                                     ",isa-pointer") valid26;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip\n,isa-pointer") valid27;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "sign-and-strip\t,isa-pointer") valid28;
int *__ptrauth(VALID_DATA_KEY, 1, 0, "") valid29;

struct S5 {
  intp __ptrauth(1, 1, 51, "authenticates-null-values") f0;
};

struct S5 globalS5; // expected-error {{globals with authenticated null values are currently unsupported}}
