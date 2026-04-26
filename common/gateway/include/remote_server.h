#pragma once

#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include "zmq_endpoint.h"

#include "all.h"
#include "unit_data.h"

using namespace StackFlows;

void remote_server_work();
void remote_server_stop_work();
