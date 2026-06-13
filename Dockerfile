# Stage 1: Build
FROM drogonframework/drogon:latest AS builder

RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

RUN apt update && \
    apt install -y --no-install-recommends git cmake build-essential libssl-dev openssl libpq5 libpq-dev && \
    git clone --depth 1 --branch v0.7.0 https://github.com/Thalhammer/jwt-cpp.git /tmp/jwt-cpp && \
    cd /tmp/jwt-cpp && mkdir build && cd build && \
    cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) && make install && rm -rf /tmp/jwt-cpp && \
    mkdir -p /etc/ssl/drogon && \
    openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
      -keyout /etc/ssl/drogon/server.key \
      -out /etc/ssl/drogon/server.crt \
      -subj "/C=CN/ST=Guangdong/L=Guangzhou/O=test/OU=test/CN=localhost" && \
    chmod 600 /etc/ssl/drogon/server.key && \
    apt clean && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt main.cc ./
COPY controllers/ ./controllers/
COPY filters/ ./filters/
COPY models/ ./models/
COPY utils/ ./utils/
COPY test/ ./test/
COPY config.json ./

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && cp YannickWXserver /app/server && rm -rf build

# Stage 2: Runtime
FROM ubuntu:22.04
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    apt update && \
    apt install -y --no-install-recommends \
        libssl3 ca-certificates \
        libhiredis0.14 \
        libcrypt1 \
        libmariadb3 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/server /app/server
COPY --from=builder /etc/ssl/drogon/ /etc/ssl/drogon/
COPY config.json /app/config.json

WORKDIR /app
EXPOSE 8080 443
CMD ["./server"]
