get_filename_component(Chubby_proto "${CMAKE_CURRENT_SOURCE_DIR}/Chubby.proto" ABSOLUTE)
get_filename_component(Chubby_proto_path "${Chubby_proto}" PATH)

set(Chubby_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.pb.cc")
set(Chubby_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.pb.h")
set(Chubby_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.grpc.pb.cc")
set(Chubby_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/Chubby.grpc.pb.h")

add_custom_command(
      OUTPUT "${Chubby_proto_srcs}" "${Chubby_proto_hdrs}" "${Chubby_grpc_srcs}" "${Chubby_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${Chubby_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${Chubby_proto}"
      DEPENDS "${Chubby_proto}")

include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(Chubby_proto_grpc
  ${Chubby_proto_srcs}
  ${Chubby_proto_hdrs}
  ${Chubby_grpc_srcs}
  ${Chubby_grpc_hdrs})

target_link_libraries(Chubby_proto_grpc
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})
