cmake_minimum_required(VERSION 3.20)
project(unit_manager VERSION 1.6)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)  
set(REQUIREMENTS  
    pthread 
    util 
    glog
    )

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ../zmq-comm/include
    ../stackflow/include
    ../network/include
    ${REPO_ROOT}/logging
    ${REPO_ROOT}/third-party
    "/usr/local/include/"
    "include" "."
)

file(GLOB_RECURSE SRCS "src/main.cpp" 
"src/remote_server.cpp" 
"src/config.cpp" 
"src/tcp_comm.cpp"
"src/remote_action.cpp" 
"src/unit_data.cpp" 
"src/zmq_bus.cpp"  
"${CMAKE_CURRENT_SOURCE_DIR}/../network/src/*.cc")



find_package(simdjson QUIET)
# link_directories(/usr/lib/x86_64-linux-gnu)

add_executable(${PROJECT_NAME}  ${SRCS})

target_link_libraries(${PROJECT_NAME} PRIVATE 
    pthread 
    util 
    glog
    pgm 
    sodium 
    norm
    bsd
    gssapi_krb5
    zmq)

if(simdjson_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE simdjson::simdjson)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE /usr/local/lib/libsimdjson.a)
endif()

if(TARGET stackflow)
    target_link_libraries(${PROJECT_NAME} PRIVATE stackflow)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../install/lib/libstackflow.a)
endif()



install(TARGETS ${PROJECT_NAME}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)

install(FILES "sys_config.json"
    DESTINATION share/${PROJECT_NAME})
