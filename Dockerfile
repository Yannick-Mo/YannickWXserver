# 基于官方 Drogon 镜像
FROM drogonframework/drogon:latest

# 安装编译依赖 + 从源码编译安装 jwt-cpp + 生成自签名SSL证书 + 清理冗余文件
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    apt update && \
    # 安装编译所需的基础工具和依赖（新增 libpq5 libpq-dev 解决PostgreSQL依赖）
    apt install -y --no-install-recommends git cmake build-essential libssl-dev openssl libpq5 libpq-dev && \
    # 克隆 jwt-cpp 源码（指定版本更稳定）
    git clone --depth 1 --branch v0.7.0 https://github.com/Thalhammer/jwt-cpp.git /tmp/jwt-cpp && \
    # 编译安装 jwt-cpp
    cd /tmp/jwt-cpp && mkdir build && cd build && \
    cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) && make install && \
    # ========== 生成自签名SSL证书 ==========
    # 创建证书存放目录（固定路径，方便代码引用）
    mkdir -p /etc/ssl/drogon && \
    # 非交互式生成自签名证书（无需手动输入国家/省份等信息）
    openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
      -keyout /etc/ssl/drogon/server.key \
      -out /etc/ssl/drogon/server.crt \
      -subj "/C=CN/ST=Guangdong/L=Guangzhou/O=test/OU=test/CN=localhost" && \
    # 设置证书权限（安全最佳实践：私钥仅root可读）
    chmod 600 /etc/ssl/drogon/server.key && \
    chown root:root /etc/ssl/drogon/* && \
    # ========== 清理冗余文件 ==========
    rm -rf /tmp/jwt-cpp && \
    # 卸载编译/生成证书的依赖（保留运行时依赖 libssl-dev + libpq5）
    # 注意：只删除 libpq-dev（编译依赖），保留 libpq5（运行依赖）
    apt remove -y git cmake build-essential openssl libpq-dev && \
    apt autoremove -y && \
    apt clean && rm -rf /var/lib/apt/lists/*

# 暴露SSL服务端口（比如8848），方便使用者映射
EXPOSE 8848