set(LIB_NAME zlib)
# 指定动态库的输出名称
set_target_properties(${LIB_NAME} PROPERTIES OUTPUT_NAME ${LIB_NAME})
# 使动态库和静态库同时存在
set_target_properties(${LIB_NAME}static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(${LIB_NAME} PROPERTIES CLEAN_DIRECT_OUTPUT 1)

target_include_directories(${LIB_NAME} PUBLIC
    # 包含生成库文件所需的路径
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    #为自定义module生成保护路径,使用module时不用再include了
    $<INSTALL_INTERFACE:include>)

# 定义有哪些头文件
set_target_properties(${LIB_NAME} PROPERTIES PUBLIC_HEADER "${ZLIB_PUBLIC_HDRS}")

# 安装库文件
install(
    # 安装目标
    TARGETS ${LIB_NAME} ${LIB_NAME}static
    # 导出安装库文件信息(为了生成module)
    EXPORT ${LIB_NAME}-targets
    # 头文件
    PUBLIC_HEADER DESTINATION include
    # 静态库
    ARCHIVE DESTINATION lib
    # 动态库
    LIBRARY DESTINATION lib
    # 可执行程序
    RUNTIME DESTINATION bin)

# 生成xxx-config.cmake文件
install(
    # 库文件信息
    EXPORT ${LIB_NAME}-targets
    # 加上名字空间(定义了则使用时也需加上)
    NAMESPACE ${LIB_NAME}::
    # 生成<lib_name>-config.cmake文件
    FILE ${LIB_NAME}-config.cmake
    DESTINATION lib/cmake/${LIB_NAME})