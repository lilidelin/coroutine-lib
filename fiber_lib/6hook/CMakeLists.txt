# ============================================================
# 6hook/CMakeLists.txt  —  6hook 模块主构建脚本
#
# 生成：
#   1. sylar_hook_lib    (STATIC 库)：hook + 所有底层组件（iomanager/scheduler/
#                                       fiber/timer/fd_manager/thread）
#   2. main_hook_demo    (可执行)    ：main.cpp —— HTTP demo server
#   3. 见 benchmark/ 下的 3 个对比测试
#
# 用法：
#   cd fiber_lib/6hook
#   mkdir -p build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
# ============================================================

cmake_minimum_required(VERSION 3.10)
project(coroutine_lib_6hook CXX)

# ---- C++ 标准 & 编译选项 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# 基础警告 + 优化（Release 默认 -O3）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---- 源文件：6hook 核心组件 ----
set(SYLAR_CORE_SOURCES
    hook.cpp
    fiber.cpp
    scheduler.cpp
    ioscheduler.cpp
    timer.cpp
    fd_manager.cpp
    thread.cpp
)

set(SYLAR_CORE_HEADERS
    hook.h
    fiber.h
    scheduler.h
    ioscheduler.h
    timer.h
    fd_manager.h
    thread.h
)

# ---- 静态库：所有组件打包成一个库，benchmark 和 main 都链接它 ----
add_library(sylar_hook_lib STATIC ${SYLAR_CORE_SOURCES})
target_include_directories(sylar_hook_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Linux 特有依赖
find_package(Threads REQUIRED)
target_link_libraries(sylar_hook_lib PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---- 可执行：main.cpp demo（HTTP Hello World 服务器）----
add_executable(main_hook_demo main.cpp)
target_link_libraries(main_hook_demo PRIVATE sylar_hook_lib)

# 最小回归用例：同一截止时间的多个 timer 都应保留并触发。
add_executable(timer_same_deadline_test tests/timer_same_deadline_test.cpp)
target_link_libraries(timer_same_deadline_test PRIVATE sylar_hook_lib)
target_compile_definitions(timer_same_deadline_test PRIVATE SYLAR_TIMER_TESTING)

# ---- benchmark 子目录 ----
option(BUILD_6HOOK_BENCHMARKS "Build benchmarks for 6hook module" ON)
if(BUILD_6HOOK_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# ---- 汇总信息 ----
message(STATUS "========================================")
message(STATUS " 6hook module configured")
message(STATUS "   Targets:")
message(STATUS "     sylar_hook_lib        (static lib)")
message(STATUS "     main_hook_demo        (HTTP demo)")
if(BUILD_6HOOK_BENCHMARKS)
message(STATUS "     bench1_sleep          (throughput sleep)")
message(STATUS "     bench2_echo_servers   (echo servers)")
message(STATUS "     bench3_latency        (latency distribution)")
endif()
message(STATUS "========================================")
