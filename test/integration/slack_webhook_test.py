#!/usr/bin/env python3

import csv
import http.server
import io
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
DUCKDB = ROOT / "build/release/duckdb"
EXTENSION = ROOT / "build/release/extension/slack/slack.duckdb_extension"
FAKE_WEBHOOK = "https://hooks.slack.com/services/T000/B000/test"


class MockSlackHandler(http.server.BaseHTTPRequestHandler):
    requests = []
    lock = threading.Lock()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(length)
        with self.lock:
            self.requests.append((self.path, self.headers.get("Content-Type"), raw_body))

        message = json.loads(raw_body)["text"]
        if message == "timeout":
            time.sleep(16)
        if message == "disconnect":
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return

        if message == "rate-limited":
            self.send_response(429)
            self.send_header("Retry-After", "4")
            body = b"rate_limited"
        elif message == "api-error":
            self.send_response(403)
            body = b"invalid_auth"
        elif message == "malformed":
            self.send_response(200)
            body = b"not-json"
        else:
            self.send_response(200)
            body = b"ok"

        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, _format, *_args):
        pass


def run_sql(sql, endpoint=None, timeout=30):
    env = os.environ.copy()
    if endpoint:
        env["DUCKDB_SLACK_TEST_API_URL"] = endpoint
    else:
        env.pop("DUCKDB_SLACK_TEST_API_URL", None)
    return subprocess.run(
        [str(DUCKDB), "-no-agent", "-unsigned", "-csv", "-c", sql],
        capture_output=True,
        text=True,
        env=env,
        timeout=timeout,
        check=False,
    )


def result_row(stdout):
    rows = list(csv.reader(io.StringIO(stdout)))
    for index, row in enumerate(rows[:-1]):
        if row == ["success", "channel", "message_ts", "message"]:
            return dict(zip(row, rows[index + 1]))
    raise AssertionError(f"Could not find slack_post result row in output:\n{stdout}")


def call_sql(message, named_secret=False):
    escaped_message = message.replace("'", "''")
    secret_argument = "secret_name => 'slack_test'" if named_secret else "'slack_test'"
    return (
        f"LOAD '{EXTENSION}'; "
        f"CREATE TEMP SECRET slack_test (TYPE slack, WEBHOOK_URL '{FAKE_WEBHOOK}'); "
        f"CALL slack_post({secret_argument}, msg => '{escaped_message}');"
    )


def run_post(endpoint, message, expected_success, expected_text, named_secret=False):
    before = len(MockSlackHandler.requests)
    result = run_sql(
        call_sql(message, named_secret=named_secret),
        endpoint,
        timeout=25 if message == "timeout" else 10,
    )
    if result.returncode:
        raise AssertionError(f"DuckDB failed:\n{result.stderr}\n{result.stdout}")
    row = result_row(result.stdout)
    assert row["success"] == str(expected_success).lower(), row
    assert expected_text in row["message"], row
    assert row["channel"] == "" and row["message_ts"] == "", row
    assert FAKE_WEBHOOK not in result.stdout + result.stderr

    after = len(MockSlackHandler.requests)
    assert after - before == 1, (message, before, after)
    path, content_type, body = MockSlackHandler.requests[-1]
    assert path == "/mock-post", path
    assert content_type == "application/json; charset=utf-8", content_type
    assert json.loads(body) == {"text": message}, body


