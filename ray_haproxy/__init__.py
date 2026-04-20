"""ray-haproxy: HAProxy binary distribution for Ray Serve.

Public API
----------
get_haproxy_binary()
    Returns the path to a usable haproxy executable, applying the following
    resolution order:

    1. ``RAY_SERVE_HAPROXY_BINARY_PATH`` environment variable (explicit override).
    2. The binary bundled inside this package (``ray_haproxy/bin/haproxy``).
    3. ``haproxy`` on the system PATH.

    Raises ``FileNotFoundError`` if no usable binary is found.
"""

import os
import shutil

_BIN_DIR = os.path.join(os.path.dirname(__file__), "bin")
_BUNDLED = os.path.join(_BIN_DIR, "haproxy")


def get_haproxy_binary() -> str:
    """Return the path to a usable haproxy executable.

    Resolution order:
      1. RAY_SERVE_HAPROXY_BINARY_PATH env var
      2. Bundled binary (ray_haproxy/bin/haproxy)
      3. System PATH

    Raises:
        FileNotFoundError: if no executable haproxy binary is found.
    """
    env_path = os.environ.get("RAY_SERVE_HAPROXY_BINARY_PATH")
    if env_path is not None:
        if os.path.isfile(env_path) and os.access(env_path, os.X_OK):
            return env_path
        raise FileNotFoundError(
            f"RAY_SERVE_HAPROXY_BINARY_PATH={env_path!r} does not point to an executable file."
        )

    if os.path.isfile(_BUNDLED) and os.access(_BUNDLED, os.X_OK):
        return _BUNDLED

    system_haproxy = shutil.which("haproxy")
    if system_haproxy:
        return system_haproxy

    raise FileNotFoundError(
        "No HAProxy binary found. "
        "Install ray-haproxy, set RAY_SERVE_HAPROXY_BINARY_PATH, "
        "or ensure 'haproxy' is on PATH."
    )
