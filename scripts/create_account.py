"""One-time (or repeatable) tool to set the web login password.

There is exactly one account. Run locally against DB_PATH, or against
Railway's volume - and note which is which, because getting it wrong is
silent: `railway run` executes on your laptop with Railway's variables
injected, so it writes to a local file, not to /data. Inside the container:
`railway ssh -- python scripts/create_account.py --username you --password ...`
"""

import argparse
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from server.accounts import Accounts  # noqa: E402
from server.config import Settings  # noqa: E402


async def main(username: str, password: str) -> None:
    settings = Settings()
    accounts = Accounts(f"sqlite+aiosqlite:///{settings.db_path}")
    await accounts.init()
    await accounts.create_user(username, password)
    await accounts.close()
    print(f"account '{username}' set")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password", required=True)
    args = parser.parse_args()
    asyncio.run(main(args.username, args.password))
