#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic> // 引入 atomic

#pragma comment(lib, "ws2_32.lib")

// 全局退出标志，使用 atomic 保证线程间可见性和原子性
static std::atomic<bool> g_quit_flag{false};

// 稳定发送所有数据的函数
bool sendAll(SOCKET sock, const std::string& message) {
    const char* data = message.c_str();
    int total = message.size();
    int sent = 0;
    int bytesleft = total;
    int n;

    while (sent < total) {
        n = send(sock, data + sent, bytesleft, 0);
        if (n == SOCKET_ERROR) {
            std::cerr << "[错误] 发送数据失败: " << WSAGetLastError() << std::endl;
            return false;
        }
        sent += n;
        bytesleft -= n;
    }
    return true;
}

// 接收消息的线程函数,并接收退出标志的引用
void receiveMessages(SOCKET server_socket, std::atomic<bool>& quit_flag_ref) {
    char buffer[4096];
    int bytes_received;

    while (!quit_flag_ref.load()) { // 每次循环都检查退出标志,只要退出标志是 false，就继续循环
        // 使用 select 设置一个短暂的超时，避免 recv 永久阻塞
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        timeval timeout;
        timeout.tv_sec = 0;  // 0秒
        timeout.tv_usec = 100000; // 100毫秒

        int activity = select(0, &read_fds, NULL, NULL, &timeout);

        if (activity > 0) {
            // Socket 有数据可读
            if (FD_ISSET(server_socket, &read_fds)) {
                bytes_received = recv(server_socket, buffer, sizeof(buffer) - 1, 0);
                if (bytes_received > 0) {
                    buffer[bytes_received] = '\0';
                    // 服务器发送的消息以 \n 结尾，直接打印即可
                    std::cout << buffer; // buffer中已经包含 \n
                } else if (bytes_received == 0) {
                    // 服务器关闭了连接
                    std::cout << "[系统] 与服务器的连接已断开。" << std::endl;
                    break; // 退出循环
                } else {
                    // recv 出错, 但在使用 select 后，可能是由本地 close 引起的，检查退出标志
                    if (!quit_flag_ref.load()) {
                         std::cerr << "[错误] 接收服务器消息失败: " << WSAGetLastError() << std::endl;
                    }
                    break; // 无论何种错误，都退出循环
                }
            }
        } else if (activity == 0) {
            // select 超时，继续下一次循环检查 quit_flag
            continue;
        } else {
            // select 本身出错, 但在使用 select 后，可能是由本地 close 引起的，检查退出标志
            if (!quit_flag_ref.load()) {
                std::cerr << "[错误] select() 失败: " << WSAGetLastError() << std::endl;
            }
            break; // 退出循环
        }
    }
    // 线程结束时打印日志
    // std::cout << "[调试] 接收线程已退出。" << std::endl;
}


int main() {
    // 设置控制台输入输出为UTF-8编码，以便正确处理中文
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // 重置退出标志 (以防之前运行过)
    g_quit_flag.store(false);

    // 1. 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[错误] WSAStartup 失败" << std::endl;
        return 1;
    }
    std::cout << "[系统] Winsock 初始化成功。" << std::endl;

    // 2. 创建socket
    SOCKET client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_socket == INVALID_SOCKET) {
        std::cerr << "[错误] 创建socket失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }
    std::cout << "[系统] 客户端socket创建成功。" << std::endl;

    // 3. 获取服务器地址和端口
    std::string server_ip;
    int server_port;
    std::cout << "[提示] 请输入服务器IP地址 (例如 127.0.0.1): ";
    std::cin >> server_ip;
    std::cout << "[提示] 请输入服务器端口号 (例如 1221): ";
    std::cin >> server_port;

    // 4. 连接到服务器
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // 使用 inet_addr 解析 IPv4 地址 
    server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());
    if (server_addr.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "[错误] 无效的IPv4地址格式: " << server_ip << std::endl;
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }

    if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[错误] 连接服务器失败: " << WSAGetLastError() << " (请检查服务器是否运行以及IP/端口是否正确)" << std::endl;
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[系统] 成功连接到服务器 " << server_ip << ":" << server_port << "!" << std::endl;

    // 5. 发送用户名
    std::string username;
    std::cout << "[提示] 请输入你的聊天昵称: ";
    std::cin.ignore(); // 清除之前输入的换行符
    std::getline(std::cin, username);

    // 协议规定：第一条消息是用户名，以 \n 结尾
    std::string username_msg = username + "\n";
    if (!sendAll(client_socket, username_msg)) {
        std::cerr << "[错误] 发送用户名失败。" << std::endl;
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[系统] 用户名 \"" << username << "\" 已发送。" << std::endl;

    // 6. 创建接收消息的线程 - 传递退出标志的引用
    std::thread receiver_thread(receiveMessages, client_socket, std::ref(g_quit_flag));
    std::cout << "[系统] 消息接收线程已启动。" << std::endl;

    // 7. 主线程用于发送消息
    std::cout << "[提示] 现在可以开始聊天了。输入消息并按回车发送，输入 /quit 退出。" << std::endl;
    std::string message;
    bool user_quit = false;
    while (!user_quit && std::getline(std::cin, message)) {
        if (message == "/quit") {
            std::cout << "[系统] 正在退出..." << std::endl;
            user_quit = true; // 标记用户请求退出
            break; // 跳出循环
        }

        if (!message.empty()) {
            // 协议规定：聊天消息以 \n 结尾
            std::string chat_msg = message + "\n";
            if (!sendAll(client_socket, chat_msg)) {
                std::cerr << "[错误] 发送聊天消息失败。" << std::endl;
                user_quit = true; // 发送出错也标记退出
                break; // 跳出循环
            }
        }
    }

    // 8. 关闭
    // 通知接收线程准备退出
    g_quit_flag.store(true);
    // 关闭socket，这会使得接收线程中的 select/recv 立刻返回
    shutdown(client_socket, SD_BOTH); // 关闭读写
    closesocket(client_socket);
    // 等待接收线程结束
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    // 清理 Winsock
    WSACleanup();
    std::cout << "[系统] 客户端已关闭。" << std::endl;
    return 0;
}
