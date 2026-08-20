add_executable(NativeFb2ZipTest
  NativeFb2ZipTest.cpp
  InflateOptimizationTest.cpp
  ${REPO_ROOT}/lib/Fb2/native/ZipReader.cpp
  ${REPO_ROOT}/lib/Fb2/native/Inflate.cpp
)

target_include_directories(NativeFb2ZipTest PRIVATE
  ${REPO_ROOT}/lib/Fb2/native
  ${REPO_ROOT}/src/reader/core/io
)

target_link_libraries(NativeFb2ZipTest PRIVATE
  inkmod_test_common
  GTest::gtest_main
)

gtest_discover_tests(NativeFb2ZipTest)
