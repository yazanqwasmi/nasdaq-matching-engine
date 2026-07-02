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

add_library(nsq_book STATIC src/book/order_book.cpp)
target_link_libraries(nsq_book PUBLIC nsq_common)

enable_testing()
add_subdirectory(tests)
