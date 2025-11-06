#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool send_all(int fd, const std::string &data) {
    size_t total_sent = 0;
    const char *ptr = data.c_str();
    size_t to_send = data.size();
    while (total_sent < to_send) {
        ssize_t sent = ::send(fd, ptr + total_sent, to_send - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

bool read_line(int fd, std::string &line_out, std::string &buffer) {
    for (;;) {
        auto pos = buffer.find('\n');
        if (pos != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            line_out = line;
            return true;
        }

        char temp[4096];
        ssize_t received = ::recv(fd, temp, sizeof(temp), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (received == 0) {
            return false;  // EOF
        }
        buffer.append(temp, temp + received);
    }
}

int connect_to_server(const std::string &host, const std::string &port) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    int ret = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (ret != 0) {
        std::cerr << "[Error] getaddrinfo: " << ::gai_strerror(ret) << std::endl;
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(res);
    return fd;
}

int create_listener(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::perror("setsockopt");
        ::close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }

    if (::listen(fd, 16) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }

    return fd;
}

void listener_thread(int listen_fd, std::atomic<bool> &running) {
    while (running.load()) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (!running.load()) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            break;
        }

        std::string buffer;
        std::string message;
        if (read_line(client_fd, message, buffer)) {
            std::cout << "[P2P] Received: " << message << std::endl;
        }
        ::close(client_fd);
    }
}

