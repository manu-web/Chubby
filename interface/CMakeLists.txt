get_filename_component(Chubby_proto "${CMAKE_CURRENT_SOURCE_DIR}/Chubby.proto" ABSOLUTE)
get_filename_component(Chubby_proto_path "${Chubby_proto}" PATH)
get_filename_component(Paxos_proto "${CMAKE_CURRENT_SOURCE_DIR}/Paxos.proto" ABSOLUTE)
get_filename_component(Paxos_proto_path "${Paxos_proto}" PATH)

set(Chubby_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.pb.cc")
set(Chubby_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.pb.h")
set(Chubby_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.grpc.pb.cc")
set(Chubby_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.grpc.pb.h")

set(Paxos_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/Paxos.pb.cc")
set(Paxos_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Paxos.pb.h")
set(Paxos_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/Paxos.grpc.pb.cc")
set(Paxos_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Paxos.grpc.pb.h")

add_custom_command(
      OUTPUT "${Chubby_proto_srcs}" "${Chubby_proto_hdrs}" "${Chubby_grpc_srcs}" "${Chubby_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${Chubby_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${Chubby_proto}"
      DEPENDS "${Chubby_proto}")


add_custom_command(
  OUTPUT "${Paxos_proto_srcs}" "${Paxos_proto_hdrs}" "${Paxos_grpc_srcs}" "${Paxos_grpc_hdrs}"
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
    --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${Paxos_proto_path}"
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
    "${Paxos_proto}"
  DEPENDS "${Paxos_proto}")

include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(Chubby_proto_grpc
  ${Chubby_proto_srcs}
  ${Chubby_proto_hdrs}
  ${Chubby_grpc_srcs}
  ${Chubby_grpc_hdrs})

add_library(Paxos_proto_grpc
${Paxos_proto_srcs}
${Paxos_proto_hdrs}
${Paxos_grpc_srcs}
${Paxos_grpc_hdrs})


target_link_libraries(Chubby_proto_grpc
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

target_link_libraries(Paxos_proto_grpc
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})