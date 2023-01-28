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
        src/dict.c
        src/geohash_helper.c
        src/sds.c
        src/crcspeed.c
        src/hyperloglog.c
        src/adlist.c
        src/eval.c
        src/lzf_d.c
        src/geo.c
        src/lolwut.c
        src/lazyfree.c
        src/rand.c
        src/object.c
        src/functions.c
        src/lolwut6.c
        src/util.c
        src/memtest.c
        src/replication.c
        src/redis-check-rdb.c
        src/sparkline.c
        src/t_stream.c
        src/intset.c
        src/aof.c
        src/sentinel.c
        src/tracking.c
        src/timeout.c
        src/networking.c
        src/pqsort.c
        src/sort.c
        src/anet.c
        src/module.c
        src/setproctitle.c
        src/lzf_c.c
        src/sha1.c
        src/t_set.c
        src/childinfo.c
        src/lolwut5.c
        src/call_reply.c
        src/redis-check-aof.c
        src/localtime.c
        src/rdb.c
        src/slowlog.c
        src/listpack.c
        src/multi.c
        src/debug.c
        src/cli_common.c
        src/t_list.c
        src/rio.c
        src/cluster.c
        src/connection.c
        src/function_lua.c
        src/bio.c
        src/ae.c
        src/blocked.c
        src/t_string.c
        src/config.c
        src/endianconv.c
        src/crc16.c
        src/setcpuaffinity.c
        src/script.c
        src/bitops.c
        src/ziplist.c
        src/defrag.c
        src/acl.c
        src/tls.c
        src/geohash.c
        src/sha256.c
        src/siphash.c
        src/crc64.c
        src/resp_parser.c
        src/mt19937-64.c
        src/zmalloc.c
        src/zipmap.c
        src/syncio.c
        src/db.c
        src/t_zset.c
        src/notify.c
        src/t_hash.c
        src/evict.c
        src/script_lua.c
        src/latency.c
        src/rax.c
        src/release.c
        src/commands.c
        src/expire.c
        src/pubsub.c
        src/monotonic.c
        src/quicklist.c
        )
set(SRC_SERVER src/server.c ${SRC_SERVER_TMP} ${OPTION})
set(SRC_CLI
        src/anet.c
        src/sds.c
        src/adlist.c
        src/redis-cli.c
        src/zmalloc.c
        src/release.c
        src/ae.c
        src/crc64.c
        src/crc16.c
        src/dict.c
        src/siphash.c
        )
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    # better not to work with jemalloc
endif ()
set(EXECUTABLE_OUTPUT_PATH src)
add_executable(redis-server ${SRC_SERVER})
add_executable(redis-cli ${SRC_CLI})
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
