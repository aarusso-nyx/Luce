Import("env")

# Apply RTTI stripping only to C++ units to avoid noisy C compiler warnings.
env.Append(CXXFLAGS=["-fno-rtti"])
