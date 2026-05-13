from pathlib import Path
import os

PI_ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = PI_ROOT / "data"
LOG_DIR = PI_ROOT / "logs"
DB_PATH = DATA_DIR / "wage_pi.sqlite3"
APP_VERSION = os.getenv("WAGE_PI_VERSION", "0.1.0")
OFFLINE_THRESHOLD_SECONDS = int(os.getenv("WAGE_PI_OFFLINE_THRESHOLD_SECONDS", "60"))

DATA_DIR.mkdir(parents=True, exist_ok=True)
LOG_DIR.mkdir(parents=True, exist_ok=True)
