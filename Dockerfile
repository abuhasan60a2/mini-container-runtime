FROM ubuntu:22.04

# Avoid interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    make \
    iproute2 \
    iptables \
    net-tools \
    wget \
    curl \
    libjansson-dev \
    strace \
    procps \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /minibox
COPY . .

RUN make clean && make

# Default: open a shell for interactive testing
CMD ["/bin/bash"]
