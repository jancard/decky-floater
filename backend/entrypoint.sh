#!/bin/sh
# The Decky CLI mounts the plugin's bin/ at /backend/out.
set -e
cd /backend
make
