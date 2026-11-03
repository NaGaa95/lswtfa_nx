Run `CC=gcc python3 tests/run_controller_tests.py` with a host C compiler.

The tests cover every mapped button, simultaneous presses, independent stick
axes, axis limits, diagonals, D-pad movement, clearing released inputs, the
ARM64 input-device layout, preserving the engine's input metadata, fallback
for other device layouts, and the actual relocation installer compiled with
a synthetic loader image.

The supported game library has 24 input channels, 24-byte value records, a
channel count at device offset 16, and a value-array pointer at offset 24.
The hook supplies raw values during `fnInput_Poll`; the original engine poll
then applies deadzones and detects button transitions.

Run `CC=gcc python3 tests/run_shop_tests.py` to check the offline content hook.
It compiles the actual installer and JNI boolean handler, checking the native
call order, character indices and ownership flags, red-brick purchases, ship
catalog bounds, profile reload, asset-refresh and save flags, atomic relocation
patching, unsupported-library guards and completed billing query.
