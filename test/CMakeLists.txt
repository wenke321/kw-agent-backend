
#add_executable(chat_client_test chat_client_test.cpp)
add_executable(tools_test tools_test.cpp)
add_executable(file_delete_line_test file_delete_line_test.cpp)
add_executable(file_insert_line_test file_insert_line_test.cpp)
add_executable(file_read_line_test file_read_line_test.cpp)
add_executable(ripgrep_test ripgrep_test.cpp)
add_executable(exec_cmd_test exec_cmd_test.cpp)

#target_link_libraries(chat_client_test PRIVATE kw-agent_backend_impl)
target_link_libraries(tools_test PRIVATE kw_agent_backend)
target_link_libraries(file_delete_line_test PRIVATE kw_agent_backend)
target_link_libraries(file_insert_line_test PRIVATE kw_agent_backend)
target_link_libraries(file_read_line_test PRIVATE kw_agent_backend)
target_link_libraries(ripgrep_test PRIVATE kw_agent_backend)
target_link_libraries(exec_cmd_test PRIVATE kw_agent_backend)


#if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#    target_compile_options(chat_client_test PRIVATE -Wall -Wextra)
#endif()

# 安装我们自己的库和可执行文件
#install(TARGETS kw-agent_backend chat_client_test tools_test
#    LIBRARY DESTINATION lib
#    RUNTIME DESTINATION bin
#)

# 安装第三方动态库到 lib 目录
#install(FILES
#    "${CMAKE_SOURCE_DIR}/third-party/curl/lib/libcurl-x64.dll"
#    "${CMAKE_SOURCE_DIR}/third-party/llhttp/lib/llhttp.dll"
#    DESTINATION lib
#)

# 让可执行文件和动态库在安装后，能从自己所在目录找到依赖
#set_target_properties(chat_client_test kw-agent_backend_impl PROPERTIES
#    INSTALL_RPATH "$ORIGIN/../lib"   # bin 目录往上一级的 lib
#)
#
#set_target_properties(tools_test kw-agent_backend_impl PROPERTIES
#    INSTALL_RPATH "$ORIGIN/../lib"   # bin 目录往上一级的 lib
#)
