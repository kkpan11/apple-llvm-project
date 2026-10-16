struct S {
  int x = 7;
  int foo() { return x; }
};

int main() {
  S s;
  int (S::*mp)() = &S::foo;
  return (s.*mp)(); // break here
}
