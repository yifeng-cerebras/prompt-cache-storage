cmake_minimum_required(VERSION 3.20)
project(s3_rocksdb_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(S3GW_ENABLE_LTO "Enable link-time optimization" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

find_package(Boost REQUIRED COMPONENTS program_options system thread)
find_package(OpenSSL REQUIRED)

find_package(RocksDB CONFIG QUIET)
if (RocksDB_FOUND)
  set(ROCKSDB_TARGET RocksDB::rocksdb)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ROCKSDB REQUIRED rocksdb)
  set(ROCKSDB_INCLUDE_DIRS_FILTERED "")
  foreach(dir IN LISTS ROCKSDB_INCLUDE_DIRS)
    if (EXISTS "${dir}")
      list(APPEND ROCKSDB_INCLUDE_DIRS_FILTERED "${dir}")
    endif()
  endforeach()
  add_library(rocksdb_pkg INTERFACE)
  target_include_directories(rocksdb_pkg INTERFACE ${ROCKSDB_INCLUDE_DIRS_FILTERED})
  target_link_directories(rocksdb_pkg INTERFACE ${ROCKSDB_LIBRARY_DIRS})
  target_link_libraries(rocksdb_pkg INTERFACE ${ROCKSDB_LIBRARIES})
  target_compile_options(rocksdb_pkg INTERFACE ${ROCKSDB_CFLAGS_OTHER})
  set(ROCKSDB_TARGET rocksdb_pkg)
endif()

add_executable(s3gw
  src/main.cpp
  src/http_server.cpp
  src/s3_api.cpp
  src/storage.cpp
  src/sigv4.cpp
  src/metrics.cpp
  src/util.cpp
)

target_include_directories(s3gw PRIVATE src)

target_link_libraries(s3gw PRIVATE
  Boost::program_options
  Boost::system
  Boost::thread
  OpenSSL::Crypto
  ${ROCKSDB_TARGET}
)

add_executable(s3gw_test_range
  tests/test_range.cpp
  src/s3_api.cpp
  src/storage.cpp
  src/sigv4.cpp
  src/metrics.cpp
  src/util.cpp
)

target_include_directories(s3gw_test_range PRIVATE src)
target_link_libraries(s3gw_test_range PRIVATE
  Boost::system
  Boost::thread
  OpenSSL::Crypto
  ${ROCKSDB_TARGET}
)

if (S3GW_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if(ipo_supported)
    set_property(TARGET s3gw PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(s3gw PRIVATE stdc++fs)
  target_link_libraries(s3gw_test_range PRIVATE stdc++fs)
endif()

if (MSVC)
  target_compile_options(s3gw PRIVATE /W4)
else()
  target_compile_options(s3gw PRIVATE -Wall -Wextra -Wpedantic)
endif()
