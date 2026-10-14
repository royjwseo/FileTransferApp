#define NOMINMAX
#include <atomic>
#include <charconv>
#include <iostream>
#include <syncstream>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

#include "Network.h"

namespace
{
    void PrintVersion()
    {
        std::osyncstream(std::cout) << "FileTransferApp protocol=" << Protocol::WIRE_VERSION
            << " requestId64-parity maxFileBytes=" << Protocol::MAX_FILE_SIZE << '\n';
    }

    void PrintUsage()
    {
        std::osyncstream(std::cout) << "Usage:\n"
            << "  FileTransferApp.exe                         Interactive menu, port 9000\n"
            << "  FileTransferApp.exe --host [--bind IPv4] [--port 1..65535] [--reconnect]\n"
            << "  FileTransferApp.exe --connect IPv4 [--port 1..65535] [--reconnect]\n"
            << "  FileTransferApp.exe --version\n"
            << "  FileTransferApp.exe --help\n";
    }

    // Declared after the threads and before they start, so exception unwinding
    // always interrupts I/O and joins workers before Network is destroyed.
    struct SessionCleanup
    {
        Network& network;
        std::thread& receiver;
        std::thread& sender;

        ~SessionCleanup()
        {
            network.RequestStop();
            if (sender.joinable()) sender.join();
            if (receiver.joinable()) receiver.join();
            network.Disconnect();
        }
    };
}

