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

License
-------

HAProxy is distributed under the **GNU General Public License v2**.
See `LICENSE <https://github.com/haproxy/haproxy/blob/master/LICENSE>`_
for details.
