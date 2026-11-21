#!/usr/bin/env python3
"""crypto_selftest.py — verify the app's C SHA-256 / HMAC / base64url / JWT
against Python's hashlib, the same way tools/glm_crypto_selftest.mjs once
checked the E6 MiniApp. Builds a tiny C harness on the fly.

Usage: python3 tools/crypto_selftest.py
Exit 0 when every vector matches.
"""
import base64
import hashlib
import hmac
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"

static void hex(const uint8_t *in, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", in[i]);
    out[n * 2] = 0;
}

int main(void) {
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char op[16], hex_in[4096], hex_key[4096];
        if (sscanf(line, "%15s %4095s %4095s", op, hex_in, hex_key) < 2) continue;
        size_t in_len = strlen(hex_in) / 2, key_len = strlen(hex_key) / 2;
        if (!strcmp(hex_in, "-")) in_len = 0;
        if (!strcmp(hex_key, "-")) key_len = 0;
        uint8_t *in = malloc(in_len ? in_len : 1), *key = malloc(key_len ? key_len : 1);
        for (size_t i = 0; i < in_len; i++) sscanf(hex_in + i * 2, "%2hhx", &in[i]);
        for (size_t i = 0; i < key_len; i++) sscanf(hex_key + i * 2, "%2hhx", &key[i]);
        uint8_t out[32];
        char hex_out[65];
        if (!strcmp(op, "sha256")) {
            glm_sha256(in, in_len, out);
            hex(out, 32, hex_out);
            printf("%s\n", hex_out);
        } else if (!strcmp(op, "hmac")) {
            glm_hmac_sha256(key, key_len, in, in_len, out);
            hex(out, 32, hex_out);
            printf("%s\n", hex_out);
        } else if (!strcmp(op, "b64")) {
            char b64[8192];
            glm_b64url_encode(in, in_len, b64, sizeof b64);
            printf("%s\n", b64);
        } else if (!strcmp(op, "jwt")) {
            char jwt[4096];
            if (!glm_jwt_mint((char *)in, 1700000000, jwt, sizeof jwt)) jwt[0] = 0;
            printf("%s\n", jwt);
        }
        free(in);
        free(key);
    }
    return 0;
}
"""

# NIST-style vectors, plus the padding edge cases the E6 selftest once covered
vectors = []


def vec_sha(data):
    vectors.append(("sha256", data, b"", hashlib.sha256(data).hexdigest()))


def vec_hmac(key, data):
    vectors.append(("hmac", data, key,
                    hmac.new(key, data, hashlib.sha256).hexdigest()))


def vec_b64(data):
    vectors.append(("b64", data, b"",
                    base64.urlsafe_b64encode(data).rstrip(b"=").decode()))


vec_sha(b"")
vec_sha(b"abc")
vec_sha(b"a" * 55)
vec_sha(b"a" * 56)
vec_sha(b"a" * 63)
vec_sha(b"a" * 64)
vec_sha(b"a" * 65)
vec_sha("中文摘要测试".encode())
vec_sha("emoji \U0001f600 padding".encode())
vec_hmac(b"secret", b"")
vec_hmac(b"", b"data")
vec_hmac(b"k" * 63, b"payload")
vec_hmac(b"k" * 64, b"payload")
vec_hmac(b"k" * 65, b"payload")
vec_hmac("密钥-secret".encode(), "中文消息 body".encode())
vec_b64(b"")
vec_b64(b"a")
vec_b64(b"ab")
vec_b64(b"abc")
vec_b64(b"abcd")
vec_b64(bytes(range(0, 256)))


def main():
    with tempfile.TemporaryDirectory() as td:
        csrc = os.path.join(td, "harness.c")
        with open(csrc, "w") as f:
            f.write(HARNESS)
        binp = os.path.join(td, "harness")
        subprocess.run(
            ["cc", "-std=c99", "-O2", "-I", SRC, csrc,
             os.path.join(SRC, "crypto.c"), "-o", binp],
            check=True,
        )

        cases = []
        expected = []
        for op, data, key, want in vectors:
            cases.append(f"{op} {data.hex() or '-'} {key.hex() or '-'}")
            expected.append(want)
        # a JWT mint case: sign with Python the same way and compare
        api_key = "0123abcd.zyxw9876"
        header = json.dumps({"alg": "HS256", "sign_type": "SIGN"}, separators=(",", ":"))
        payload = json.dumps(
            {"api_key": api_key.split(".")[0], "exp": 1700003600, "timestamp": 1700000000000},
            separators=(",", ":"),
        )
        b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
        signing = f"{b64(header.encode())}.{b64(payload.encode())}"
        sig = hmac.new(api_key.split(".")[1].encode(), signing.encode(),
                       hashlib.sha256).digest()
        cases.append(f"jwt {api_key.encode().hex()} -")
        expected.append(f"{signing}.{b64(sig)}")

        out = subprocess.run([binp], input="\n".join(cases) + "\n", capture_output=True,
                             text=True, check=True).stdout.splitlines()

        fails = 0
        for i, (op, data, key, want) in enumerate(vectors):
            got = out[i] if i < len(out) else "<missing>"
            if got != want:
                fails += 1
                print(f"FAIL [{op}] case #{i} ({data[:16]!r}…)\n  got  {got}\n  want {want}")
        # the JWT mint case
        jwt_got = out[len(vectors)] if len(out) > len(vectors) else "<missing>"
        if jwt_got != expected[-1]:
            fails += 1
            print(f"FAIL [jwt]\n  got  {jwt_got}\n  want {expected[-1]}")
        total = len(expected)
        print(f"{total} vectors, {fails} failed")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
