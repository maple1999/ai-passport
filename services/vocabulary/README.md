<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Vocabulary service

This service is the bounded API and progress dashboard for the AI Passport
IELTS vocabulary trainer. It uses SQLite for the single-user MVP and ships a
small project-authored academic starter list. It does not claim to be a full
licensed IELTS word list.

Copy `.env.example` to `.env`, replace every value, and start the service with
`docker compose up -d --build`. The compose file binds only to
`127.0.0.1:18081`; expose it through an existing HTTPS reverse proxy after
checking that the route and port do not conflict with other services.
`nginx-location.conf` is an optional location snippet for an existing Nginx
server that already defines the `mapletech_req` and `mapletech_conn` zones.
Test the full Nginx configuration before reloading it.

The device API supports activation, bounded study sessions, idempotent result
updates, result corrections, and session completion. The dashboard is served
at `/` and accepts the administrator token in the page without persisting it.
