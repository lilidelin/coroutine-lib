# ============================================================
# benchmark/CMakeLists.txt  —  benchmark 子目录构建脚本
#
# 用法：（建议从 6hook 根目录的 CMakeLists.txt add_subdirectory）
#   mkdir build && cd build
#   cmake .. && make -j
#   生成的可执行文件：
#     bench1_sleep        ——  Sleep 吞吐量对比
#     bench2_echo_servers ——  Echo Server（A:8081 阻塞 / B:8082 协程）
#     bench3_latency      ——  延迟分布对比（需要 bench2 先起）
# ============================================================

# 所有 benchmark 都依赖 6hook 核心库（sylar_hook_lib，定义在父 CMakeLists.txt）

# ---- Bench1: Sleep 吞吐量 ----
add_executable(bench1_sleep bench1_throughput_sleep.cpp)
target_link_libraries(bench1_sleep PRIVATE sylar_hook_lib pthread dl)

# ---- Bench2: Echo Server（A/B 两个组同进程跑，不同端口）----
add_executable(bench2_echo_servers bench2_echo_servers.cpp)
target_link_libraries(bench2_echo_servers PRIVATE sylar_hook_lib pthread dl)

# ---- Bench3: 延迟分布（自己做客户端）----
add_executable(bench3_latency bench3_latency.cpp)
target_link_libraries(bench3_latency PRIVATE pthread)

# ---- 把 echo.lua 拷贝到 build/benchmark 目录方便跑 wrk ----
configure_file(echo.lua ${CMAKE_CURRENT_BINARY_DIR}/echo.lua COPYONLY)

message(STATUS "Benchmarks configured: bench1_sleep, bench2_echo_servers, bench3_latency")
