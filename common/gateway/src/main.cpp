#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <iostream>
#include <string>

#include "all.h"

#include "zmq_bus.h"
#include "remote_action.h"
#include "remote_server.h"
#include "unit_data.h"

pthread_spinlock_t key_sql_lock;
std::unordered_map<std::string, std::any> key_sql;
std::string zmq_s_format;
std::string zmq_c_format;
volatile sig_atomic_t main_exit_flage = 0;
int gateway_signal_fd = -1;

void get_run_config()
{
    load_default_config();
}

void tcp_work();

void tcp_stop_work();

namespace {

bool read_required_string_config(const std::string& key, std::string& out)
{
    try {
        out = std::any_cast<std::string>(key_sql.at(key));
        return !out.empty();
    } catch (...) {
        ALOGE("missing or invalid config:%s", key.c_str());
        return false;
    }
}

bool read_required_int_config(const std::string& key)
{
    try {
        (void)std::any_cast<int>(key_sql.at(key));
        return true;
    } catch (...) {
        ALOGE("missing or invalid config:%s", key.c_str());
        return false;
    }
}

}  // namespace

void all_work()
{
    if (!read_required_string_config("config_zmq_s_format", zmq_s_format) ||
        !read_required_string_config("config_zmq_c_format", zmq_c_format) ||
        !read_required_int_config("config_tcp_server") ||
        !read_required_int_config("config_zmq_min_port") ||
        !read_required_int_config("config_zmq_max_port")) {
        main_exit_flage = 1;
        return;
    }
    remote_server_work();
    if (main_exit_flage != 0) {
        return;
    }
    tcp_work();
}

void all_stop_work()
{
    tcp_stop_work();
    remote_server_stop_work();
}

static void __sigint(int iSigNo)
{
    (void)iSigNo;
    main_exit_flage = 1;
    if (gateway_signal_fd >= 0) {
        uint64_t one = 1;
        (void)write(gateway_signal_fd, &one, sizeof(one));
    }
}

void all_work_check()
{
}

int main(int argc, char *argv[])
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = __sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    mkdir("/tmp/ai_gateway", 0777);
    mkdir("/tmp/llm", 0777);
    gateway_signal_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (gateway_signal_fd < 0)
    {
        ALOGE("gateway signal fd init false");
        exit(1);
    }
    if (pthread_spin_init(&key_sql_lock, PTHREAD_PROCESS_PRIVATE) != 0)
    {
        ALOGE("key_sql_lock init false");
        exit(1);
    }
    ALOGD("ai_gateway_sys start");
    get_run_config();
    ALOGD("ai_gateway_sys work");
    all_work();
    while (main_exit_flage == 0)
    {
        sleep(1);
    }
    ALOGD("ai_gateway_sys stop");
    all_stop_work();
    close(gateway_signal_fd);
    gateway_signal_fd = -1;
    pthread_spin_destroy(&key_sql_lock);

    return 0;
}
