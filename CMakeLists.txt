cmake_minimum_required(VERSION 3.1)

get_filename_component(proto_file "./messages.proto" ABSOLUTE)
get_filename_component(proto_file_path "${proto_file}" PATH)

find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

find_program(gRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

set(example_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/messages.pb.cc")
set(example_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/messages.pb.h")
set(example_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/messages.grpc.pb.cc")
set(example_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/messages.grpc.pb.h")

add_custom_command(
  OUTPUT "${example_proto_srcs}" "${example_proto_hdrs}" "${example_grpc_srcs}"
         "${example_grpc_hdrs}"
  COMMAND
    ${PROTOBUF_PROTOC_EXECUTABLE} ARGS "--grpc_out=${CMAKE_CURRENT_BINARY_DIR}"
    "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}" "--proto_path=${proto_file_path}"
    "--plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}" "${proto_file}")

add_library(example_grpc_proto ${example_grpc_srcs} ${example_grpc_hdrs}
                               ${example_proto_srcs} ${example_proto_hdrs})

include_directories(${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(example_grpc_proto gRPC::grpc++ protobuf::libprotobuf)

foreach(_target client server)
  add_executable(${_target} "${_target}.cc")
  target_link_libraries(${_target} example_grpc_proto ${OPENTELEMETRY_CPP_LIBRARIES})
endforeach()

