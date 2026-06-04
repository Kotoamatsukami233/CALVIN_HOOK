#pragma once

#include "quickjs.h"

void js_socket_server_init(JSContext *ctx, JSValue ns);
void js_socket_server_start();
void js_socket_server_stop();
