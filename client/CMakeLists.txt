set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")

include_directories(${CMAKE_CURRENT_BINARY_DIR}/../interface)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../client)

set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
add_library(kv739 SHARED client_interface.cpp)
    target_link_libraries(kv739
        Chubby_proto_grpc
        ${_PROTOBUF_LIBPROTOBUF}
        ${_REFLECTION}
        ${_GRPC_GRPCPP})