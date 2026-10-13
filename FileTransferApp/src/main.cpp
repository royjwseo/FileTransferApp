#define NOMINMAX
#include <atomic>
#include <iostream>
#include <exception>
#include <limits>
#include <string>
#include <thread>

#include "Network.h"

namespace
{
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

int main()
{
    Network network;

    if (!network.Initialize())
        return 1;

    std::cout << "=== FileTransferApp ===\n";
    std::cout << "1. Host\n";
    std::cout << "2. Connect\n";
    std::cout << "Select: ";

    int choice = 0;

    if (!(std::cin >> choice))
        return 1;

    bool connected = false;

    if (choice == 1)
    {
        connected = network.StartServer(9000);
    }
    else if (choice == 2)
    {
        std::string ip;

        std::cout << "IP: ";
        std::cin >> ip;

        connected = network.Connect(ip, 9000);
    }

    if (!connected)
        return 1;

    std::thread receiver;
    std::thread fileSender;
    std::atomic<bool> sendingFile{ false };
    SessionCleanup cleanup{ network, receiver, fileSender };

    try
    {
        receiver = std::thread(&Network::ReceiveLoop, &network);

        std::cin.ignore(
            std::numeric_limits<std::streamsize>::max(),
            '\n');

        std::cout << "\nCommands:\n"
            << "  /file <path>  Send file\n"
            << "  /accept       Accept incoming file\n"
            << "  /reject       Reject incoming file\n"
            << "  /quit         Exit\n"
            << "  other text    Send text\n\n";

        std::string message;

        while (std::getline(std::cin, message))
        {
            if (message == "/quit")
                break;

            if (message == "/accept")
            {
                network.AcceptFile();
            }
            else if (message == "/reject")
            {
                network.RejectFile();
            }
            else if (message.rfind("/file ", 0) == 0)
            {
                // 이미 파일 전송 중이면 거부
                if (sendingFile.load())
                {
                    std::cout << "File transfer in progress\n";
                    continue;
                }

                // 이전 작업이 끝났다면 스레드 회수
                if (fileSender.joinable())
                    fileSender.join();

                std::string path = message.substr(6);

                if (path.size() >= 2 &&
                    path.front() == '"' &&
                    path.back() == '"')
                {
                    path = path.substr(1, path.size() - 2);
                }

                sendingFile.store(true);

                fileSender = std::thread(
                    [&network, &sendingFile, path]()
                    {
                        try
                        {
                            bool success = network.SendFile(path);

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

                std::cout << "File send task started\n";
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
