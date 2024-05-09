FROM ubuntu:22.04

RUN \
    echo 'Acquire::ForceIPv4 "true";' >> /etc/apt/apt.conf.d/1000-force-ipv4-transport && \
    apt-get update && \
    PKGS="curl git unzip wget zip locales sudo" && \
    PKGS="${PKGS} build-essential ccache vim cmake" && \
    PKGS="${PKGS} linux-tools-common linux-tools-generic" && \
    PKGS="${PKGS} python3 python3-pip libunwind-dev" && \
    apt-get install -y ${PKGS} && \
    DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai apt-get -y install tzdata && \
    pip3 install compiledb

# install docker cli
RUN \
    # Add Docker's official GPG key:
    apt-get install -y ca-certificates curl && \
    install -m 0755 -d /etc/apt/keyrings && \
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc && \
    chmod a+r /etc/apt/keyrings/docker.asc && \
    # Add the repository to Apt sources:
    echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu jammy stable" > /etc/apt/sources.list.d/docker.list && \
    apt-get update && \
    apt-get install -y docker-ce-cli docker-buildx-plugin docker-compose-plugin && \
    apt-get autoclean && apt-get autoremove && \
    rm -rf /var/lib/apt/lists/*

ENV LANG en_US.UTF-8  
ENV LANGUAGE en_US:en  
ENV LC_ALL en_US.UTF-8  
