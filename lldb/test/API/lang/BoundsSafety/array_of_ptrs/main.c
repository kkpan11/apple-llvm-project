#include <stdlib.h>
#include <ptrcheck.h>

int *__bidi_indexable array_of_bounds_safety_pointers[2];

int main() {
  array_of_bounds_safety_pointers[0] = (int *)malloc(16); // break here 1
  return 0; // break here 2
}
