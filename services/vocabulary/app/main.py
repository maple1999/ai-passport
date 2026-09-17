from __future__ import annotations

import hashlib
import hmac
import os
import secrets
import sqlite3
import uuid
from contextlib import asynccontextmanager, contextmanager
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, status
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field


STARTER_WORDS = (
    ("adapt", "适应；改编", "动词", "/əˈdæpt/"),
    ("allocate", "分配；划拨", "动词", "/ˈæləkeɪt/"),
    ("coherent", "连贯的；一致的", "形容词", "/kəʊˈhɪərənt/"),
    ("derive", "获得；源自", "动词", "/dɪˈraɪv/"),
    ("diminish", "减少；削弱", "动词", "/dɪˈmɪnɪʃ/"),
    ("empirical", "以实验或观察为依据的", "形容词", "/ɪmˈpɪrɪkəl/"),
    ("facilitate", "促进；使便利", "动词", "/fəˈsɪlɪteɪt/"),
    ("fluctuate", "波动；起伏", "动词", "/ˈflʌktʃueɪt/"),
    ("inhibit", "抑制；阻碍", "动词", "/ɪnˈhɪbɪt/"),
    ("inevitable", "不可避免的", "形容词", "/ɪnˈevɪtəbəl/"),
    ("justify", "证明……合理；为……辩护", "动词", "/ˈdʒʌstɪfaɪ/"),
    ("notion", "概念；看法", "名词", "/ˈnəʊʃən/"),
    ("prevalent", "普遍的；盛行的", "形容词", "/ˈprevələnt/"),
    ("profound", "深刻的；影响深远的", "形容词", "/prəˈfaʊnd/"),
    ("reinforce", "加强；强化", "动词", "/ˌriːɪnˈfɔːs/"),
    ("subsequent", "随后的；后来的", "形容词", "/ˈsʌbsɪkwənt/"),
    ("sustainable", "可持续的", "形容词", "/səˈsteɪnəbəl/"),
    ("tentative", "暂定的；试探性的", "形容词", "/ˈtentətɪv/"),
    ("underlying", "根本的；潜在的", "形容词", "/ˌʌndəˈlaɪɪŋ/"),
    ("valid", "有效的；有根据的", "形容词", "/ˈvælɪd/"),
)


@dataclass(frozen=True)
class Settings:
    database: str
    pairing_code: str
    admin_token: str

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(
            database=os.getenv("VOCAB_DATABASE", "/data/vocabulary.sqlite3"),
            pairing_code=os.getenv("VOCAB_PAIRING_CODE", ""),
            admin_token=os.getenv("VOCAB_ADMIN_TOKEN", ""),
        )


class ActivationRequest(BaseModel):
    pairing_code: str = Field(min_length=4, max_length=64)
    device_name: str = Field(default="AI Passport", min_length=1, max_length=64)


class SessionRequest(BaseModel):
    batch_size: int = Field(default=10, ge=1, le=10)


class ResultRequest(BaseModel):
    result: str = Field(pattern="^(known|again)$")
    idempotency_key: str = Field(min_length=8, max_length=80)


def utc_now() -> datetime:
    return datetime.now(UTC).replace(microsecond=0)


