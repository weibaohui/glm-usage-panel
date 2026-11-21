#!/usr/bin/env python3
"""mock_server.py — a stand-in for the BigModel usage API, faithful to the
interface contract documented in the Youdao E6 / e-ink / Kindle versions:
  GET /api/monitor/usage/quota/limit?type=2                     (API JWT + org/project headers)
  GET /api/biz/customer-package-reset/list?targetType=TEAM
  GET /api/biz/account/query-org-owner-account-report
  GET /api/biz/account/query-customer-account-report
  GET /api/monitor/usage/model-usage?startTime=..&endTime=..
  GET /api/biz/customer/getCustomerInfo                         (web JWT discovery)

Usage: python3 tools/mock_server.py [port]     (default 8791)

Point the app at it: base_url = http://127.0.0.1:8791
It also verifies the caller sends a plausible fresh JWT in Authorization.
"""
import base64
import hashlib
import hmac
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8791


def iso_in(seconds):
    """an ISO-8601 stamp `seconds` from now, +08:00 like the real API"""
    t = time.localtime(time.time() + seconds)
    return time.strftime("%Y-%m-%dT%H:%M:%S.000+08:00", t)


def check_jwt(auth):
    """decode (not verify) the API JWT and complain when stale"""
    if not auth:
        return "缺少 Authorization"
    if auth.startswith("Bearer "):
        return None  # web JWT path, fine
    parts = auth.split(".")
    if len(parts) != 3:
        return "Authorization 不是三段 JWT"
    pad = "=" * (-len(parts[1]) % 4)
    try:
        payload = json.loads(base64.urlsafe_b64decode(parts[1] + pad))
        ts = payload.get("timestamp", 0) / 1000
        if abs(time.time() - ts) > 300:
            return f"JWT timestamp 太旧（{int(time.time() - ts)} 秒前签发）"
    except Exception as exc:  # noqa: BLE001
        return f"JWT 解析失败: {exc}"
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # noqa: A003
        sys.stderr.write("[mock] %s\n" % (fmt % args))

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        url = urlparse(self.path)
        q = parse_qs(url.query)
        auth = self.headers.get("Authorization", "")
        org = self.headers.get("Bigmodel-Organization", "")
        proj = self.headers.get("Bigmodel-Project", "")

        # JWT freshness — the real API rejects stale timestamps (code 1003)
        err = check_jwt(auth)
        if err and not url.path.startswith("/api/biz/customer/getCustomerInfo"):
            self._json({"code": 1003, "message": err + "（mock 模拟 code 1003）"})
            return

        if url.path == "/api/monitor/usage/quota/limit":
            if org == "org-decoy":  # reproduce the silent-empty pitfall
                self._json({"code": 200, "data": {}})
                return
            if org and org != "org-team1":
                self._json({"code": 500, "message": "当前用户不存在coding plan"})
                return
            self._json({
                "code": 200,
                "data": {
                    "level": "pro",
                    "limits": [
                        {
                            "type": "TOKENS_LIMIT",
                            "percentage": 37.4,
                            "remaining": 5062120,
                            "nextResetTime": iso_in(2 * 3600 + 13 * 60),
                        },
                        {
                            "type": "CREDIT_LIMIT",
                            "percentage": 12.8,
                            "remaining": 41048800,
                            "nextResetTime": iso_in(4 * 86400 + 6 * 3600),
                        },
                        {
                            "type": "TIME_LIMIT",
                            "percentage": 14.2,
                            "currentValue": 213,
                            "usage": 1500,
                            "nextResetTime": iso_in(19 * 86400),
                        },
                    ],
                },
            })
        elif url.path == "/api/biz/customer-package-reset/list":
            self._json({
                "code": 200,
                "data": {
                    "fiveHourResets": [{"available": True}, {"available": True}],
                    "weekResets": [{"available": True}, {"available": False}],
                },
            })
        elif url.path == "/api/biz/account/query-org-owner-account-report":
            self._json({"code": 200, "data": {"balance": 286.40, "giveAmount": 120.00,
                                              "totalSpendAmount": 837.13}})
        elif url.path == "/api/biz/account/query-customer-account-report":
            self._json({"code": 200, "data": {"balance": 15.20, "giveAmount": 0.0,
                                              "totalSpendAmount": 16.47}})
        elif url.path == "/api/monitor/usage/model-usage":
            self._json({
                "code": 200,
                "data": {
                    "usageList": [
                        {"model": "glm-4.6", "totalTokens": 18432000, "count": 1284},
                        {"model": "glm-4.5-air", "totalTokens": 9644000, "count": 866},
                        {"model": "glm-4.5-flash", "totalTokens": 7213000, "count": 1203},
                        {"model": "glm-4.5v", "totalTokens": 2310000, "count": 97},
                        {"model": "glm-4.6-flash", "totalTokens": 867000, "count": 244},
                    ]
                },
            })
        elif url.path == "/api/biz/customer/getCustomerInfo":
            if not auth.startswith("Bearer "):
                self._json({"code": 401, "message": "JWTToken not allow access"})
                return
            self._json({
                "code": 200,
                "data": {
                    "organizations": [
                        {
                            "organizationId": "org-team1",
                            "organizationName": "团队编程套餐",
                            "projects": [{"projectId": "proj-team1", "projectName": "主项目",
                                          "projectType": 2}],
                        },
                        {
                            "organizationId": "org-personal",
                            "organizationName": "个人空间",
                            "projects": [{"projectId": "proj-personal", "projectName": "默认",
                                          "projectType": 1}],
                        },
                    ]
                },
            })
        else:
            self._json({"code": 404, "message": f"mock: unknown path {url.path}"}, code=404)


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"mock BigModel API on http://127.0.0.1:{PORT}")
    server.serve_forever()
