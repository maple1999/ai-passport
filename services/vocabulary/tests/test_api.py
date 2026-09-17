from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fastapi.testclient import TestClient

from app.main import Settings, create_app


class VocabularyApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        settings = Settings(
            database=str(Path(self.temp.name) / "test.sqlite3"),
            pairing_code="pair-1234",
            admin_token="admin-secret",
        )
        self.client_context = TestClient(create_app(settings))
        self.client = self.client_context.__enter__()
        response = self.client.post(
            "/v1/devices/activate",
            json={"pairing_code": "pair-1234", "device_name": "test"},
        )
        self.assertEqual(response.status_code, 200)
        self.token = response.json()["device_token"]
        self.headers = {"Authorization": f"Bearer {self.token}"}

    def tearDown(self) -> None:
        self.client_context.__exit__(None, None, None)
        self.temp.cleanup()

    def test_activation_rejects_bad_code(self) -> None:
        response = self.client.post(
            "/v1/devices/activate",
            json={"pairing_code": "bad-code", "device_name": "other"},
        )
        self.assertEqual(response.status_code, 403)

    def test_session_result_is_idempotent_and_correctable(self) -> None:
        session = self.client.post(
            "/v1/study/sessions", json={"batch_size": 2}, headers=self.headers
        ).json()
        self.assertEqual(len(session["cards"]), 2)
        card = session["cards"][0]
        path = f"/v1/study/sessions/{session['session_id']}/cards/{card['id']}/result"
        payload = {"result": "known", "idempotency_key": "event-known-0001"}
        first = self.client.put(path, json=payload, headers=self.headers)
        duplicate = self.client.put(path, json=payload, headers=self.headers)
        self.assertFalse(first.json()["duplicate"])
        self.assertTrue(duplicate.json()["duplicate"])

        correction = self.client.put(
            path,
            json={"result": "again", "idempotency_key": "event-again-0001"},
            headers=self.headers,
        )
        self.assertEqual(correction.status_code, 200)
        progress = self.client.get(
            "/v1/admin/progress",
            headers={"Authorization": "Bearer admin-secret"},
        ).json()
        self.assertEqual(progress["learned"], 1)
        self.assertEqual(progress["again"], 1)
        self.assertEqual(progress["lapses"], 1)

    def test_device_endpoints_require_token(self) -> None:
        self.assertEqual(self.client.get("/v1/device/config").status_code, 401)


if __name__ == "__main__":
    unittest.main()
