FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    libreadline-dev \
    zlib1g-dev \
    flex \
    bison \
    git \
    gdb \
    locales \
    perl \
    pkg-config \
    llvm-dev \
    clang \
    liblz4-dev \
    libzstd-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV PATH=/usr/local/pgsql/bin:$PATH

# UID 1000 matches the typical first user on Ubuntu hosts, so bind-mounted
# files stay readable/writable without permission issues.
RUN useradd -m -u 1000 -s /bin/bash pgdev && \
    mkdir -p /usr/local/pgsql && \
    chown pgdev:pgdev /usr/local/pgsql

USER pgdev
WORKDIR /postgres

CMD ["/bin/bash"]