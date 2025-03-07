#include <App.h>
#include "json.hpp"

#include <chrono>
#include <mutex>
#include <algorithm>
#include <thread>
#include <vector>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <atomic>
#include <ctime>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstdlib>

#define SSL false

using namespace std;

vector<string> clusterNodes;

int serverPort = 8000;
int clusterPort = 8035;
int threadCount = 4;

ofstream logMessagesFile;
bool logMessages = false;

using ChannelsMap = map<string, set<uWS::WebSocket<SSL, true> *>>;
using StringPtr = shared_ptr<string>;

struct PerSocketData
{
    string name;
    string channel;
    int64_t lastPing = 0;
    int64_t lastPingSent = 0;
    int64_t packets = 0;
    time_t packetsTime = 0;
};

struct PerThreadData
{
    uWS::Loop *loop = nullptr;
    ChannelsMap *channels = nullptr;
};

atomic<int64_t> connections = 0;
atomic<int64_t> exceptions = 0;
atomic<int64_t> blocked = 0;
atomic<int64_t> packets = 0;
atomic<int64_t> messageId = 0;

vector<mutex> mutexes(threadCount);
vector<thread *> threads(threadCount, nullptr);
vector<PerThreadData> threads_data(threadCount);

mutex globalMutex;
map<string, multiset<string>> globalChannels;

map<string, set<uWS::WebSocket<SSL, true> *>> channels;
mutex channelMutex;

int64_t millis()
{
    return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
}

string getCurrentTimeAsString()
{
    auto now = chrono::system_clock::now();
    time_t now_time = chrono::system_clock::to_time_t(now);
    auto ms = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()) % 1000;

    struct tm timeinfo;
    localtime_r(&now_time, &timeinfo); // Para evitar problemas com threads

    stringstream ss;
    ss << put_time(&timeinfo, "%Y-%m-%dT%H:%M:%S") << '.' << setfill('0') << setw(3) << ms.count() << "Z";

    return ss.str();
}

void sendPing()
{
    static string pingStr = "{\"type\":\"ping\",\"ping\":";
    for (int index = 0; index < threadCount; ++index)
    {
        lock_guard<mutex> lock(mutexes[index]);
        if (!threads_data[index].loop)
        {
            continue;
        }

        threads_data[index].loop->defer([index]
                                        {
            for(auto& channel : *(threads_data[index].channels)) {
                for(auto& ws : channel.second) {
                    PerSocketData* userData = (PerSocketData*)(ws->getUserData());
                    userData->lastPingSent = millis();
                    ws->send(pingStr + to_string(userData->lastPing) + "}", uWS::TEXT);                    
                }
            } });
    }
}

bool sendToCluster(const StringPtr &channel, const StringPtr &message)
{
    bool sent = false;

    if (clusterNodes.empty())
    {
        return sent;
    }

    for (const string &node : clusterNodes)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            continue;
        }

        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(clusterPort);
        inet_pton(AF_INET, node.c_str(), &serverAddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) >= 0)
        {
            string msg = *channel + "|" + *message;
            send(sock, msg.c_str(), msg.size(), 0);
            sent = true;
        }

        close(sock);
    }

    return sent;
}

void dispatchMessage(const StringPtr &channel, const StringPtr &message)
{
    for (int index = 0; index < threadCount; ++index)
    {
        lock_guard<mutex> lock(mutexes[index]);
        if (!threads_data[index].loop)
        {
            continue;
        }

        threads_data[index].loop->defer([index, channel, message]
                                        {
            auto it = threads_data[index].channels->find(*channel);
            if(it != threads_data[index].channels->end()) {
                for(auto& ws : it->second) {
                    ws->send(*message, uWS::TEXT);
                }
            } });
    }
}

