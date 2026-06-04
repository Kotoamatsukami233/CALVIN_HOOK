#include "js_socket_server.h"
#include "js_engine.h"
#include "js_scope.h"

#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>

#define LOG_TAG "JSSocket"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::atomic<bool> g_running{false};
static std::atomic<int> g_listen_port{0};
static std::thread g_server_thread;
static std::vector<int> g_clients;
static std::mutex g_clients_mutex;

// Forward declare the JS recv callback
static JSValue g_recv_callback = JS_UNDEFINED;
static JSContext *g_socket_ctx = nullptr;

// Send JSON to all connected clients
static void broadcast_to_clients(const char *json) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        std::string msg = std::string(json) + "\n";
        ssize_t sent = send(*it, msg.c_str(), msg.size(), MSG_NOSIGNAL);
        if (sent <= 0) {
            close(*it);
            it = g_clients.erase(it);
        } else {
            ++it;
        }
    }
}

// Handle a single client connection
static void handle_client(int fd) {
    char buf[4096];
    std::string line;

    while (g_running) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        line += buf;

        // Process complete lines (JSON messages)
        size_t pos;
        while ((pos = line.find('\n')) != std::string::npos) {
            std::string msg = line.substr(0, pos);
            line.erase(0, pos + 1);

            if (msg.empty()) continue;

            // Parse as JSON and execute
            JSContext *ctx = JSEngine::GetContext();
            if (!ctx) continue;

            JSScope scope;

            // Check if message starts with {"type":"script"
            // Simple protocol: each line is a JSON object with:
            //   {"type":"script","action":"load","script":"<base64>","name":"..."}
            // For simplicity, just evaluate the message as JS source

            // Try to extract "script" field from JSON
            std::string script;
            const char *script_key = strstr(msg.c_str(), "\"script\"");
            if (script_key) {
                const char *colon = strchr(script_key, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ' || *colon == '"') colon++;
                    const char *end = strchr(colon, '"');
                    if (end) {
                        // This is a simple extraction — real impl would use base64 decode
                        script.assign(colon, end - colon);
                    }
                }
            }

            if (script.empty()) {
                // Treat the whole message as JS source
                script = msg;
            }

            std::string result = JSEngine::LoadScript(script, "<socket>");

            // Send result back
            std::string response = "{\"type\":\"result\",\"value\":" + result + "}\n";
            send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
        }
    }
}

// Server accept loop
static void server_loop(int server_fd) {
    LOGI("Socket server listening on port %d", g_listen_port.load());

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (g_running) LOGE("accept failed: %s", strerror(errno));
            break;
        }

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients.push_back(client_fd);
        }

        LOGI("Client connected: %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Handle client in a separate thread
        std::thread(handle_client, client_fd).detach();
    }

    close(server_fd);
}

// ============================================================
// JS API functions
// ============================================================

// send() override that also broadcasts to socket clients
static JSValue js_socket_send(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) return JS_EXCEPTION;

    broadcast_to_clients(msg);

    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_socket_start(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    int port = 0;  // auto-assign
    if (argc >= 1) {
        int32_t p;
        if (JS_ToInt32(ctx, &p, argv[0]) == 0) port = p;
    }

    if (g_running) {
        return JS_ThrowTypeError(ctx, "Server already running on port %d", g_listen_port.load());
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return JS_ThrowTypeError(ctx, "socket() failed");

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(server_fd);
        return JS_ThrowTypeError(ctx, "bind() failed");
    }

    if (listen(server_fd, 5) < 0) {
        close(server_fd);
        return JS_ThrowTypeError(ctx, "listen() failed");
    }

    // Get actual port if auto-assigned
    socklen_t addr_len = sizeof(addr);
    getsockname(server_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
    g_listen_port.store(ntohs(addr.sin_port));

    g_running = true;
    g_socket_ctx = ctx;
    g_server_thread = std::thread(server_loop, server_fd);

    LOGI("Socket server started on port %d", g_listen_port.load());

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "port", JS_NewInt32(ctx, g_listen_port.load()));
    return result;
}

static JSValue js_socket_stop(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    g_running = false;

    // Close all clients
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (int fd : g_clients) close(fd);
        g_clients.clear();
    }

    if (g_server_thread.joinable()) g_server_thread.join();
    g_listen_port.store(0);
    return JS_UNDEFINED;
}

static JSValue js_socket_get_port(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    return JS_NewInt32(ctx, g_listen_port.load());
}

static const JSCFunctionListEntry socket_funcs[] = {
    JS_CFUNC_DEF("start", 1, js_socket_start),
    JS_CFUNC_DEF("stop", 0, js_socket_stop),
    JS_CFUNC_DEF("send", 1, js_socket_send),
    JS_CFUNC_DEF("getPort", 0, js_socket_get_port),
};

void js_socket_server_init(JSContext *ctx, JSValue ns) {
    JSValue sock = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sock, socket_funcs,
                               sizeof(socket_funcs) / sizeof(socket_funcs[0]));
    JS_DefinePropertyValueStr(ctx, ns, "SocketServer", sock, JS_PROP_C_W_E);
}

void js_socket_server_start() {
    // Can be called from C to auto-start on a random port
}

void js_socket_server_stop() {
    g_running = false;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (int fd : g_clients) close(fd);
        g_clients.clear();
    }
    if (g_server_thread.joinable()) g_server_thread.join();
}
