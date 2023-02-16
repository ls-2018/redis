cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
message("CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
project(redis VERSION 7.0)
#set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../src)
message(CMAKE_RUNTIME_OUTPUT_DIRECTORY is:${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
#if (NOT CMAKE_BUILD_TYPE)
message(STATUS "No build type defined; defaulting to 'Debug'")
set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
        "The type of build. Possible values are: Debug,Release,
RelWithDebInfo and MinSizeRel.")

#endif()
message(STATUS "Host is: ${CMAKE_HOST_SYSTEM}. Build target is:
${CMAKE_SYSTEM}")
get_filename_component(REDIS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
message(STATUS "Project root directory is: ${REDIS_ROOT}")
# Just for debugging when handling a new platform.
if (false)
    message("C++ compiler supports these language features:")
    foreach (i ${CMAKE_CXX_COMPILE_FEATURES})
        message(" ${i}")
    endforeach ()
endif ()
message(STATUS "Generating release.h...")
execute_process(
        COMMAND sh -c ./mkreleasehdr.sh
        WORKING_DIRECTORY ${REDIS_ROOT}/src/
)
add_subdirectory(deps)
add_subdirectory(src/modules)
set(SRC_SERVER_TMP
        src/over-dict.c
        src/over-geohash_helper.c
        src/over-sds.c
        src/over-crcspeed.c
        src/hyperloglog.c
        src/over-double_ended_linked_list.c
        src/eval.c
        src/over-lzf_d.c
        src/over-geo.c
        src/over-lolwut.c
        src/over-lazyfree.c
        src/over-rand.c
        src/over-object.c
        src/over-functions.c
        src/over-lolwut6.c
        src/over-util.c
        src/over-memtest.c
        src/replication.c
        src/redis-check-rdb.c
        src/sparkline.c
        src/t_stream.c
        src/intset.c
        src/over-aof.c
        src/sentinel.c
        src/tracking.c
        src/over-timeout.c
        src/over-networking.c
        src/over-pqsort.c
        src/over-sort.c
        src/over-anet.c
        src/module.c
        src/over-setproctitle.c
        src/over-lzf_c.c
        src/over-sha1.c
        src/t_set.c
        src/over-childinfo.c
        src/over-lolwut5.c
        src/over-call_reply.c
        src/redis-check-aof.c
        src/over-localtime.c
        src/over-rdb.c
        src/slowlog.c
        src/listpack.c
        src/multi.c
        src/over-debug.c
        src/over-cli_common.c
        src/t_list.c
        src/rio.c
        src/cluster.c
        src/over-connection.c
        src/over-function_lua.c
        src/over-bio.c
        src/over-ae.c
        src/over-blocked.c
        src/t_string.c
        src/over-config.c
        src/over-endianconv.c
        src/over-crc16.c
        src/over-setcpuaffinity.c
        src/script.c
        src/over-bitops.c
        src/ziplist.c
        src/over-defrag.c
        src/acl.c
        src/tls.c
        src/over-geohash.c
        src/over-sha256.c
        src/over-siphash.c
        src/over-crc64.c
        src/over-resp_parser.c
        src/over-mt19937-64.c
        src/over-zmalloc.c
        src/zipmap.c
        src/over-syncio.c
        src/db.c
        src/t_zset.c
        src/notify.c
        src/t_hash.c
        src/over-evict.c
        src/script_lua.c
        src/over-latency.c
        src/rax.c
        src/over-release.c
        src/commands.c
        src/over-expire.c
        src/pubsub.c
        src/over-monotonic.c
        src/quicklist.c
        src/over-server.h)
set(SRC_SERVER src/over-server.c ${SRC_SERVER_TMP} ${OPTION} src/over-server.h)
set(SRC_CLI
        src/over-anet.c
        src/over-sds.c
        src/over-double_ended_linked_list.c
        src/redis-cli.c
        src/over-zmalloc.c
        src/over-release.c
        src/over-ae.c
        src/over-crc64.c
        src/over-crc16.c
        src/over-dict.c
        src/over-siphash.c
        src/over-server.h)
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    # better not to work with jemalloc
endif ()
set(EXECUTABLE_OUTPUT_PATH src)
add_executable(redis-server ${SRC_SERVER} src/over-server.h)
add_executable(redis-cli ${SRC_CLI} src/over-server.h)
set_property(TARGET redis-server PROPERTY C_STANDARD 99)
set_property(TARGET redis-server PROPERTY CXX_STANDARD 11)
set_property(TARGET redis-server PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET redis-cli PROPERTY C_STANDARD 99)
set_property(TARGET redis-cli PROPERTY CXX_STANDARD 11)
set_property(TARGET redis-cli PROPERTY CXX_STANDARD_REQUIRED ON)
target_include_directories(redis-server
        PRIVATE ${REDIS_ROOT}/deps/hiredis
        PRIVATE ${REDIS_ROOT}/deps/linenoise
        PRIVATE ${REDIS_ROOT}/deps/hdr_histogram
        PRIVATE ${REDIS_ROOT}/deps/lua/src
        )
target_include_directories(redis-cli
        PRIVATE ${REDIS_ROOT}/deps/hiredis
        PRIVATE ${REDIS_ROOT}/deps/linenoise
        PRIVATE ${REDIS_ROOT}/deps/hdr_histogram
        PRIVATE ${REDIS_ROOT}/deps/lua/src
        )
target_link_libraries(redis-server
        PRIVATE pthread
        PRIVATE m
        PRIVATE lua
        PRIVATE linenoise
        PRIVATE hiredis
        PRIVATE hdr_histogram
        )
target_link_libraries(redis-cli
        PRIVATE pthread
        PRIVATE m
        PRIVATE linenoise
        PRIVATE hiredis
        )
link_directories(deps/hiredis/ deps/linenoise/ diredeps/lua/src)
install(TARGETS redis-server
        RUNTIME DESTINATION bin
        )
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -c")
