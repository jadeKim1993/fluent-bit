FROM debian:trixie-slim
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    build-essential ca-certificates make cmake pkg-config libssl-dev libcurl4-openssl-dev \
    libsasl2-dev zlib1g-dev flex bison libyaml-dev procps python3 \
    && rm -rf /var/lib/apt/lists/*