std::vector<std::string> split(const std::string &input) {
    std::istringstream iss(input);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <ServerIP> <ServerPort> <MyPort>" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    std::string server_port = argv[2];
    uint16_t my_port = static_cast<uint16_t>(std::stoi(argv[3]));

    int server_fd = connect_to_server(server_ip, server_port);
    if (server_fd < 0) {
        std::cerr << "[Error] Unable to connect to server." << std::endl;
        return 1;
    }

    int listen_fd = create_listener(my_port);
    if (listen_fd < 0) {
        ::close(server_fd);
        return 1;
    }

    std::atomic<bool> running{true};
    std::thread listener(listener_thread, listen_fd, std::ref(running));

    std::cout << "Commands: register <UserName>, login <UserName>, list, pay <Payee> <Amount>, exit" << std::endl;

    std::string server_buffer;
    std::map<std::string, std::pair<std::string, std::string>> online_list;
    std::mutex online_mutex;

    std::string current_user;
    bool logged_in = false;

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        auto tokens = split(line);
        if (tokens.empty()) {
            continue;
        }

        const std::string &cmd = tokens[0];

        if (cmd == "register") {
            if (tokens.size() != 2) {
                std::cout << "Usage: register <UserAccountName>" << std::endl;
                continue;
            }
            std::string username = tokens[1];
            std::string message = "REGISTER#" + username + "\r\n";
            if (!send_all(server_fd, message)) {
                std::cerr << "[Error] Failed to send register command." << std::endl;
                break;
            }
            std::string response;
            if (!read_line(server_fd, response, server_buffer)) {
                std::cerr << "[Error] Server disconnected." << std::endl;
                break;
            }
            std::cout << "[Server] " << response << std::endl;
        } else if (cmd == "login") {
            if (tokens.size() != 2) {
                std::cout << "Usage: login <UserAccountName>" << std::endl;
                continue;
            }
            std::string username = tokens[1];
            std::ostringstream oss;
            oss << username << "#" << my_port << "\r\n";
            if (!send_all(server_fd, oss.str())) {
                std::cerr << "[Error] Failed to send login command." << std::endl;
                break;
            }
            std::string balance_line;
            if (!read_line(server_fd, balance_line, server_buffer)) {
                std::cerr << "[Error] Server disconnected." << std::endl;
                break;
            }
            if (balance_line == "220 AUTH_FAIL") {
                std::cout << "[Server] " << balance_line << std::endl;
                logged_in = false;
                current_user.clear();
                {
                    std::lock_guard<std::mutex> lock(online_mutex);
                    online_list.clear();
                }
                continue;
            }

            std::string public_key_line;
            std::string count_line;
            if (!read_line(server_fd, public_key_line, server_buffer) ||
                !read_line(server_fd, count_line, server_buffer)) {
                std::cerr << "[Error] Server disconnected." << std::endl;
                break;
            }

            int count = 0;
            try {
                count = std::stoi(count_line);
            } catch (const std::exception &) {
                std::cerr << "[Error] Invalid list count from server." << std::endl;
                break;
            }

            std::map<std::string, std::pair<std::string, std::string>> temp_list;
            bool list_failed = false;
            for (int i = 0; i < count; ++i) {
                std::string entry_line;
                if (!read_line(server_fd, entry_line, server_buffer)) {
                    std::cerr << "[Error] Server disconnected." << std::endl;
                    list_failed = true;
                    break;
                }
                std::istringstream entry_stream(entry_line);
                std::string name, ip, port;
                if (!std::getline(entry_stream, name, '#') ||
                    !std::getline(entry_stream, ip, '#') ||
                    !std::getline(entry_stream, port, '#')) {
                    std::cerr << "[Error] Malformed online entry: " << entry_line << std::endl;
                    continue;
                }
                temp_list[name] = {ip, port};
            }
            if (list_failed) {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(online_mutex);
                online_list = std::move(temp_list);
            }

            logged_in = true;
            current_user = username;

            std::cout << "[Server] AccountBalance: " << balance_line << std::endl;
            std::cout << "[Server] PublicKey: " << public_key_line << std::endl;
            std::cout << "[Server] Online Count: " << count << std::endl;
            {
                std::lock_guard<std::mutex> lock(online_mutex);
                for (const auto &entry : online_list) {
                    std::cout << "[Server] " << entry.first << "#" << entry.second.first << "#" << entry.second.second << std::endl;
                }
            }
            std::cout << "[Info] Online list updated." << std::endl;
        } else if (cmd == "list") {
            if (!logged_in) {
                std::cout << "[Info] Please login first." << std::endl;
                continue;
            }
            if (!send_all(server_fd, std::string("List\r\n"))) {
                std::cerr << "[Error] Failed to send list command." << std::endl;
                break;
            }
            std::string balance_line, public_key_line, count_line;
            if (!read_line(server_fd, balance_line, server_buffer) ||
                !read_line(server_fd, public_key_line, server_buffer) ||
                !read_line(server_fd, count_line, server_buffer)) {
                std::cerr << "[Error] Server disconnected." << std::endl;
                break;
            }
            int count = 0;
            try {
                count = std::stoi(count_line);
            } catch (const std::exception &) {
                std::cerr << "[Error] Invalid list count from server." << std::endl;
                break;
            }

            std::map<std::string, std::pair<std::string, std::string>> temp_list;
            bool list_failed = false;
            for (int i = 0; i < count; ++i) {
                std::string entry_line;
                if (!read_line(server_fd, entry_line, server_buffer)) {
                    std::cerr << "[Error] Server disconnected." << std::endl;
                    list_failed = true;
                    break;
                }
                std::istringstream entry_stream(entry_line);
                std::string name, ip, port;
                if (!std::getline(entry_stream, name, '#') ||
                    !std::getline(entry_stream, ip, '#') ||
                    !std::getline(entry_stream, port, '#')) {
                    std::cerr << "[Error] Malformed online entry: " << entry_line << std::endl;
                    continue;
                }
                temp_list[name] = {ip, port};
            }
            if (list_failed) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(online_mutex);
                online_list = std::move(temp_list);
            }

            std::cout << "[Server] AccountBalance: " << balance_line << std::endl;
            std::cout << "[Server] PublicKey: " << public_key_line << std::endl;
            std::cout << "[Server] Online Count: " << count << std::endl;
            {
                std::lock_guard<std::mutex> lock(online_mutex);
                for (const auto &entry : online_list) {
                    std::cout << "[Server] " << entry.first << "#" << entry.second.first << "#" << entry.second.second << std::endl;
                }
            }
            std::cout << "[Info] Online list updated." << std::endl;
        } else if (cmd == "pay") {
            if (!logged_in) {
                std::cout << "[Info] Please login first." << std::endl;
                continue;
            }
            if (tokens.size() != 3) {
                std::cout << "Usage: pay <PayeeUserName> <Amount>" << std::endl;
                continue;
            }
            std::string payee = tokens[1];
            std::string amount_str = tokens[2];

            int amount = 0;
            try {
                amount = std::stoi(amount_str);
            } catch (const std::exception &) {
                std::cout << "[Info] Invalid amount." << std::endl;
                continue;
            }
            if (amount <= 0) {
                std::cout << "[Info] Amount must be positive." << std::endl;
                continue;
            }

            std::pair<std::string, std::string> target;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(online_mutex);
                auto it = online_list.find(payee);
                if (it != online_list.end()) {
                    target = it->second;
                    found = true;
                }
            }
            if (!found) {
                std::cout << "[Info] Payee not found in the online list." << std::endl;
                continue;
            }

            int pay_fd = connect_to_server(target.first, target.second);
            if (pay_fd < 0) {
                std::cout << "[Error] Unable to connect to payee." << std::endl;
                continue;
            }

            std::ostringstream msg;
            msg << current_user << "#" << amount << "#" << payee << "\r\n";
            if (!send_all(pay_fd, msg.str())) {
                std::cout << "[Error] Failed to send payment message." << std::endl;
                ::close(pay_fd);
                continue;
            }
            std::cout << "[P2P] Sent to " << payee << " (" << target.first << ":" << target.second
                      << "): " << current_user << "#" << amount << "#" << payee << std::endl;
            ::close(pay_fd);
        } else if (cmd == "exit") {
            if (!send_all(server_fd, std::string("Exit\r\n"))) {
                std::cerr << "[Error] Failed to send exit command." << std::endl;
            } else {
                std::string response;
                if (read_line(server_fd, response, server_buffer)) {
                    std::cout << "[Server] " << response << std::endl;
                }
            }
            break;
        } else {
            std::cout << "[Info] Unknown command." << std::endl;
        }
    }

    running.store(false);
    ::shutdown(listen_fd, SHUT_RDWR);
    ::close(listen_fd);
    if (listener.joinable()) {
        listener.join();
    }

    ::close(server_fd);

    std::cout << "Bye." << std::endl;
    return 0;
}
