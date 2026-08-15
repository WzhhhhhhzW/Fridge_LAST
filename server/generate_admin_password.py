#!/usr/bin/env python3
"""Generate a PBKDF2 value for DASHBOARD_ADMIN_PASSWORD_HASH."""

from __future__ import annotations

import base64
import getpass
import hashlib
import secrets


ROUNDS = 600_000


def encoded(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def main() -> None:
    password = getpass.getpass("请输入新的网页管理员密码（至少10位）: ")
    if len(password) < 10:
        raise SystemExit("错误：管理员密码至少需要10个字符")
    confirmation = getpass.getpass("请再次输入密码: ")
    if password != confirmation:
        raise SystemExit("错误：两次输入的密码不一致")
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, ROUNDS)
    print(f"pbkdf2_sha256${ROUNDS}${encoded(salt)}${encoded(digest)}")


if __name__ == "__main__":
    main()