def run_table_post(endpoint, color):
    table = "```\nMetric     | Count\n-----------+------\nProcessed  |    42\nFailed     |     0\n```"
    sql = (
        f"LOAD '{EXTENSION}'; "
        f"CREATE TEMP SECRET slack_test (TYPE slack, WEBHOOK_URL '{FAKE_WEBHOOK}'); "
        "SET VARIABLE report = (SELECT concat('```', chr(10), "
        "'Metric     | Count', chr(10), '-----------+------', chr(10), "
        "string_agg(printf('%-10s | %5d', metric, count), chr(10) ORDER BY idx), "
        "chr(10), '```') "
        "FROM (VALUES (1, 'Processed', 42), (2, 'Failed', 0)) AS data(idx, metric, count)); "
        f"CALL slack_post(secret_name => 'slack_test', msg => getvariable('report'), color => '{color}');"
    )
    before = len(MockSlackHandler.requests)
    result = run_sql(sql, endpoint)
    if result.returncode:
        raise AssertionError(f"DuckDB failed:\n{result.stderr}\n{result.stdout}")
    assert result_row(result.stdout)["success"] == "true", result.stdout
    assert len(MockSlackHandler.requests) == before + 1
    path, content_type, body = MockSlackHandler.requests[-1]
    assert path == "/mock-post"
    assert content_type == "application/json; charset=utf-8"
    assert json.loads(body) == {
        "text": table,
        "attachments": [
            {
                "color": color,
                "fallback": table,
                "blocks": [{"type": "section", "text": {"type": "mrkdwn", "text": table}}],
            }
        ],
    }, body


def run_persistent_secret_tests():
    with tempfile.TemporaryDirectory(prefix="duckdb-slack-secrets-") as directory:
        quoted_directory = directory.replace("'", "''")
        create = run_sql(
            f"LOAD '{EXTENSION}'; SET secret_directory='{quoted_directory}'; "
            f"CREATE PERSISTENT SECRET slack_persistent "
            f"(TYPE slack, WEBHOOK_URL '{FAKE_WEBHOOK}');"
        )
        if create.returncode:
            raise AssertionError(f"Persistent secret creation failed:\n{create.stderr}")

        read = run_sql(
            f"LOAD '{EXTENSION}'; SET secret_directory='{quoted_directory}'; "
            "SELECT persistent FROM duckdb_secrets() WHERE name='slack_persistent';"
        )
        if read.returncode or "true" not in read.stdout.lower():
            raise AssertionError(f"Persistent secret reload failed:\n{read.stderr}\n{read.stdout}")

        drop = run_sql(
            f"LOAD '{EXTENSION}'; SET secret_directory='{quoted_directory}'; "
            "DROP PERSISTENT SECRET slack_persistent;"
        )
        if drop.returncode:
            raise AssertionError(f"Persistent secret cleanup failed:\n{drop.stderr}")


def run_test_override_safety(endpoint):
    real_like_secret = (
        f"LOAD '{EXTENSION}'; "
        "CREATE TEMP SECRET slack_test (TYPE slack, "
        "WEBHOOK_URL 'https://hooks.slack.com/services/T111/B222/not-the-test-placeholder'); "
        "CALL slack_post('slack_test', msg => 'must not be redirected');"
    )
    before = len(MockSlackHandler.requests)
    result = run_sql(real_like_secret, endpoint)
    assert result.returncode != 0, result.stdout
    assert "only permitted with the integration test placeholder" in result.stderr, result.stderr
    assert len(MockSlackHandler.requests) == before

    non_loopback = run_sql(call_sql("must not be redirected"), "http://example.com/mock-post")
    assert non_loopback.returncode != 0, non_loopback.stdout
    assert "loopback interface" in non_loopback.stderr, non_loopback.stderr
    assert len(MockSlackHandler.requests) == before


def main():
    if not DUCKDB.exists() or not EXTENSION.exists():
        raise SystemExit("Build the extension first with `make`")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockSlackHandler)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}/mock-post"
    try:
        run_post(
            endpoint,
            'escaped "quote" and newline\n',
            True,
            "accepted",
            named_secret=True,
        )
        run_post(endpoint, "api-error", False, "HTTP 403")
        run_post(endpoint, "rate-limited", False, "retry after 4 seconds")
        run_post(endpoint, "malformed", False, "invalid webhook response")
        run_post(endpoint, "disconnect", False, "request failed")
        run_post(endpoint, "timeout", False, "request failed")
        run_table_post(endpoint, "good")
        run_table_post(endpoint, "danger")
        run_test_override_safety(endpoint)
        run_persistent_secret_tests()
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=3)

    print("Slack webhook integration tests passed")


if __name__ == "__main__":
    main()
