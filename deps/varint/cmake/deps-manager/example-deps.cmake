# deps.cmake - Example Dependency Configuration
# ==============================================
#
# Copy this file to your project's deps/ directory and customize.
# This demonstrates various dependency declaration patterns.

# Basic dependency - just git URL and tag
deps_add(varint
    GIT https://github.com/mattsta/varint.git
    TAG v1.0.0
    TARGETS varint-static varintDimension-static
)

# Dependency with options to disable tests/examples
deps_add(json
    GIT https://github.com/nlohmann/json.git
    TAG v3.11.3
    TARGETS nlohmann_json
    OPTIONS
        JSON_BuildTests=OFF
        JSON_Install=OFF
)

# Dependency with CMakeLists.txt in subdirectory
deps_add(googletest
    GIT https://github.com/google/googletest.git
    TAG v1.14.0
    TARGETS gtest gtest_main gmock
    OPTIONS
        BUILD_GMOCK=ON
        INSTALL_GTEST=OFF
)

# Force local-only dependency (will fail if not found locally)
deps_add(my_private_lib
    GIT git@github.com:myorg/private-lib.git
    TAG main
    MODE local
    TARGETS private_lib
)

# Dependency with explicit local path override
deps_add(experimental_lib
    GIT https://github.com/example/experimental.git
    TAG develop
    LOCAL_PATH /home/dev/repos/experimental
    TARGETS exp_lib
)

# Logging library with multiple targets
deps_add(spdlog
    GIT https://github.com/gabime/spdlog.git
    TAG v1.12.0
    TARGETS spdlog spdlog_header_only
    OPTIONS
        SPDLOG_BUILD_EXAMPLE=OFF
        SPDLOG_BUILD_TESTS=OFF
)

# Header-only library
deps_add(fmt
    GIT https://github.com/fmtlib/fmt.git
    TAG 10.1.1
    TARGETS fmt fmt-header-only
    OPTIONS
        FMT_TEST=OFF
        FMT_DOC=OFF
)

# Cryptography library
deps_add(openssl
    GIT https://github.com/openssl/openssl.git
    TAG openssl-3.1.4
    TARGETS ssl crypto
    # Note: OpenSSL has complex build - may need special handling
)
