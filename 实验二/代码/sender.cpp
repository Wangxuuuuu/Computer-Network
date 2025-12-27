#include "rdt.h"
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
#include <thread>

using namespace std;

// 全局变量
SOCKET sock;
sockaddr_in serverAddr;
int addrLen = sizeof(serverAddr);
double packetLossRate = 0.0; // 丢包率 (0.0 - 1.0)
int maxWindowSize = 20;      // 最大发送窗口大小 (默认20)
int delayMs = 0;             // 模拟延时 (毫秒)

// RENO 状态
enum RenoState {
    SLOW_START,                     // 慢启动
    CONGESTION_AVOIDANCE,           // 拥塞避免
    FAST_RECOVERY                   // 快速恢复
};

// 发送缓冲区中的包结构
struct SenderPacket {
    Packet pkt;         // 数据包
    bool acked;         // 是否被确认
    bool sent;          // 是否已发送
    chrono::steady_clock::time_point sendTime; // 发送时间,用于超时检测
};

vector<SenderPacket> packets;   // 发送缓冲区,索引对应包的序列号减1(从 0 开始，而 seq 从 1 开始)
double cwnd = 1.0;              // 拥塞窗口，代表“发送方觉得网络能承受多少包”
int ssthresh = 16;              // 慢启动阈值
int dupAckCount = 0;            // 重复 ACK 计数器,用于快速重传
RenoState state = SLOW_START;

// 发送窗口变量
int base = 0;                   // 已确认的包的下一个索引(滑动窗口左边界)
int nextSeqNum = 0;             // 下一个要发送的包的索引(滑动窗口右边界)

// 统计信息
int totalBytesSent = 0;
auto startTime = chrono::steady_clock::now();

// 发送单个数据包
// seq_index: 包在 packets 中的索引
void send_packet(int seq_index) {
    if (seq_index >= packets.size()) return;
    
    SenderPacket& sp = packets[seq_index];

    // 模拟丢包 (仅针对数据包，不丢握手包)
    // 如果概率< packetLossRate则模拟丢包
    if (packetLossRate > 0.0 && ((rand() % 1000) / 1000.0 < packetLossRate)) {
        // cout << "[Simulated Loss] Packet " << sp.pkt.header.seq << " dropped." << endl;
        // 即使“丢包”，逻辑上也认为尝试发送了，只是没调用 sendto
        if (!sp.sent) {
            sp.sent = true;
            sp.sendTime = chrono::steady_clock::now();
        }
        return;
    }

    sp.pkt.header.checksum = 0;
    sp.pkt.header.checksum = calculate_checksum(&sp.pkt);
    
    // 模拟网络延时
    if (delayMs > 0) {
        this_thread::sleep_for(chrono::milliseconds(delayMs));
    }

    sendto(sock, (char*)&sp.pkt, sizeof(PacketHeader) + sp.pkt.header.length, 0, (sockaddr*)&serverAddr, addrLen);
    
    sp.sent = true;
    sp.sendTime = chrono::steady_clock::now();
    // print_packet_info("SEND", sp.pkt); // 调试输出
}

// 握手
bool handshake() {
    Packet synPkt;
    memset(&synPkt, 0, sizeof(synPkt));
    synPkt.header.flags = FLAG_SYN;
    synPkt.header.seq = 0;              // 初始序列号设置为0
    synPkt.header.length = 0;
    synPkt.header.checksum = calculate_checksum(&synPkt);

    // 发送 SYN
    sendto(sock, (char*)&synPkt, sizeof(PacketHeader), 0, (sockaddr*)&serverAddr, addrLen);
    cout << "[Handshake] SYN sent." << endl;

    // 等待 SYN + ACK
    Packet recvPkt;
    fd_set readfds;             // 文件描述符集合,用于 select 函数,监控套接字的可读状态
    timeval tv;                 // 超时设置
    tv.tv_sec = 2;              // 2秒超时
    tv.tv_usec = 0;             // 微秒部分设置为0

    FD_ZERO(&readfds);          // 清空集合
    FD_SET(sock, &readfds);     // 将套接字加入集合,以监控其可读状态

    // select 等待,它的作用是“带超时的等待”.如果 2 秒内没收到服务器的回复，select 就会返回 0，握手失败,避免程序死锁。
    int ret = select(0, &readfds, NULL, NULL, &tv);

    if (ret > 0) {
        int len = recvfrom(sock, (char*)&recvPkt, sizeof(recvPkt), 0, (sockaddr*)&serverAddr, &addrLen);
        if (len > 0 && (recvPkt.header.flags & (FLAG_SYN | FLAG_ACK))) {
            if (calculate_checksum(&recvPkt) == recvPkt.header.checksum) {
                cout << "[Handshake] SYN+ACK received." << endl;
                
                // 发送 ACK
                Packet ackPkt;
                memset(&ackPkt, 0, sizeof(ackPkt));
                ackPkt.header.flags = FLAG_ACK;
                ackPkt.header.seq = 1;                          // 客户端初始序列号为1
                ackPkt.header.ack = recvPkt.header.seq + 1;     // 确认号为服务器初始序列号+1
                ackPkt.header.length = 0;
                ackPkt.header.checksum = calculate_checksum(&ackPkt);
                
                sendto(sock, (char*)&ackPkt, sizeof(PacketHeader), 0, (sockaddr*)&serverAddr, addrLen);
                cout << "[Handshake] ACK sent. Connection Established." << endl;
                return true;
            }
        }
    }
    
    cout << "[Handshake] Failed." << endl;
    return false;
}

