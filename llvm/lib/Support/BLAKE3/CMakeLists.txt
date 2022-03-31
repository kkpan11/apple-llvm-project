set(LLVM_BLAKE3_FILES
  blake3.c
  blake3_dispatch.c
  blake3_portable.c
)

# The BLAKE3 team recommends using the assembly versions, from the README:
#
# "For each of the x86 SIMD instruction sets, four versions are available:
# three flavors of assembly (Unix, Windows MSVC, and Windows GNU) and one
# version using C intrinsics. The assembly versions are generally
# preferred. They perform better, they perform more consistently across
# different compilers, and they build more quickly."

if (MSVC)
  check_symbol_exists(_M_X64 "" IS_X64)
  check_symbol_exists(_M_ARM64 "" IS_ARM64)
else()
  check_symbol_exists(__x86_64__ "" IS_X64)
  check_symbol_exists(__aarch64__ "" IS_ARM64)

  if (IS_X64)
    # Clang-6 needs this flag.
    set_source_files_properties(blake3_avx512_x86-64_windows_gnu.S
     PROPERTIES COMPILE_OPTIONS "-mavx512vl")
    set_source_files_properties(blake3_avx512_x86-64_unix.S
     PROPERTIES COMPILE_OPTIONS "-mavx512vl")
  endif()
endif()

if (IS_X64)
  if (MSVC)
    enable_language(ASM_MASM)
    list(APPEND LLVM_BLAKE3_FILES
      blake3_sse2_x86-64_windows_msvc.asm
      blake3_sse41_x86-64_windows_msvc.asm
      blake3_avx2_x86-64_windows_msvc.asm
      blake3_avx512_x86-64_windows_msvc.asm
    )
  elseif(WIN32)
    list(APPEND LLVM_BLAKE3_FILES
      blake3_sse2_x86-64_windows_gnu.S
      blake3_sse41_x86-64_windows_gnu.S
      blake3_avx2_x86-64_windows_gnu.S
      blake3_avx512_x86-64_windows_gnu.S
    )
  else()
    list(APPEND LLVM_BLAKE3_FILES
      blake3_sse2_x86-64_unix.S
      blake3_sse41_x86-64_unix.S
      blake3_avx2_x86-64_unix.S
      blake3_avx512_x86-64_unix.S
    )
  endif()
endif()

if (IS_ARM64)
  list(APPEND LLVM_BLAKE3_FILES
    blake3_neon.c
  )
endif()

add_library(LLVMSupportBlake3 OBJECT EXCLUDE_FROM_ALL ${LLVM_BLAKE3_FILES})
llvm_update_compile_flags(LLVMSupportBlake3)
