add_executable(fs_helpers_test
  FsHelpersTest.cpp
  ../../lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(fs_helpers_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(fs_helpers_test PRIVATE
  GTest::gtest_main
  inkmod_test_common
)

gtest_discover_tests(fs_helpers_test)