int main(int argc, char* argv[])
{
    const bool interactive = argc == 1;
    int choice = 0;
    uint16_t port = 9000;
    std::string ip, bindIp = "0.0.0.0";
    if (argc == 2 && std::string_view(argv[1]) == "--version")
    {
        PrintVersion();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        PrintUsage();
        return 0;
    }
    bool valid = true, hasPort = false, hasBind = false, autoReconnect = false;
    for (int i = 1; i < argc && valid; ++i)
    {
        const std::string_view arg(argv[i]);
        if (arg == "--reconnect" && !autoReconnect) autoReconnect = true;
        else if (arg == "--host" && choice == 0) choice = 1;
        else if (arg == "--connect" && choice == 0 && i + 1 < argc)
        {
            choice = 2;
            ip = argv[++i];
        }
        else if (arg == "--bind" && !hasBind && i + 1 < argc)
        {
            hasBind = true;
            bindIp = argv[++i];
        }
        else if (arg == "--port" && !hasPort && i + 1 < argc)
        {
            hasPort = true;
            const std::string_view value(argv[++i]);
            unsigned number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            valid = result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                number > 0 && number <= 65535;
            if (valid) port = static_cast<uint16_t>(number);
        }
        else valid = false;
    }
    if (!interactive && (!valid || choice == 0 || (hasBind && choice != 1)))
    {
        std::cerr << "Invalid command line\n";
        PrintUsage();
        return 1;
    }

    Network network;

    if (!network.Initialize())
        return 1;

    std::osyncstream(std::cout) << "=== FileTransferApp ===\n";
    PrintVersion();
    if (interactive)
    {
        std::osyncstream(std::cout) << "1. Host\n2. Connect\nSelect: ";
        if (!(std::cin >> choice)) return 1;
    }

    bool connected = false;

    if (choice == 1)
    {
        connected = network.StartServer(port, bindIp);
    }
    else if (choice == 2)
    {
        if (interactive)
        {
            std::osyncstream(std::cout) << "IP: ";
            std::cin >> ip;
        }
        connected = network.Connect(ip, port);
    }

    if (!connected)
        return 1;
    network.EnableAutoReconnect(autoReconnect);

    std::thread receiver;
    std::thread fileSender;
    std::atomic<bool> sendingFile{ false };
    SessionCleanup cleanup{ network, receiver, fileSender };

    try
    {
        receiver = std::thread(&Network::ReceiveLoop, &network);

        if (interactive)
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        std::osyncstream(std::cout) << "\nCommands:\n"
            << "  /file <path>  Send file\n"
            << "  /resume <transferId> <path> Resume owned partial file\n"
            << "  /status <transferId> <path> Verify prior commit\n"
            << "  /accept       Accept incoming file\n"
            << "  /reject       Reject incoming file\n"
            << "  /cancel [requestId] Cancel one active file transfer\n"
            << "  /reconnect    Reconnect to the same endpoint\n"
            << "  /quit         Exit\n"
            << "  other text    Send text\n\n";

        std::string message;

        while (std::getline(std::cin, message))
        {
            if (message == "/quit")
                break;

            if (message == "/reconnect")
            {
                if (network.IsConnected() || network.AutoReconnectEnabled())
                {
                    std::osyncstream(std::cout) << "Already connected or automatic reconnect enabled\n" << std::flush;
                    continue;
                }
                if (fileSender.joinable()) fileSender.join();
                if (receiver.joinable()) receiver.join();
                if (network.Reconnect())
                {
                    std::osyncstream(std::cout) << "[Reconnected] new requestId session\n" << std::flush;
                    receiver = std::thread(&Network::ReceiveLoop, &network);
                }
                else std::osyncstream(std::cout) << "Reconnect failed\n" << std::flush;
            }
            else if (message == "/cancel" || message.rfind("/cancel ", 0) == 0)
            {
                uint64_t id = 0;
                if (message != "/cancel")
                {
                    const std::string_view value(message.data() + 8, message.size() - 8);
                    const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
                    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !id)
                    {
                        std::osyncstream(std::cout) << "Invalid requestId\n";
                        continue;
                    }
                }
                network.CancelFile(id);
            }
            else if (message == "/accept")
            {
                network.AcceptFile();
            }
            else if (message == "/reject")
            {
                network.RejectFile();
            }
            else if (message.rfind("/file ", 0) == 0 || message.rfind("/resume ", 0) == 0 ||
                message.rfind("/status ", 0) == 0)
            {
                // 이미 파일 전송 중이면 거부
                if (sendingFile.load())
                {
                    std::osyncstream(std::cout) << "File transfer in progress\n";
                    continue;
                }

                // 이전 작업이 끝났다면 스레드 회수
                if (fileSender.joinable())
                    fileSender.join();

                uint8_t mode = message.rfind("/resume ", 0) == 0 ? 1 :
                    message.rfind("/status ", 0) == 0 ? 2 : 0;
                std::string transferId;
                std::string path = message.substr(mode ? 8 : 6);
                if (mode) {
                    const auto separator = path.find(' ');
                    if (separator != 64 || separator + 1 == path.size()) {
                        std::osyncstream(std::cout) << "Usage: /resume|/status <transferId> <path>\n"; continue;
                    }
                    transferId = path.substr(0, separator); path.erase(0, separator + 1);
                }

                if (path.size() >= 2 &&
                    path.front() == '"' &&
                    path.back() == '"')
                {
                    path = path.substr(1, path.size() - 2);
                }

                sendingFile.store(true);

                fileSender = std::thread(
                    [&network, &sendingFile, path, transferId, mode]()
                    {
                        try
                        {
                            bool success = network.SendFile(path, transferId, mode);

                            if (!success)
                                std::cerr << "\nFile transfer not completed\n";
                        }
                        catch (const std::exception& e)
                        {
                            std::cerr << "File send failed: " << e.what() << '\n';
                            network.RequestStop();
                        }
                        catch (...)
                        {
                            std::cerr << "File send failed: unknown exception\n";
                            network.RequestStop();
                        }

                        sendingFile.store(false);
                    });

                std::osyncstream(std::cout) << "File send task started\n";
            }
            else
            {
                if (!network.SendText(message))
                {
                    std::cerr << "Text send failed\n";
                    break;
                }
            }
        }

    }
    catch (const std::exception& e)
    {
        std::cerr << "Session failed: " << e.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "Session failed: unknown exception\n";
        return 1;
    }

    return 0;
}
