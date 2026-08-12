add_executable(reader_core_io_test
  IByteReaderTest.cpp
  ../../src/reader/core/BookIdentity.cpp
  ../../src/reader/core/DocumentProgress.cpp
  ../../src/reader/core/ReaderJobQueue.cpp
  ../../src/reader/document/XhtmlTextCursor.cpp
  ../../src/reader/layout/LayoutEngine.cpp
  ../../src/reader/render/RenderPageRenderer.cpp
  ../../src/reader/session/ReaderSession.cpp
  ../../src/reader/session/ReaderState.cpp
)

target_link_libraries(reader_core_io_test PRIVATE
  GTest::gtest_main
  inkmod_test_common
)

gtest_discover_tests(reader_core_io_test)
