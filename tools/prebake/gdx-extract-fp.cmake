# Injected into Torch's project() via -DCMAKE_PROJECT_torch_INCLUDE=<this file> for every
# gdx-extract build (release workflow, port/CMakeLists.txt superbuild, tools/prebake/README.md).
#
# The golden archive needs bit-identical float results on every host. Clang contracts a*b+c into
# a fused multiply-add by default, and only arm64 has FMA in its baseline ISA, so the arm64 slice
# of the macOS universal binary computed different FZX course checksums
# (torch/src/factories/fzerox/CourseFactory.cpp, one float expression truncated to int32) and
# missed the golden SHA-256 while the x86_64 slice matched. The N64 game never fuses either.
#
# Done here rather than through CMAKE_CXX_FLAGS because Torch overwrites that variable.
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