def iso(value: datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def connect(database: str) -> sqlite3.Connection:
    db = sqlite3.connect(database, timeout=5)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys = ON")
    db.execute("PRAGMA journal_mode = WAL")
    return db


@contextmanager
def database_connection(database: str):
    db = connect(database)
    try:
        with db:
            yield db
    finally:
        db.close()


def initialize(database: str) -> None:
    path = Path(database)
    path.parent.mkdir(parents=True, exist_ok=True)
    with database_connection(database) as db:
        db.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                token_hash TEXT NOT NULL UNIQUE,
                created_at TEXT NOT NULL,
                last_seen_at TEXT NOT NULL,
                revoked_at TEXT
            );
            CREATE TABLE IF NOT EXISTS words (
                id TEXT PRIMARY KEY,
                word TEXT NOT NULL UNIQUE,
                meaning TEXT NOT NULL,
                part_of_speech TEXT NOT NULL,
                phonetic TEXT NOT NULL,
                position INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS progress (
                device_id TEXT NOT NULL REFERENCES devices(id),
                word_id TEXT NOT NULL REFERENCES words(id),
                due_at TEXT NOT NULL,
                interval_days INTEGER NOT NULL DEFAULT 0,
                known_streak INTEGER NOT NULL DEFAULT 0,
                lapse_count INTEGER NOT NULL DEFAULT 0,
                last_result TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                PRIMARY KEY (device_id, word_id)
            );
            CREATE TABLE IF NOT EXISTS study_sessions (
                id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL REFERENCES devices(id),
                started_at TEXT NOT NULL,
                finished_at TEXT
            );
            CREATE TABLE IF NOT EXISTS session_cards (
                id TEXT PRIMARY KEY,
                session_id TEXT NOT NULL REFERENCES study_sessions(id),
                word_id TEXT NOT NULL REFERENCES words(id),
                position INTEGER NOT NULL,
                result TEXT,
                previous_due_at TEXT,
                previous_interval_days INTEGER NOT NULL DEFAULT 0,
                previous_known_streak INTEGER NOT NULL DEFAULT 0,
                previous_lapse_count INTEGER NOT NULL DEFAULT 0,
                UNIQUE(session_id, position)
            );
            CREATE TABLE IF NOT EXISTS review_events (
                idempotency_key TEXT PRIMARY KEY,
                device_id TEXT NOT NULL REFERENCES devices(id),
                session_card_id TEXT NOT NULL REFERENCES session_cards(id),
                result TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            """
        )
        for position, (word, meaning, part_of_speech, phonetic) in enumerate(
            STARTER_WORDS, start=1
        ):
            db.execute(
                """INSERT OR IGNORE INTO words
                   (id, word, meaning, part_of_speech, phonetic, position)
                   VALUES (?, ?, ?, ?, ?, ?)""",
                (f"starter-{position:03d}", word, meaning, part_of_speech, phonetic, position),
            )


def bearer(authorization: str | None) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing bearer token")
    token = authorization[7:].strip()
    if not token:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing bearer token")
    return token


def create_app(settings: Settings | None = None) -> FastAPI:
    config = settings or Settings.from_env()

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        if not config.pairing_code or not config.admin_token:
            raise RuntimeError("VOCAB_PAIRING_CODE and VOCAB_ADMIN_TOKEN are required")
        initialize(config.database)
        yield

    app = FastAPI(title="AI Passport Vocabulary", version="1.0.0", lifespan=lifespan)

    def current_device(authorization: str | None = Header(default=None)) -> sqlite3.Row:
        digest = token_hash(bearer(authorization))
        with database_connection(config.database) as db:
            row = db.execute(
                "SELECT * FROM devices WHERE token_hash = ? AND revoked_at IS NULL", (digest,)
            ).fetchone()
            if not row:
                raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid device token")
            db.execute("UPDATE devices SET last_seen_at = ? WHERE id = ?", (iso(utc_now()), row["id"]))
            return row

    def require_admin(authorization: str | None = Header(default=None)) -> None:
        supplied = bearer(authorization)
        if not hmac.compare_digest(supplied, config.admin_token):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid admin token")

    @app.get("/health")
    def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.post("/v1/devices/activate")
    def activate(request: ActivationRequest) -> dict[str, str]:
        if not hmac.compare_digest(request.pairing_code, config.pairing_code):
            raise HTTPException(status.HTTP_403_FORBIDDEN, "invalid pairing code")
        raw_token = secrets.token_urlsafe(32)
        device_id = str(uuid.uuid4())
        now = iso(utc_now())
        with database_connection(config.database) as db:
            db.execute(
                "INSERT INTO devices VALUES (?, ?, ?, ?, ?, NULL)",
                (device_id, request.device_name, token_hash(raw_token), now, now),
            )
        return {"device_id": device_id, "device_token": raw_token}

    @app.get("/v1/device/config")
    def device_config(device: sqlite3.Row = Depends(current_device)) -> dict[str, object]:
        return {
            "device_id": device["id"],
            "word_list": "starter-academic-v1",
            "batch_size": 10,
            "auto_pronunciation": False,
            "audio_available": False,
        }

    @app.post("/v1/study/sessions")
    def create_session(
        request: SessionRequest, device: sqlite3.Row = Depends(current_device)
    ) -> dict[str, object]:
        now = iso(utc_now())
        session_id = str(uuid.uuid4())
        with database_connection(config.database) as db:
            words = db.execute(
                """
                SELECT w.*, p.due_at, p.interval_days, p.known_streak, p.lapse_count
                FROM words w
                LEFT JOIN progress p ON p.word_id = w.id AND p.device_id = ?
                WHERE p.word_id IS NULL OR p.due_at <= ?
                ORDER BY CASE WHEN p.word_id IS NULL THEN 1 ELSE 0 END,
                         COALESCE(p.due_at, ''), w.position
                LIMIT ?
                """,
                (device["id"], now, request.batch_size),
            ).fetchall()
            db.execute(
                "INSERT INTO study_sessions VALUES (?, ?, ?, NULL)",
                (session_id, device["id"], now),
            )
            cards: list[dict[str, object]] = []
            for position, word in enumerate(words):
                card_id = str(uuid.uuid4())
                db.execute(
                    """INSERT INTO session_cards
                       (id, session_id, word_id, position, result, previous_due_at,
                        previous_interval_days, previous_known_streak, previous_lapse_count)
                       VALUES (?, ?, ?, ?, NULL, ?, ?, ?, ?)""",
                    (
                        card_id,
                        session_id,
                        word["id"],
                        position,
                        word["due_at"],
                        word["interval_days"] or 0,
                        word["known_streak"] or 0,
                        word["lapse_count"] or 0,
                    ),
                )
                cards.append(
                    {
                        "id": card_id,
                        "word": word["word"],
                        "meaning": word["meaning"],
                        "part_of_speech": word["part_of_speech"],
                        "phonetic": word["phonetic"],
                        "position": position,
                    }
                )
        return {"session_id": session_id, "cards": cards, "complete": not cards}

    @app.put("/v1/study/sessions/{session_id}/cards/{card_id}/result")
    def submit_result(
        session_id: str,
        card_id: str,
        request: ResultRequest,
        device: sqlite3.Row = Depends(current_device),
    ) -> dict[str, object]:
        now_dt = utc_now()
        now = iso(now_dt)
        with database_connection(config.database) as db:
            existing = db.execute(
                "SELECT session_card_id, result FROM review_events WHERE idempotency_key = ?",
                (request.idempotency_key,),
            ).fetchone()
            if existing:
                if existing["session_card_id"] != card_id or existing["result"] != request.result:
                    raise HTTPException(status.HTTP_409_CONFLICT, "idempotency key conflict")
                return {"accepted": True, "duplicate": True}

            card = db.execute(
                """
                SELECT sc.*, s.device_id FROM session_cards sc
                JOIN study_sessions s ON s.id = sc.session_id
                WHERE sc.id = ? AND sc.session_id = ?
                """,
                (card_id, session_id),
            ).fetchone()
            if not card or card["device_id"] != device["id"]:
                raise HTTPException(status.HTTP_404_NOT_FOUND, "session card not found")

            previous_interval = card["previous_interval_days"]
            previous_streak = card["previous_known_streak"]
            previous_lapses = card["previous_lapse_count"]
            if request.result == "known":
                interval_days = max(1, previous_interval * 2 if previous_interval else 1)
                streak = previous_streak + 1
                lapses = previous_lapses
                due_at = iso(now_dt + timedelta(days=interval_days))
            else:
                interval_days = 0
                streak = 0
                lapses = previous_lapses + 1
                due_at = iso(now_dt + timedelta(minutes=10))

            db.execute(
                "INSERT INTO review_events VALUES (?, ?, ?, ?, ?)",
                (request.idempotency_key, device["id"], card_id, request.result, now),
            )
            db.execute("UPDATE session_cards SET result = ? WHERE id = ?", (request.result, card_id))
            db.execute(
                """
                INSERT INTO progress
                    (device_id, word_id, due_at, interval_days, known_streak,
                     lapse_count, last_result, updated_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id, word_id) DO UPDATE SET
                    due_at=excluded.due_at,
                    interval_days=excluded.interval_days,
                    known_streak=excluded.known_streak,
                    lapse_count=excluded.lapse_count,
                    last_result=excluded.last_result,
                    updated_at=excluded.updated_at
                """,
                (
                    device["id"], card["word_id"], due_at, interval_days,
                    streak, lapses, request.result, now,
                ),
            )
        return {"accepted": True, "duplicate": False, "next_due_at": due_at}

    @app.post("/v1/study/sessions/{session_id}/finish")
    def finish_session(
        session_id: str, device: sqlite3.Row = Depends(current_device)
    ) -> dict[str, int | bool]:
        with database_connection(config.database) as db:
            session = db.execute(
                "SELECT * FROM study_sessions WHERE id = ? AND device_id = ?",
                (session_id, device["id"]),
            ).fetchone()
            if not session:
                raise HTTPException(status.HTTP_404_NOT_FOUND, "session not found")
            db.execute(
                "UPDATE study_sessions SET finished_at = COALESCE(finished_at, ?) WHERE id = ?",
                (iso(utc_now()), session_id),
            )
            counts = db.execute(
                """SELECT COUNT(*) total,
                          SUM(CASE WHEN result='known' THEN 1 ELSE 0 END) known,
                          SUM(CASE WHEN result='again' THEN 1 ELSE 0 END) again
                   FROM session_cards WHERE session_id = ?""",
                (session_id,),
            ).fetchone()
        return {
            "finished": True,
            "total": counts["total"] or 0,
            "known": counts["known"] or 0,
            "again": counts["again"] or 0,
        }

    @app.get("/v1/admin/progress", dependencies=[Depends(require_admin)])
    def admin_progress() -> dict[str, object]:
        with database_connection(config.database) as db:
            totals = db.execute(
                """SELECT COUNT(*) learned,
                          SUM(CASE WHEN last_result='known' THEN 1 ELSE 0 END) known,
                          SUM(CASE WHEN last_result='again' THEN 1 ELSE 0 END) again,
                          SUM(lapse_count) lapses
                   FROM progress"""
            ).fetchone()
            recent = db.execute(
                """SELECT w.word, w.meaning, p.last_result, p.due_at,
                          p.known_streak, p.lapse_count
                   FROM progress p JOIN words w ON w.id=p.word_id
                   ORDER BY p.updated_at DESC LIMIT 20"""
            ).fetchall()
        return {
            "learned": totals["learned"] or 0,
            "known": totals["known"] or 0,
            "again": totals["again"] or 0,
            "lapses": totals["lapses"] or 0,
            "recent": [dict(row) for row in recent],
        }

    @app.get("/", response_class=HTMLResponse)
    def dashboard() -> str:
        return DASHBOARD_HTML

    return app


DASHBOARD_HTML = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Passport IELTS</title><style>
body{font-family:system-ui,sans-serif;max-width:760px;margin:auto;padding:24px;background:#eef7ff;color:#14213d}
.card{background:white;border:3px solid #14213d;box-shadow:5px 5px #14213d;padding:18px;margin:18px 0}
input,button{font:inherit;padding:10px;border:2px solid #14213d}button{background:#ffd166;cursor:pointer}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}.stats div{background:#d8f3dc;padding:12px;text-align:center}
table{width:100%;border-collapse:collapse}td,th{padding:8px;border-bottom:1px solid #ccd}small{color:#526}
@media(max-width:520px){.stats{grid-template-columns:repeat(2,1fr)}table{font-size:13px}}
</style></head><body><h1>IELTS 学习进度</h1>
<div class="card"><label>管理令牌 <input id="token" type="password" autocomplete="current-password"></label>
<button onclick="loadProgress()">查看进度</button><small id="status"></small></div>
<div id="content"></div><script>
async function loadProgress(){const t=document.querySelector('#token').value;const s=document.querySelector('#status');
s.textContent=' 加载中…';const r=await fetch('v1/admin/progress',{headers:{Authorization:'Bearer '+t}});
if(!r.ok){s.textContent=' 验证失败';return}const d=await r.json();s.textContent='';
const rows=d.recent.map(x=>`<tr><td>${x.word}</td><td>${x.meaning}</td><td>${x.last_result}</td><td>${x.known_streak}</td><td>${x.lapse_count}</td></tr>`).join('');
document.querySelector('#content').innerHTML=`<div class="card stats"><div>已学习<br><b>${d.learned}</b></div><div>最近认识<br><b>${d.known}</b></div><div>待巩固<br><b>${d.again}</b></div><div>遗忘次数<br><b>${d.lapses}</b></div></div><div class="card"><table><thead><tr><th>单词</th><th>释义</th><th>结果</th><th>连续</th><th>遗忘</th></tr></thead><tbody>${rows}</tbody></table></div>`}
</script></body></html>"""


app = create_app()
