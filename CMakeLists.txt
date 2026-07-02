cmake_minimum_required(VERSION 3.24)
project(nasdaq_sim_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Werror)

option(SANITIZE_ADDRESS "Build with AddressSanitizer + UBSan" OFF)
option(SANITIZE_THREAD "Build with ThreadSanitizer" OFF)

if(SANITIZE_ADDRESS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(SANITIZE_THREAD)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(googletest)

# Header-only common library.
add_library(nsq_common INTERFACE)
target_include_directories(nsq_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(nsq_book STATIC src/book/order_book.cpp src/book/fast_book.cpp)
target_link_libraries(nsq_book PUBLIC nsq_common)

add_library(nsq_soup STATIC src/soup/soup.cpp)
target_link_libraries(nsq_soup PUBLIC nsq_common)

add_library(nsq_mold STATIC src/mold/mold.cpp)
target_link_libraries(nsq_mold PUBLIC nsq_common)

add_library(nsq_engine STATIC src/engine/engine.cpp)
target_link_libraries(nsq_engine PUBLIC nsq_book)

add_library(nsq_gateway STATIC src/gateway/gateway.cpp)
target_link_libraries(nsq_gateway PUBLIC nsq_engine nsq_soup)

add_library(nsq_feed STATIC src/feed/feed.cpp src/feed/capture.cpp)
target_link_libraries(nsq_feed PUBLIC nsq_engine nsq_mold)

add_library(nsq_client STATIC src/client/itch_book.cpp src/client/ouch_client.cpp)
target_link_libraries(nsq_client PUBLIC nsq_book nsq_mold nsq_soup)

add_executable(demo src/apps/demo.cpp)
target_link_libraries(demo PRIVATE nsq_book)

add_executable(exchanged src/apps/exchanged.cpp)
target_link_libraries(exchanged PRIVATE nsq_gateway nsq_feed)

add_executable(flowgen src/apps/flowgen.cpp)
target_link_libraries(flowgen PRIVATE nsq_soup)

add_executable(itchlisten src/apps/itchlisten.cpp)
target_link_libraries(itchlisten PRIVATE nsq_client)

add_executable(marketsim src/apps/marketsim.cpp)
target_link_libraries(marketsim PRIVATE nsq_client)

add_executable(itchview src/apps/itchview.cpp)
target_link_libraries(itchview PRIVATE nsq_client)

add_executable(itchreplay src/apps/itchreplay.cpp)
target_link_libraries(itchreplay PRIVATE nsq_feed)

add_executable(bench src/apps/bench.cpp)
target_link_libraries(bench PRIVATE nsq_gateway nsq_feed nsq_client)

add_executable(itchfile src/apps/itchfile.cpp)
target_link_libraries(itchfile PRIVATE nsq_client)

enable_testing()
add_subdirectory(tests)
