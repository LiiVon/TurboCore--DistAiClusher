#pragma once

// -------------- 标准库头文件，所有模块都需要，统一放在global.h里，一次include到处用 --------------
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <ctime>
#include <functional>
#include <yaml-cpp/yaml.h>
#include <unordered_map>
#include <any>
#include <future>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstddef>
#include <csignal>
#include <climits>
#include <cstdint>
#include <map>

// -------------- 跨平台统一宏定义，所有模块共享，避免每个文件重复定义 --------------
#ifdef _WIN32

// Windows平台专属定义
#   include <winsock2.h>

#   include <ws2tcpip.h>

#   undef ERROR

typedef ptrdiff_t ssize_t;
#   define INVALID_SOCKET_VALUE INVALID_SOCKET

#else

// Linux/Unix平台专属定义
#   include <sys/socket.h>

#   include <netinet/in.h>

#   include <unistd.h>

#   include <fcntl.h>

#   include <errno.h>

#   define INVALID_SOCKET_VALUE (-1)

#endif

// -------------- 兼容不支持SO_REUSEPORT的老Windows平台 --------------
#ifndef SO_REUSEPORT

#   define SO_REUSEPORT 15

#endif

// -------------- 统一SocketType别名，所有模块共享，不用每个文件重新定义 --------------
#ifdef _WIN32

using SocketType = SOCKET;
#else

using SocketType = int;
#endif

// 方便输出log
inline std::string thread_id_to_str(std::thread::id id) 
{
    std::stringstream ss;
    ss << id;
    return ss.str();
}
