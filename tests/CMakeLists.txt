include(GoogleTest)

function(nsq_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  # PRE_TEST: enumerate tests at ctest time, not by running each binary during
  # the (parallel, sanitized) build — avoids build-time discovery flakiness.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

nsq_test(common_test nsq_common)
nsq_test(book_test nsq_book)
nsq_test(book_random_test nsq_book)
nsq_test(ouch_test nsq_common)
nsq_test(soup_test nsq_soup)
nsq_test(itch_test nsq_common)
nsq_test(mold_test nsq_mold)
nsq_test(engine_test nsq_engine)
nsq_test(gateway_test nsq_gateway)
nsq_test(feed_test nsq_feed nsq_client)
nsq_test(fastbook_test nsq_book)
# The allocation-counting test replaces global operator new/delete, which is
# incompatible with the sanitizers' own allocator interceptors (and moot when
# they replace the allocator anyway). Build it only in non-sanitized configs;
# it still runs on the macOS and Linux Release legs.
if(NOT SANITIZE_ADDRESS AND NOT SANITIZE_THREAD)
  nsq_test(fastbook_alloc_test nsq_book)
endif()
nsq_test(ouch_client_test nsq_client nsq_gateway)
nsq_test(capture_test nsq_feed nsq_client)
nsq_test(latency_test nsq_common)
nsq_test(raw_itch_test nsq_client)
nsq_test(itch_file_test nsq_client)