// 读取文件并打包
void load_file(const string& filename) {
    ifstream file(filename, ios::binary);       // 以二进制模式打开文件
    if (!file.is_open()) {
        cerr << "Failed to open file: " << filename << endl;
        exit(1);
    }

    file.seekg(0, ios::end);               // file.seekg 用于设置输入流的读取位置,这里将读取位置移动到文件末尾
    int fileSize = file.tellg();           // file.tellg 用于获取当前读取位置的偏移量,这里获取的是文件大小
    file.seekg(0, ios::beg);               // 将读取位置移动回文件开头

    int seq = 1; // 数据包序列号从1开始
    while (file.tellg() < fileSize) {
        SenderPacket sp;
        memset(&sp, 0, sizeof(sp));
        sp.pkt.header.seq = seq++;          // 序列号从1开始递增
        sp.pkt.header.flags = 0; // 普通数据包
        sp.acked = false;
        sp.sent = false;

        int remaining = fileSize - (int)file.tellg();   // 剩余未读字节数
        int len = min(MSS, remaining);                  // 本次读取的字节数不超过 MSS 和剩余字节数
        file.read(sp.pkt.data, len);                    // 读取数据到包的 data 部分,长度为 len
        sp.pkt.header.length = len;                     // 设置包的长度字段为len
        
        packets.push_back(sp);                          // 将打包好的数据包加入发送缓冲区
    }
    file.close();
    cout << "File loaded. Total packets: " << packets.size() << endl;
}

