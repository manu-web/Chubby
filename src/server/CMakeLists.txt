set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

include_directories(${CMAKE_CURRENT_BINARY_DIR}/../../interface)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../common)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../include)

find_library(ROCKSDB_LIBRARY rocksdb REQUIRED)
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h REQUIRED)
include_directories(${ROCKSDB_INCLUDE_DIR})

add_executable(chubby_server server.cpp)
    target_link_libraries(chubby_server
        Chubby_proto_grpc
        threadPoolLib
        rocksDBlib
        paxos_server
        ${SQLite3_LIBRARIES}
        ${_PROTOBUF_LIBPROTOBUF}
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${ROCKSDB_LIBRARY}) 