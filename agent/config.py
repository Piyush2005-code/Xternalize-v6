"""Shared host-side configuration for the xv6 speculative-process demo."""

from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parent
XV6_ROOT = AGENT_DIR.parent

QEMU_COMMAND = ("make", "qemu-bridge")
QEMU_CWD = XV6_ROOT
SERIAL_HOST = "127.0.0.1"
SERIAL_PORT = 5555

GUEST_COMMANDS = {
    "begin_wrapper": "xbegin",
    "begin": "specsh",
    "commit": "xcommit",
    "abort": "xabort",
    "preview": "xpreview",
    "list_files": "ls {path}",
    "read_file": "cat {path}",
    "write_file": "echo {text} > {path}",
    "delete_file": "rm {path}",
}

ALLOWED_GUEST_ROOT = "/"
DEMO_DELETE_PATH = "/a.txt"
DEMO_WRITE_PATH = "/b.txt"
INBOX_PATH = "/inbox.txt"
IMPORTANT_PATH = "/important.txt"

MAX_PATH_LENGTH = 128
MAX_TEXT_LENGTH = 512
DEFAULT_COMMAND_TIMEOUT = 10.0
BOOT_TIMEOUT = 120.0