// 挥手,关闭连接
void teardown() {
    Packet finPkt;
    memset(&finPkt, 0, sizeof(finPkt));
    finPkt.header.flags = FLAG_FIN;
    finPkt.header.seq = packets.size() + 1;     // FIN 包的序列号在发送的最后一个数据包之后
    finPkt.header.length = 0;
    finPkt.header.checksum = calculate_checksum(&finPkt);

    sendto(sock, (char*)&finPkt, sizeof(PacketHeader), 0, (sockaddr*)&serverAddr, addrLen);
    cout << "[Teardown] FIN sent." << endl;

    // 简单等待 ACK
    Packet recvPkt;
    timeval tv = {2, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    
    if (select(0, &readfds, NULL, NULL, &tv) > 0) {
        recvfrom(sock, (char*)&recvPkt, sizeof(recvPkt), 0, (sockaddr*)&serverAddr, &addrLen);
        if (recvPkt.header.flags & FLAG_ACK) {
            cout << "[Teardown] ACK received. Connection Closed." << endl;
        }
    }
}

int main(int argc, char* argv[]) {
    srand(time(0)); // 初始化随机种子

    if (argc < 4) {
        cout << "Usage: " << argv[0] << " <server_ip> <server_port> <file_path> [loss_rate] [window_size] [delay_ms]" << endl;
        cout << "Example: " << argv[0] << " 127.0.0.1 8080 data.txt 0.1 20 100" << endl;
        return 1;
    }

    string serverIp = argv[1];                  // 服务器 IP 地址
    int serverPort = atoi(argv[2]);             // 服务器端口
    string filePath = argv[3];                  // 发送文件路径
    if (argc >= 5) {                            // 可选参数:丢包率
        packetLossRate = atof(argv[4]);
        cout << "Packet Loss Rate set to: " << packetLossRate << endl;
    }
    if (argc >= 6) {                            // 可选参数:窗口大小
        maxWindowSize = atoi(argv[5]);
        cout << "Max Window Size set to: " << maxWindowSize << endl;
    }
    if (argc >= 7) {                            // 可选参数:延时
        delayMs = atoi(argv[6]);
        cout << "Delay set to: " << delayMs << " ms" << endl;
    }

    // 初始化 Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    serverAddr.sin_family = AF_INET;                // 地址族:IPv4
    serverAddr.sin_port = htons(serverPort);        // 主机字节序转换为网络字节序(大端)
    // 使用 inet_addr 替代 inet_pton 以兼容 MinGW
    serverAddr.sin_addr.s_addr = inet_addr(serverIp.c_str());       // 服务器 IP 地址

    if (!handshake()) {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // 读取并打包文件
    load_file(filePath);                    
    
    // 记录开始时间
    startTime = chrono::steady_clock::now();

    // 主循环：发送和接收
    while (base < packets.size()) {
        // 1. 发送窗口内的包
        int windowSize = min((int)cwnd, maxWindowSize);             // 计算当前窗口大小，取拥塞窗口和最大窗口的较小值
        // 发送窗口内未发送的包,直到达到窗口上限
        while (nextSeqNum < packets.size() && nextSeqNum < base + windowSize) {
            if (!packets[nextSeqNum].sent) {
                send_packet(nextSeqNum);
            }
            nextSeqNum++;
        }

        // 2. 接收 ACK
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeval tv = {0, 10000}; // 10ms 超时,这里的10ms代表等待ACK的时间间隔

        int ret = select(0, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            Packet recvPkt;
            sockaddr_in fromAddr;
            int fromLen = sizeof(fromAddr);
            int len = recvfrom(sock, (char*)&recvPkt, sizeof(recvPkt), 0, (sockaddr*)&fromAddr, &fromLen);
            
            if (len > 0 && (recvPkt.header.flags & FLAG_ACK)) {
                if (calculate_checksum(&recvPkt) == recvPkt.header.checksum) {
                    int ackSeq = recvPkt.header.ack;        // 接收端返回的是它收到的包的序列号(SR选择确认)
                    int ackIndex = ackSeq - 1;              // 索引号为序列号减1

                    // 如果该包在窗口范围内,那么标记为已确认
                    if (ackIndex >= base && ackIndex < packets.size()) {
                        if (!packets[ackIndex].acked) {
                            packets[ackIndex].acked = true;
                            
                            if (ackIndex == base) {
                                // 收到 Base 的 ACK，滑动窗口
                                while (base < packets.size() && packets[base].acked) {
                                    base++;
                                }
                                
                                // RENO: 收到新数据的 ACK
                                if (state == FAST_RECOVERY) {
                                    // 标准 RENO: 收到新数据的 ACK，退出快速恢复 (Deflation)
                                    // 将 cwnd 恢复为 ssthresh
                                    cwnd = (double)ssthresh;
                                    state = CONGESTION_AVOIDANCE;
                                    dupAckCount = 0;
                                } else {
                                    // 正常状态下的 ACK 处理
                                    dupAckCount = 0;
                                    if (state == SLOW_START) {
                                        cwnd += 1.0;                // 每收到一个 ACK，窗口加 1
                                        if (cwnd >= ssthresh) {
                                            state = CONGESTION_AVOIDANCE;
                                        }
                                    } else if (state == CONGESTION_AVOIDANCE) {
                                        cwnd += 1.0 / cwnd;         // 每收到一个 ACK，窗口加 1/cwnd
                                    }
                                }
                            } else {
                                // 收到乱序 ACK (SACK) -> 视为重复 ACK
                                if (state == FAST_RECOVERY) {
                                    // 标准 RENO: 快速恢复期间，每收到一个重复 ACK，cwnd 加 1 (允许发送新数据)
                                    cwnd += 1.0;
                                } else {
                                    dupAckCount++;
                                    if (dupAckCount == 3) {
                                        // 快速重传
                                        cout << "[Fast Retransmit] Packet " << packets[base].pkt.header.seq << endl;
                                        send_packet(base); // 重传 Base

                                        // 进入快速恢复
                                        ssthresh = max(2, (int)cwnd / 2);   // 阈值减半
                                        cwnd = ssthresh + 3;                // 快速恢复阶段，拥塞窗口设置为阈值加3
                                        state = FAST_RECOVERY;              // 显式进入快速恢复状态
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 3. 处理超时
        if (base < packets.size()) {
            auto now = chrono::steady_clock::now();
            auto duration = chrono::duration_cast<chrono::milliseconds>(now - packets[base].sendTime).count();
            if (packets[base].sent && duration > TIMEOUT_MS) {
                cout << "[Timeout] Packet " << packets[base].pkt.header.seq << endl;
                send_packet(base); // 重传 Base
                
                // RENO 超时处理
                ssthresh = max(2, (int)cwnd / 2);
                cwnd = 1.0;                 // 重置为1
                state = SLOW_START;         // 重新进入慢启动阶段
                dupAckCount = 0;
            }
        }
        
        // 简单的流量控制显示
        // cout << "\rBase: " << base << " Cwnd: " << cwnd << " Ssthresh: " << ssthresh << flush;
    }

    auto endTime = chrono::steady_clock::now();
    auto totalTimeUs = chrono::duration_cast<chrono::microseconds>(endTime - startTime).count();
    double totalTimeSec = totalTimeUs / 1000000.0;      // 除以一百万转换为秒(microseconds转为seconds)
    
    // 计算吞吐率 (Bytes / Second) -> MB/s
    // 估算总传输数据量 = 包数量 * 数据长度 (忽略重传的开销，计算有效吞吐率 Goodput)
    long long totalBytes = 0;
    for(const auto& p : packets) totalBytes += p.pkt.header.length;
    
    double throughput = (double)totalBytes / 1024.0 / 1024.0 / totalTimeSec;        // MB/s
    
    cout << endl << "Transfer Complete!" << endl;
    cout << "Time: " << totalTimeSec << " s" << endl;
    cout << "Throughput: " << throughput << " MB/s" << endl;

    teardown();

    closesocket(sock);
    WSACleanup();
    return 0;
}
