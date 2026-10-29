Vendored OpenThread Spinel codec
################################

``spinel.c`` and ``spinel.h`` come unmodified from OpenThread
``src/lib/spinel/`` at commit ``3d645eb9106cb1d5c2ff11891e84dc14e52e293a``:
https://github.com/openthread/openthread/tree/3d645eb9106cb1d5c2ff11891e84dc14e52e293a/src/lib/spinel

They are distributed under the BSD-3-Clause license in ``LICENSE``, also taken
from that commit.

Do not modify these files. To update them, re-vendor both files and
``LICENSE`` from a newer OpenThread commit and update the commit above.
Compiler warnings are silenced per file in ``subsys/spinel/CMakeLists.txt``.
