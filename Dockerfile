FROM eibly/ubuntu:LTS

ARG DEBIAN_FRONTEND=noninteractive
ARG TZ=Etc/UTC

ENV DEBIAN_FRONTEND=${DEBIAN_FRONTEND}
ENV TZ=${TZ}

RUN \
    # Update packages
    apt-get update && \
    # Install dependencies
    apt-get install -y git g++ zlib1g-dev unzip build-essential curl && \
    # Create user ubuntu with a random password
    useradd -m -s /bin/bash ubuntu && \
    echo "ubuntu:$(openssl rand -base64 16)" | chpasswd && \
    usermod -aG sudo ubuntu && \
    # Clone project and dependencies
    git clone https://github.com/kleberholtz/t-botserver.git && \
    cd t-botserver && \
    curl -o json.hpp https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp > /dev/null 2>&1 && \
    curl -o uWebSockets-18.9.0.zip https://github.com/uNetworking/uWebSockets/archive/refs/tags/v18.9.0.zip > /dev/null 2>&1 && \
    unzip uWebSockets-18.9.0.zip && \
    mv uWebSockets-18.9.0 uWebSockets && \
    cd uWebSockets && \
    rm -rf uSockets && \
    curl -o uSockets-0.5.0.zip https://github.com/uNetworking/uSockets/archive/refs/tags/v0.5.0.zip > /dev/null 2>&1 && \
    unzip uSockets-0.5.0.zip && \
    mv uSockets-0.5.0 uSockets && \
    make && \
    cd .. && \
    # Build
    g++ main.cpp -std=c++17 -Ofast uWebSockets/uSockets/uSockets.a -IuWebSockets/src -IuWebSockets/uSockets/src -lz -lpthread -o botserver && \
    # Move to home directory
    mv /t-botserver/botserver /home/ubuntu/botserver && \
    chown -R ubuntu:ubuntu /home/ubuntu/botserver && \
    chmod +x /home/ubuntu/botserver && \
    # Clean
    apt-get remove -y git g++ zlib1g-dev unzip build-essential && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    rm -rf /t-botserver

USER ubuntu

WORKDIR /home/ubuntu

# Expose ports (8000 for WS and 8035 for Clustering)
EXPOSE 8000
EXPOSE 8035

ENTRYPOINT [ "/home/ubuntu/botserver" ]