void processMessage(uWS::WebSocket<SSL, true> *ws, string_view &raw_message, ChannelsMap *channels)
{
    packets += 1;
    PerSocketData *userData = (PerSocketData *)(ws->getUserData());
    auto msg = nlohmann::json::parse(raw_message);
    string type = msg["type"];

    if (userData->name.empty() || userData->channel.empty())
    {
        if (type != "init")
        {
            ws->end();
            return;
        }

        userData->name = msg["name"];
        userData->channel = msg["channel"];
        userData->lastPing = 0;
        userData->lastPingSent = millis();

        if (userData->name.empty() || userData->channel.empty() || userData->name.size() > 35 || userData->channel.size() > 30)
        {
            blocked += 1;
            ws->end();
            return;
        }

        auto it = channels->find(userData->channel);
        if (it != channels->end() && it->second.size() > 60)
        {
            blocked += 1;
            ws->end();
            return;
        }

        (*channels)[userData->channel].insert(ws);
        globalMutex.lock();
        globalChannels[userData->channel].insert(userData->name);
        globalMutex.unlock();
        return;
    }

    if (userData->packetsTime < time(nullptr))
    {
        userData->packetsTime = time(nullptr) + 1;
        userData->packets = 0;
    }

    userData->packets += 1;
    // 200 packets per second
    if (userData->packets > 200 || raw_message.size() > 10 * 1024)
    {
        blocked += 1;
        ws->end();
        return;
    }

    if (type == "ping")
    {
        userData->lastPing = millis() - userData->lastPingSent;
        return;
    }

    if (type != "message")
    {
        ws->end();
        return;
    }

    string topic = msg["topic"];
    if (topic.empty() || topic.size() > 30)
    {
        ws->end();
        return;
    }

    nlohmann::json response;
    response["type"] = "message";
    response["id"] = ++messageId;
    response["name"] = userData->name;
    response["topic"] = topic;

    // special topics
    if (topic == "list")
    { // list of all users
        set<string> users;
        globalMutex.lock();
        auto it = globalChannels.find(userData->channel);
        if (it != globalChannels.end())
        {
            users = set<string>(it->second.begin(), it->second.end());
        }
        globalMutex.unlock();
        response["message"] = users;
        ws->send(response.dump(), uWS::TEXT);
        return;
    }

    // relay message
    response["message"] = msg["message"];

    auto channelPtr = make_shared<string>(userData->channel);
    auto responsePtr = make_shared<string>(response.dump());

    sendToCluster(channelPtr, responsePtr);
    dispatchMessage(channelPtr, responsePtr);

    // log message
    if (logMessages && logMessagesFile.is_open())
    {
        response["date"] = getCurrentTimeAsString();
        logMessagesFile <<  response.dump() << endl;
    }

    // cout << "Dispatching message: " << response.dump() << endl;
    return;
}

void clusterListener()
{
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0)
        return;

    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(clusterPort);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
        return;
    listen(serverSock, 10);

    cout << "Cluster listener on port " << clusterPort << " online!" << endl;

    while (true)
    {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock < 0)
            continue;

        char buffer[1024] = {0};
        int bytesRead = read(clientSock, buffer, 1024);
        if (bytesRead > 0)
        {
            string data(buffer);
            size_t delimiter = data.find("|");
            if (delimiter != string::npos)
            {
                string channel = data.substr(0, delimiter);
                string message = data.substr(delimiter + 1);

                // cout << "Receveid cluster message: " << channel << " " << message << endl;

                dispatchMessage(make_shared<string>(channel), make_shared<string>(message));
            }
        }
        close(clientSock);
    }
}

void parseClusterNodes(const string &nodes)
{
    size_t pos = 0;
    string temp = nodes;
    while ((pos = temp.find(',')) != string::npos)
    {
        clusterNodes.push_back(temp.substr(0, pos));
        temp.erase(0, pos + 1);
    }

    if (!temp.empty())
    {
        clusterNodes.push_back(temp);
    }
}

void parseArguments(int argc, char *argv[])
{
    string cluster_nodes;
    string cluster_port;
    string port;
    string threads;

    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--cluster-nodes" && i + 1 < argc)
        {
            cluster_nodes = argv[i + 1];
        }
        if (arg == "--cluster-port" && i + 1 < argc)
        {
            cluster_port = argv[i + 1];
        }
        if (arg == "--port" && i + 1 < argc)
        {
            port = argv[i + 1];
        }
        if (arg == "--threads" && i + 1 < argc)
        {
            threads = argv[i + 1];
        }
        if (arg == "--log-messages")
        {
            logMessages = true;
        }
    }

    if (cluster_nodes.empty())
    {
        char *envClusters = getenv("CLUSTER_NODES");
        if (envClusters)
        {
            cluster_nodes = string(envClusters);
        }
    }

    if (cluster_port.empty())
    {
        char *envClusterPort = getenv("CLUSTER_PORT");
        if (envClusterPort)
        {
            cluster_port = string(envClusterPort);
        }
    }

    if (port.empty())
    {
        char *envPort = getenv("PORT");
        if (envPort)
        {
            port = string(envPort);
        }
    }

    if (threads.empty())
    {
        char *envThreads = getenv("THREADS");
        if (envThreads)
        {
            threads = string(envThreads);
        }
    }

    if (!cluster_nodes.empty())
    {
        parseClusterNodes(cluster_nodes);
    }

    if (!cluster_port.empty())
    {
        clusterPort = stoi(cluster_port);
    }

    if (!port.empty())
    {
        serverPort = stoi(port);
    }

    if (!threads.empty())
    {
        threadCount = stoi(threads);
    }

    cout << "Starting websocket server ";
    if (!clusterNodes.empty())
    {
        cout << "(nodes:";
        for (const auto &node : clusterNodes)
        {
            cout << " " << node;
        }
        cout << ") ";
    }
    else
    {
        cout << "(no clustered) ";
    }

    cout << "on port " << serverPort << " using " << threadCount << " threads..." << endl;

    if (logMessages && !logMessagesFile.is_open())
    {
        logMessagesFile.open("messages.log", ios::out | ios::app);
        if (!logMessagesFile.is_open())
        {
            cerr << "Failed to open messages.log file" << endl;
        }
    }
}

