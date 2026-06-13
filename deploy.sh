#!/bin/bash
set -e

# ============================================
# YannickWXserver - 一键部署脚本
# 用法: bash deploy.sh <命令>
#    deploy  首次部署
#    rebuild 重新编译并重启
#    stop    停止服务
#    logs    查看日志
# ============================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 检查命令是否存在
check_cmd() {
    if ! command -v "$1" &> /dev/null; then
        error "$1 未安装，请先安装: $2"
        exit 1
    fi
}

# 生成随机 JWT 密钥
generate_jwt_secret() {
    openssl rand -hex 64
}

deploy() {
    info "===== 开始部署 YannickWXserver ====="

    check_cmd docker "curl -fsSL https://get.docker.com | bash"
    if ! docker compose version &>/dev/null; then
        error "Docker Compose v2 未安装，请先安装: apt install -y docker-compose-plugin"
        exit 1
    fi

    # 创建 SQL 初始化目录
    mkdir -p sql

    # 如果不存在 init.sql，生成表结构
    if [ ! -f sql/init.sql ]; then
        info "生成数据库初始化脚本..."
        cat > sql/init.sql << 'SQLEOF'
CREATE TABLE IF NOT EXISTS `user` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `username` VARCHAR(64) NOT NULL,
    `phone` VARCHAR(20) DEFAULT NULL,
    `email` VARCHAR(100) DEFAULT NULL,
    `password_hash` VARCHAR(255) NOT NULL,
    `nickname` VARCHAR(100) NOT NULL,
    `avatar_url` VARCHAR(255) DEFAULT NULL,
    `cover_url` VARCHAR(255) DEFAULT NULL,
    `gender` TINYINT DEFAULT NULL,
    `birth_date` DATE DEFAULT NULL,
    `region` VARCHAR(100) DEFAULT NULL,
    `signature` VARCHAR(255) DEFAULT NULL,
    `status` TINYINT DEFAULT NULL,
    `last_login_time` DATETIME DEFAULT NULL,
    `last_login_ip` VARCHAR(45) DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_username` (`username`),
    UNIQUE KEY `uk_phone` (`phone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `friend` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `user_id` BIGINT NOT NULL,
    `friend_id` BIGINT NOT NULL,
    `remark` VARCHAR(100) DEFAULT NULL,
    `tags` JSON DEFAULT NULL,
    `status` TINYINT NOT NULL DEFAULT 0,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_user_friend` (`user_id`, `friend_id`),
    KEY `idx_user_id` (`user_id`),
    KEY `idx_friend_id` (`friend_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `friend_request` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `from_user_id` BIGINT NOT NULL,
    `to_user_id` BIGINT NOT NULL,
    `status` TINYINT NOT NULL DEFAULT 0 COMMENT '0=pending,1=accepted,2=rejected',
    `message` VARCHAR(500) DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_to_user_id` (`to_user_id`),
    KEY `idx_from_user_id` (`from_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `group` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `group_name` VARCHAR(100) NOT NULL,
    `avatar` VARCHAR(255) DEFAULT NULL,
    `announcement` VARCHAR(500) DEFAULT NULL,
    `introduction` VARCHAR(500) DEFAULT NULL,
    `max_members` INT NOT NULL DEFAULT 200,
    `owner_id` BIGINT NOT NULL,
    `status` TINYINT NOT NULL DEFAULT 1,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_owner_id` (`owner_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `group_member` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `group_id` BIGINT NOT NULL,
    `user_id` BIGINT NOT NULL,
    `nickname` VARCHAR(100) DEFAULT NULL,
    `role` TINYINT NOT NULL DEFAULT 0 COMMENT '0=member,1=admin,2=owner',
    `status` TINYINT NOT NULL DEFAULT 1,
    `join_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_group_user` (`group_id`, `user_id`),
    KEY `idx_group_id` (`group_id`),
    KEY `idx_user_id` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `device` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `user_id` BIGINT NOT NULL,
    `device_name` VARCHAR(100) DEFAULT NULL,
    `device_type` VARCHAR(50) DEFAULT NULL,
    `push_token` VARCHAR(255) DEFAULT NULL,
    `last_login_time` DATETIME DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_user_id` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `offline_message` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `from_user_id` BIGINT NOT NULL,
    `to_user_id` BIGINT NOT NULL,
    `chat_type` TINYINT NOT NULL DEFAULT 0 COMMENT '0=single,1=group',
    `target_id` BIGINT NOT NULL,
    `msg_type` INT NOT NULL DEFAULT 0,
    `content` TEXT,
    `media_url` VARCHAR(500) DEFAULT NULL,
    `media_thumb_url` VARCHAR(500) DEFAULT NULL,
    `media_size` BIGINT DEFAULT NULL,
    `media_duration` INT DEFAULT NULL,
    `media_format` VARCHAR(20) DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `expire_time` DATETIME NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_to_user_id` (`to_user_id`),
    KEY `idx_created_at` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `moment` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `user_id` BIGINT NOT NULL,
    `content` TEXT,
    `images` JSON DEFAULT NULL,
    `video_url` VARCHAR(500) DEFAULT NULL,
    `privacy_type` TINYINT NOT NULL DEFAULT 0,
    `is_deleted` TINYINT NOT NULL DEFAULT 0,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_user_id` (`user_id`),
    KEY `idx_created_at` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `moment_like` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `moment_id` BIGINT NOT NULL,
    `user_id` BIGINT NOT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_moment_user` (`moment_id`, `user_id`),
    KEY `idx_moment_id` (`moment_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `moment_comment` (
    `id` BIGINT NOT NULL AUTO_INCREMENT,
    `moment_id` BIGINT NOT NULL,
    `user_id` BIGINT NOT NULL,
    `reply_user_id` BIGINT DEFAULT NULL,
    `content` TEXT NOT NULL,
    `is_deleted` TINYINT NOT NULL DEFAULT 0,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_moment_id` (`moment_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
SQLEOF
        info "SQL 初始化脚本已生成"
    fi

    # 检查 config.json 中的 JWT 密钥
    if grep -q '"jwt_secret": ""' config.json 2>/dev/null || ! grep -q 'jwt_secret' config.json 2>/dev/null; then
        warn "JWT 密钥为空，自动生成..."
        NEW_SECRET=$(generate_jwt_secret)
        # 更新config.json
        sed -i "s/\"jwt_secret\": \"\"/\"jwt_secret\": \"$NEW_SECRET\"/" config.json
        info "JWT 密钥已生成"
    fi

    # 检查并修改 config.json 的数据库和 Redis 地址
    info "配置 Docker 网络地址..."
    sed -i 's/"host": "192.168.232.91"/"host": "mysql"/' config.json
    sed -i 's/"host": "127.0.0.1"/"host": "redis"/' config.json

    # 修改 number_of_threads 为 0 (自动检测CPU核数)
    sed -i 's/"number_of_threads": 1/"number_of_threads": 0/' config.json

    # 修改上传目录为 Docker volume 路径
    sed -i 's|"upload_dir": "./uploads/files/"|"upload_dir": "/app/uploads/files/"|' config.json

    info "启动 Docker 服务..."
    docker compose up -d --build

    info "===== 部署完成 ====="
    info "服务器: http://$(curl -s --connect-timeout 3 ip.sb 2>/dev/null || echo "120.27.144.19"):8080"
    info "查看日志: docker compose logs -f server"
}

rebuild() {
    info "重新编译并重启..."
    docker compose build server
    docker compose up -d server
    info "完成"
}

stop() {
    info "停止服务..."
    docker compose down
    info "已停止"
}

logs() {
    docker compose logs -f "$@"
}

case "${1:-deploy}" in
    deploy)
        deploy
        ;;
    rebuild)
        rebuild
        ;;
    stop)
        stop
        ;;
    logs)
        shift
        logs "$@"
        ;;
    *)
        echo "用法: bash deploy.sh <命令>"
        echo "  deploy  首次部署"
        echo "  rebuild 重新编译并重启"
        echo "  stop    停止服务"
        echo "  logs    查看日志"
        exit 1
        ;;
esac
