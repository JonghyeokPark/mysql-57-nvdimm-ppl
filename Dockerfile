FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    bison \
    libreadline8 \
    libreadline-dev \
    libaio1 \
    libaio-dev \
    libssl-dev \
    libncurses5 \
    libncurses5-dev \
    libmysqlclient-dev \
    gnuplot \
    python3 \
    git \
    vim \
    sudo \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /bin/false mysql

WORKDIR /work

CMD ["/bin/bash"]