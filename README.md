# Websocket BOTServer (With Clustering MODE)
This is a simple websocket server for otclientv8 bots. It is based on uWebSockets and is designed to be used with otclientv8 bots.

## Compilation
Requirements:
- C++17 compiler
- libz-dev
- https://github.com/uNetworking/uWebSockets
- https://github.com/uNetworking/uSockets
- https://github.com/nlohmann/json/blob/develop/single_include/nlohmann/json.hpp

Run the following command to compile the server:
```console
g++ main.cpp -std=c++17 -Ofast uWebSockets/uSockets/uSockets.a -IuWebSockets/src -IuWebSockets/uSockets/src -lz -lpthread -o botserver
```

## Starting the server
Then `./botserver` to start the server on port 8000

### Cluster (peer to peer)
Then on all nodes `./botserver --cluster-nodes 172.18.1.10,172.18.1.11,172.18.1.13`

### Cluster (master-slave)
- Then in slave node `./botserver`
- Then in master node `./botserver --cluster-nodes slave_ip`

## Docker build
Then `docker build -t t-botserver .` in the root directory of this repository

## Docker run
Then `docker run -p 8000:8000 t-botserver` to start the server on port 8000

### Cluster (peer to peer)
Then on all nodes `docker run -p 8000:8000 -p 8035:8035 -e CLUSTER_NODES=172.18.1.10,172.18.1.11,172.18.1.13 t-botserver` to start the server on port 8000 and cluster on port 8035.

### Cluster (master-slave)
- Then slave node `docker run -p 8000:8000 t-botserver`
- Then master node `docker run -p 8000:8000 -p 8035:8035 -e CLUSTER_NODES=slave_ip t-botserver`

## Environment variables
| Name | Description | Default Value |
| --- | --- | --- |
| `CLUSTER_NODES` | Comma separated list of cluster nodes IP addresses | |
| `CLUSTER_PORT` | Cluster port | 8035 |
| `PORT` | Bot server port | 8000 |
| `THREADS` | Number of threads | 2 |

## Using it
If you want to use your bot server in bot, before calling BotServer.init set BotServer.url, for example:
```lua
BotServer.url = "ws://127.0.0.1:8000/"
BotServer.init(name(), "test_channel")
```

## BOTServer usage details (API)
Link: `http://ip:port/usage` - Returns the usage of the server