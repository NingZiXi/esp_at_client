# esp_at_client 库 CMake 子模块；由根 CMakeLists.txt add_subdirectory 引入

cmake_minimum_required(VERSION 3.22)

set(_esp_at_client_dir ${CMAKE_CURRENT_SOURCE_DIR})

add_library(esp_at_client STATIC
    ${_esp_at_client_dir}/src/esp_at_client.c
    ${_esp_at_client_dir}/src/esp_at_internal.c
    ${_esp_at_client_dir}/src/esp_at_link.c
    ${_esp_at_client_dir}/src/esp_at_port_stm32.c
    ${_esp_at_client_dir}/src/esp_at_esp_port.c
    ${_esp_at_client_dir}/src/esp_at_wifi.c
    ${_esp_at_client_dir}/src/esp_at_mqtt.c
    ${_esp_at_client_dir}/src/esp_at_http.c
    ${_esp_at_client_dir}/src/ringbuffer.c
)

target_include_directories(esp_at_client PUBLIC
    ${_esp_at_client_dir}/inc
)

# 项目根定位：用于 main/esp_at_config_user.h 链入
get_filename_component(_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# main/ 进 PUBLIC include，让库内 __has_include 能找到用户配置
target_include_directories(esp_at_client PUBLIC
    ${_PROJECT_ROOT}/main
)

target_compile_definitions(esp_at_client PUBLIC
    ESP_AT_ENABLE=1
)

# main/esp_at_config_user.h 存在 → -include 注入到所有编译单元前；缺失则沿用默认
set(_ESP_AT_USER_CONFIG "${_PROJECT_ROOT}/main/esp_at_config_user.h")
if(EXISTS "${_ESP_AT_USER_CONFIG}")
    message(STATUS "esp_at_client: found user config ${_ESP_AT_USER_CONFIG}")
    target_compile_options(esp_at_client PRIVATE
        "SHELL:-include ${_ESP_AT_USER_CONFIG}"
    )
else()
    message(STATUS "esp_at_client: no user config, using esp_at_client_config_default.h")
endif()

# 默认日志级别 INFO；用户可通过 ESP_AT_LOG_LEVEL_OVERRIDE 覆盖
if(NOT DEFINED ESP_AT_LOG_LEVEL_OVERRIDE)
    target_compile_definitions(esp_at_client PRIVATE ESP_AT_LOG_LEVEL=3)
endif()

# C11：支持 ##__VA_ARGS__
target_compile_features(esp_at_client PUBLIC c_std_11)

# 链接 CubeMX 生成的 stm32cubemx target（含 FreeRTOS/HAL/CMSIS 头与宏）
if(TARGET stm32cubemx)
    target_link_libraries(esp_at_client PUBLIC stm32cubemx)
endif()

# 链接 stm_log（如果工程启用了）
if(TARGET stm_log)
    target_link_libraries(esp_at_client PUBLIC stm_log)
endif()