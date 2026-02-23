// Native test env build shim.
// This file ensures `pio run -e luce_test_native` has a source input
// without pulling ESP-IDF firmware entry points.

int __attribute__((weak)) main() { return 0; }
