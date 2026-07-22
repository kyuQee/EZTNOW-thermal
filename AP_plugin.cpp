#include "ingestor/i_ingestor.hpp"
#include "core/event.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Forces GCC/Clang to export these symbols even if default visibility is hidden
#define EXPORT __attribute__((visibility("default")))

class APIngestor final : public mawmaw::ingestor::IIngestor {
public:
    // Matches the name 'esp_hotspot' expected by your configuration
    std::string name() const override { return "esp_hotspot"; }
    std::string version() const override { return "0.1.0"; }

    void start(mawmaw::ingestor::EmitFn emit) override {
        running_ = true;
        
        disable_firewall();
        create_hotspot();
        kill_process_on_port(port_);

        // Spawn background listener loop
        worker_ = std::thread([this, emit = std::move(emit)]() mutable {
            run_server(std::move(emit));
        });
    }

    void stop() override {
        std::cout << "Shutting down AP Plugin..." << std::endl;
        running_ = false;
        
        if (server_socket_ != -1) {
            shutdown(server_socket_, SHUT_RDWR);
            close(server_socket_);
            server_socket_ = -1;
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        enable_firewall();
        destroy_hotspot();
        std::cout << "Cleanup complete." << std::endl;
    }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    int port_ = 9000;
    int server_socket_ = -1;
    std::atomic<uint64_t> sequence_{0};

    void disable_firewall() {
        std::system("sudo systemctl stop firewalld >/dev/null 2>&1");
        std::cout << "Firewall stopped (firewalld)." << std::endl;
    }

    void enable_firewall() {
        std::system("sudo systemctl start firewalld >/dev/null 2>&1");
        std::cout << "Firewall restarted (firewalld)." << std::endl;
    }

    void kill_process_on_port(int port) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), 
            "sudo fuser -k %d/tcp >/dev/null 2>&1 || "
            "sudo lsof -t -i:%d | xargs -r sudo kill -9 >/dev/null 2>&1", port, port);
        std::system(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    void create_hotspot() {
        std::cout << "Starting host AP..." << std::endl;
        std::system("nmcli device wifi hotspot ssid MAWMAW password helloworld >/dev/null 2>&1");
        std::cout << "Hotspot running." << std::endl;
    }

    void destroy_hotspot() {
        std::system("nmcli connection down Hotspot >/dev/null 2>&1");
    }

    void run_server(mawmaw::ingestor::EmitFn emit) {
        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ == -1) {
            std::cerr << "Socket creation failed!" << std::endl;
            return;
        }

        int opt = 1;
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Socket bind failed!" << std::endl;
            close(server_socket_);
            server_socket_ = -1;
            return;
        }

        if (listen(server_socket_, 10) < 0) {
            std::cerr << "Socket listen failed!" << std::endl;
            close(server_socket_);
            server_socket_ = -1;
            return;
        }

        std::cout << "Socket server listening on port " << port_ << std::endl;

        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_sock < 0) {
                break; // Socket closed safely by stop()
            }

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            std::string client_ip(ip_str);

            std::thread([this, client_sock, client_ip, emit]() mutable {
                handle_client(client_sock, client_ip, std::move(emit));
            }).detach();
        }
    }

    void handle_client(int sock, const std::string& ip, mawmaw::ingestor::EmitFn emit) {
        std::cout << "Client connected: " << ip << std::endl;
        char buf[4096];

        while (running_.load(std::memory_order_relaxed)) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;

            std::string msg(buf, n);
            if (!msg.empty() && msg.back() == '\n') msg.pop_back();
            std::cout << "ESP32 [" << ip << "]: " << msg << std::endl;

            // Pack standard event and pass it up the pipeline
            mawmaw::core::Event ev;
            ev.timestamp_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            ev.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);

            ev.set_stream("esp32_raw");
            ev.set_schema(ip.c_str());
            ev.set_payload(buf, n);

            emit(std::move(ev));
        }

        close(sock);
        std::cout << "Client disconnected: " << ip << std::endl;
    }
};

// ----------------------------------------------------------------------
// Linker Export Area (C ABI Match for mawmaw::ingestor::PluginHandle)
// ----------------------------------------------------------------------
extern "C" {

EXPORT mawmaw::ingestor::IIngestor* mawmaw_create() {
    return new APIngestor();
}

EXPORT void mawmaw_destroy(mawmaw::ingestor::IIngestor* p) {
    delete p;
}

EXPORT const char* mawmaw_plugin_version() {
    return "0.1.0";
}

}