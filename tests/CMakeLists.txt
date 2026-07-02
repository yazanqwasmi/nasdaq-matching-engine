add_executable(common_test common_test.cpp)
target_link_libraries(common_test PRIVATE nsq_common GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(common_test)