int main(int argc, char *argv[])
{
    parseArguments(argc, argv);

    thread clusterThread(clusterListener);

    for (int index = 0; index < threadCount; ++index)
    {
        threads[index] = new thread([index]
                                    {
            ChannelsMap* channels = new ChannelsMap();

            mutexes[index].lock();
            threads_data[index].loop = uWS::Loop::get();
            threads_data[index].channels = channels;
            mutexes[index].unlock();            
            
            uWS::TemplatedApp<SSL>()
                .get("/", [](auto *res, auto *req) {
                    res->writeHeader("Content-Type", "application/json")->end("{\"status\":\"ok\"}");
                })
                .get("/usage", [](auto *res, auto *req) {
                    nlohmann::json resp;

                    resp["date"] = getCurrentTimeAsString();
                    resp["connections"] = connections.load();
                    resp["packets"] = packets.load();
                    resp["exceptions"] = exceptions.load();
                    resp["blocked"] = blocked.load();
    
                    res->writeHeader("Content-Type", "application/json")->end(resp.dump());
                })
                .ws<PerSocketData>("/*", {
                .compression = uWS::SHARED_COMPRESSOR,
                .maxPayloadLength = 64 * 1024,
                .idleTimeout = 30,
                .maxBackpressure = 256 * 1024,
                .upgrade = nullptr,
                .open = [](uWS::WebSocket<SSL, true> *ws) {
                    connections += 1;
                },
                .message = [&channels](auto *ws, string_view message, uWS::OpCode opCode) {
                    if(opCode != uWS::TEXT || message.size() > 64 * 1024) {
                        return;
                    }
                    
                    try {
                        processMessage(ws, message, channels);
                    } catch(...) {
                        exceptions += 1;
                        ws->end();
                    }
                },
                .drain = nullptr,
                .ping = nullptr,
                .pong = nullptr,                
                .close = [&channels](uWS::WebSocket<SSL, true> *ws, int code, string_view message) {
                    string& name = ((PerSocketData*)(ws->getUserData()))->name;
                    string& channel = ((PerSocketData*)(ws->getUserData()))->channel;
                    if(!channel.empty()) {
                        auto it = channels->find(channel);
                        if(it != channels->end()) {
                            it->second.erase(ws);
                            if(it->second.empty()) {
                                channels->erase(it);
                            }
                        }
                    }
                    globalMutex.lock();
                    auto it = globalChannels.find(channel);
                    if(it != globalChannels.end()) {
                        auto uit = it->second.find(name);
                        if(uit != it->second.end()) {
                            it->second.erase(uit);
                        }
                        if(it->second.empty()) {
                            globalChannels.erase(it);
                        }
                    }                    
                    globalMutex.unlock();
                    connections -= 1;
                }
            }).listen(serverPort, [index](auto *token) {
                if (!token) {
                    cout << "Thread " << index << " failed to listen on port " << serverPort << endl;
                }
            }).run();
            
            mutexes[index].lock();
            threads_data[index].loop = nullptr;
            threads_data[index].channels = nullptr;
            delete channels;
            mutexes[index].unlock(); });
    };

    bool working = true;
    while (working)
    {
        this_thread::sleep_for(chrono::milliseconds(2500));
        cout << "Connections: " << connections << " Packets: " << packets << " Exceptions: " << exceptions << " Blocked: " << blocked << endl;

        // send ping
        sendPing();

        working = false;
        for (int index = 0; index < threadCount; ++index)
        {
            lock_guard<mutex> lock(mutexes[index]);
            if (!threads_data[index].loop)
            {
                if (threads[index])
                {
                    threads[index]->join();
                    delete threads[index];
                    threads[index] = nullptr;
                }
                continue;
            }
            working = true;
        }
    }
    return 0;
}
