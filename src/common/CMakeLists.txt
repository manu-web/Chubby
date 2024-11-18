set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

find_library(ROCKSDB_LIBRARY rocksdb REQUIRED)
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h REQUIRED)
include_directories(${ROCKSDB_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src/include)

add_library(threadPoolLib thread_pool.cc)

add_library(rocksDBlib rocksdb_wrapper.cc)
target_link_libraries(rocksDBlib
        ${ROCKSDB_LIBRARY}) 