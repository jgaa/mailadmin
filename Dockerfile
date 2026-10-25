FROM debian:13 AS build
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    libboost-program-options-dev libboost-context-dev \
    libboost-coroutine-dev libboost-chrono-dev \
    libboost-json-dev libboost-url-dev libssl-dev libcrypt-dev libcurl4-openssl-dev zlib1g-dev libmariadb-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY api-server/ api-server/
COPY tests/security.cpp tests/async.cpp tests/mailstore.cpp tests/dns.cpp tests/
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMAILADMIN_BUILD_TESTS=ON -DMAILADMIN_BUILD_UI=OFF \
    && cmake --build /build --target mailadmin mailadmin_security_test mailadmin_async_test mailadmin_mailstore_test mailadmin_dns_test --parallel 2 \
    && ctest --test-dir /build --output-on-failure
# Copy only the application, not dependency headers, tests or static libraries.
RUN mkdir /artifact && cp /build/api-server/mailadmin /artifact/mailadmin

FROM debian:13-slim
RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    ca-certificates libboost-program-options1.83.0 libboost-context1.83.0 \
    libboost-coroutine1.83.0 libboost-chrono1.83.0 libboost-json1.83.0 \
    libboost-url1.83.0 libmariadb3 libssl3t64 libcrypt1 libcurl4t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /artifact/mailadmin /usr/local/bin/mailadmin
USER 107:113
ENTRYPOINT ["/usr/local/bin/mailadmin"]
CMD ["serve", "--config", "/etc/mailadmin/config.json"]
