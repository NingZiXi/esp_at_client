# esp_at_client 自研 ESP-AT 客户端库（独立维护的 CMake 子模块）
#
# 由根 CMakeLists.txt 通过 add_subdirectory(Lib/esp_at_client) 引入；
# 业务工程只需 target_link_libraries(... esp_at_client) 即可。
#
# 公开：inc/ 头文件路径、ESP_AT_ENABLE 宏。
# 依赖：FreeRTOS / STM32 HAL / 业务工程传入的 stm_log（可选）。

cmake_minimum_required(VERSION 3.22)

set(_esp_at_client_dir ${CMAKE_CURRENT_SOURCE_DIR})

add_library(esp_at_client STATIC
    # 公开 API
    ${_esp_at_client_dir}/src/esp_at_client.c
    # 内部 client 状态机
    ${_esp_at_client_dir}/src/esp_at_internal.c
    # 服务层
    ${_esp_at_client_dir}/src/esp_at_link.c
    ${_esp_at_client_dir}/src/esp_at_port_stm32.c
    ${_esp_at_client_dir}/src/esp_at_esp_port.c
    ${_esp_at_client_dir}/src/esp_at_wifi.c
    ${_esp_at_client_dir}/src/esp_at_mqtt.c
    ${_esp_at_client_dir}/src/esp_at_http.c
    # 通用环形缓冲
    ${_esp_at_client_dir}/src/ringbuffer.c
)

target_include_directories(esp_at_client PUBLIC
    ${_esp_at_client_dir}/inc
)

# 项目根定位（用户配置文件 esp_at_config_user.h 假设放在 main/ 下；
# CMakeLists.txt 用相对路径拼接即可，与 main/ 的 add_subdirectory
# 顺序无关）。
get_filename_component(_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# main/ 目录里如果存在 esp_at_config_user.h，会被自动 chain。
# 这里只把 main/ 加进 PUBLIC include 路径，让库内部 __has_include 能找到。
# 注意：用户**不应该**在自己的项目里直接包含 Lib/esp_at_client/inc；
# 唯一能被 include 的"上游头"路径就是这个 PUBLIC include。
target_include_directories(esp_at_client PUBLIC
    ${_PROJECT_ROOT}/main
)

target_compile_definitions(esp_at_client PUBLIC
    ESP_AT_ENABLE=1
)

# 用户级配置覆盖：如果 main/esp_at_config_user.h 存在，
# 用 -include 强制注入到所有 esp_at_client 编译单元的最前面。
# 缺失时完全静默（不报错），库沿用默认配置。
set(_ESP_AT_USER_CONFIG "${_PROJECT_ROOT}/main/esp_at_config_user.h")
if(EXISTS "${_ESP_AT_USER_CONFIG}")
    message(STATUS "esp_at_client: found user config ${_ESP_AT_USER_CONFIG}")
    target_compile_options(esp_at_client PRIVATE
        "SHELL:-include ${_ESP_AT_USER_CONFIG}"
    )
else()
    message(STATUS "esp_at_client: no user config, using esp_at_client_config_default.h")
endif()

# 默认日志级别（用户可通过 target_compile_definitions 覆盖 esp_at_client PRIVATE）
if(NOT DEFINED ESP_AT_LOG_LEVEL_OVERRIDE)
    target_compile_definitions(esp_at_client PRIVATE ESP_AT_LOG_LEVEL=3)
endif()

# 需要 C11（gnu11）支持 ##__VA_ARGS__
target_compile_features(esp_at_client PUBLIC c_std_11)

# 依赖 CubeMX 生成的 stm32cubemx INTERFACE target —— 它已经把 FreeRTOS、
# STM32 HAL、CMSIS include 路径与编译宏全部打包好，PUBLIC 链接会让
# esp_at_client 与消费者都能看到这些头文件。
if(TARGET stm32cubemx)
    target_link_libraries(esp_at_client PUBLIC stm32cubemx)
endif()

# 依赖 stm_log 库（如果工程启用了），esp_at_client 内部 ESP_AT_LOG* 宏通过它输出。
# 如果工程没启用 stm_log，宏自然展开为空操作，但头文件路径不可见。
if(TARGET stm_log)
    target_link_libraries(esp_at_client PUBLIC stm_log)
endif()