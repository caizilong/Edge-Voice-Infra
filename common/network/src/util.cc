#include "util.h"

#include <arpa/inet.h>
#include <string.h>
#include <chrono>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace network { 
    static int g_pid = 0;

    static thread_local int t_thread_id = 0;

    pid_t getPid() {
        if (g_pid == 0) {
            g_pid = getpid();
        }
        return g_pid;
    }

    pid_t getThreadId() {
        if (t_thread_id == 0) {
            t_thread_id = static_cast<pid_t>(::syscall(SYS_gettid));
        }
        return t_thread_id;
    }

    int64_t getNowMs() {
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now());
        return now.time_since_epoch().count();
    }

    int32_t getInt32FromNetByte(const char *buf) {
        int32_t re;
        memcpy(&re, buf, sizeof(re));
        return ntohl(re);
    }
} // namespace network