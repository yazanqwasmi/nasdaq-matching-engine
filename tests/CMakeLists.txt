include(GoogleTest)

function(nsq_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

nsq_test(common_test nsq_common)
nsq_test(book_test nsq_book)
nsq_test(book_random_test nsq_book)
