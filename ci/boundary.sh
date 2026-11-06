#!/bin/sh
# The publish boundary, checked inside the public repository.
#
# The working repository mixes receipts with the deep moat. This tree is the
# receipt half. If the moat ever appears here, the mirror that produced this
# tree was wrong, and a green build would hide it. Fail instead.
set -eu

fail=0

for path in drivers bvlibs tifa bms; do
    if [ -e "$path" ]; then
        echo "::error::$path must not exist in the public toolchain tree"
        fail=1
    fi
done

# bradsense.c is the M1 temporal core plus the object-class -> expert table.
# Match the source name and the private header, not the word "BradSense" --
# the public site's claims are allowed to name the product.
hits=$(grep -rIl --exclude-dir=.git --exclude-dir=build --exclude=boundary.sh \
        -e 'bradsense\.c' -e 'brad/bradsense\.h' . || true)
if [ -n "$hits" ]; then
    echo "::error::deep-moat source referenced in the public tree:"
    echo "$hits"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "ok: no driver layer, no BradSense implementation, no TIFA SDK"
fi
exit "$fail"
