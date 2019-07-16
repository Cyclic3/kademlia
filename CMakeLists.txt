cmake_minimum_required(VERSION 3.9)

project(kademlia VERSION 0.0.1)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Miniupnpc REQUIRED)
include_directories(${PROTO_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})

include_directories(${PROJECT_SOURCE_DIR}/include)

file(GLOB_RECURSE source src/*.cpp)

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x600)
  find_package(MSGSL REQUIRED)
  include_directories(SYSTEM ${MSGSL_INCLUDE_DIRS})
  list(APPEND extra_libs ws2_32)
  file(GLOB_RECURSE platform_source_tmp src/*.WINDOWS.cxx)
  set(platform_source ${platform_source} ${platform_source_tmp})
endif()

# Handle all unicies (Linux, OSX, etc)
if(UNIX)
  file(GLOB_RECURSE platform_source_tmp src/*.UNIX.cxx)
  set(platform_source ${platform_source} ${platform_source_tmp})
endif()

if(APPLE)
  file(GLOB_RECURSE platform_source_tmp src/*.APPLE.cxx)
  set(platform_source ${platform_source} ${platform_source_tmp})
endif()

if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
  file(GLOB_RECURSE platform_source_tmp src/*.LINUX.cxx)
  set(platform_source ${platform_source} ${platform_source_tmp})
  list(APPEND extra_libs atomic)
endif()

file(GLOB_RECURSE proto_source proto/*)

grpc_generate_cpp(rpc_gen_source rpc_gen_headers ${proto_source})

protobuf_generate_cpp(proto_gen_source proto_gen_headers ${proto_source})

list (GET proto_gen_headers 0 _proto_gen_headers_head)
get_filename_component(proto_gen_include ${_proto_gen_headers_head} DIRECTORY)
list (GET proto_gen_headers 0 _rpc_gen_headers_head)
get_filename_component(rpc_gen_include ${_rpc_gen_headers_head} DIRECTORY)
include_directories(SYSTEM ${proto_gen_include} SYSTEM ${rpc_gen_include})

# We don't want examples
file(GLOB imgui_src imgui/*.cpp)
include_directories(imgui imgui/examples)

# TODO: imgui backed detect logic
find_package(SDL2 REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})
include_directories(${OPENGL_INCLUDE_DIR})
list(APPEND imgui_backend imgui/examples/imgui_impl_sdl.cpp imgui/examples/imgui_impl_opengl2.cpp)

add_library(imgui STATIC ${imgui_src} ${imgui_backend})
target_link_libraries(imgui ${SDL2_LIBRARIES} OpenGL::GL SDL2::SDL2)

add_executable(${PROJECT_NAME} ${source} ${platform_source} ${proto_gen_source} ${rpc_gen_source})

target_link_libraries(${PROJECT_NAME} ${PROTOBUF_LIBRARIES} ${OPENSSL_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${GRPC_LIBRARIES} imgui ${MINIUPNPC_LIBRARIES} ${extra_libs})

file(GLOB_RECURSE tests tests/*.cxx)

foreach(test ${tests})
  file(RELATIVE_PATH test_rel ${CMAKE_SOURCE_DIR}/tests/ ${test})
  get_filename_component(test_fname ${test_rel} NAME_WE)
  get_filename_component(test_dir ${test_rel} DIRECTORY)
  #string(REGEX REPLACE "/" "_" test_dir ${test_dir})

  set(test_name ${test_dir}_${test_fname})

  add_executable(${test_name} ${test})

  target_link_libraries(${test_name} ${PROJECT_NAME})

  add_test(${test_name} ${test_name})
endforeach()

enable_testing()

