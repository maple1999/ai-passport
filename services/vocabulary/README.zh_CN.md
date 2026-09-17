<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 背单词服务

本服务为 AI Passport 雅思背单词功能提供有大小限制的 API 与进度页面。单用户
MVP 使用 SQLite，并附带一组由项目自行编写的小型学术词汇起步数据；它不是完整、
已授权的雅思词表。

将 `.env.example` 复制为 `.env`，替换其中全部配置，然后运行
`docker compose up -d --build`。Compose 默认只绑定到
`127.0.0.1:18081`；检查路由和端口不会与其他服务冲突后，再通过已有 HTTPS
反向代理公开。
`nginx-location.conf` 是供现有 Nginx server 使用的可选 location 片段，要求
Nginx 已定义 `mapletech_req` 和 `mapletech_conn` 限流区。重新加载前必须测试完整配置。

设备 API 支持激活、有数量上限的学习会话、幂等结果提交、结果修正和会话结束。
进度页面位于 `/`，页面只在当前请求中使用管理令牌，不持久化保存。
