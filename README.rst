ray-haproxy
===========

Pre-built `HAProxy <https://www.haproxy.org/>`_ binary distribution for
`Ray Serve <https://docs.ray.io/en/latest/serve/index.html>`_.

The wheel bundles the HAProxy binary together with its vendored shared
libraries (libssl, libcrypto, libpcre, liblua) so that Ray Serve can use
HAProxy without requiring a system-level installation.

Installation
------------

.. code-block:: bash

   pip install "ray[serve,haproxy]"

Or directly::

   pip install ray-haproxy

Platform support
----------------

* Linux x86_64 (``manylinux_2_17_x86_64``)
* Linux arm64 (``manylinux_2_17_aarch64``)

Usage
-----

.. code-block:: python

   from ray_haproxy import get_haproxy_binary
   print(get_haproxy_binary())  # /path/to/ray_haproxy/bin/haproxy

Binary resolution order:

1. ``RAY_SERVE_HAPROXY_BINARY`` environment variable (explicit override)
2. Bundled binary inside this package
3. ``haproxy`` on the system PATH

Vendored shared libraries
-------------------------

The wheel bundles HAProxy and the shared libraries it depends on that are
not guaranteed to exist on all Linux systems. At build time, the script
``ci/build/build-haproxy-dist.sh`` compiles HAProxy inside a manylinux2014
container, then uses ``ldd`` to discover every ``.so`` the binary needs.
Libraries on the `PEP 599 manylinux2014 allowlist
<https://peps.python.org/pep-0599/>`_ (libc, libz, libpthread, etc.) are
skipped — they are guaranteed present on any compatible system. Everything
else is copied into ``lib/`` and the binary's RPATH is patched to
``$ORIGIN/lib`` so it finds its vendored copies at runtime.

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Library
     - Source
     - Why
   * - OpenSSL 3.0.x (libssl, libcrypto)
     - Built from source
     - manylinux2014 ships OpenSSL 1.1.1 (EOL Sept 2023). We build 3.0.x
       (LTS, EOL Sept 2026) to get current security patches.
   * - Lua 5.4.x
     - Built from source
     - HAProxy 2.8 requires Lua 5.3+. manylinux2014 only has Lua 5.1.
   * - PCRE (libpcre, libpcreposix)
     - manylinux2014 system package
     - Discovered by ``ldd``, not on the PEP 599 allowlist. Vendored
       automatically.
   * - libxcrypt (libcrypt.so.2)
     - manylinux2014 system package
     - Discovered by ``ldd``. ``libcrypt.so.1`` (glibc) is on the
       allowlist, but ``libcrypt.so.2`` (libxcrypt) is not. Must be
       vendored — omitting it causes the binary to fail on systems that
       only have ``libcrypt.so.1``.

Libraries **not** vendored (on the PEP 599 allowlist): libc, libm, libdl,
librt, libpthread, libz, libgcc_s, libresolv, libcrypt.so.1.

License
-------

HAProxy is distributed under the **GNU General Public License v2**.
See `LICENSE <https://github.com/haproxy/haproxy/blob/master/LICENSE>`_
for details.
