# One image, one process: matchcore_server serves the API and the built UI.
#   docker build -t matchcore .
#   docker run --rm -p 8080:8080 matchcore      → http://localhost:8080

# ── 1. Frontend bundle ─────────────────────────────────────────────────────
FROM node:22-alpine AS web
WORKDIR /web
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci
COPY frontend/ ./
RUN npm run build

# ── 2. C++ build (GCC 13, CMake 3.28) ──────────────────────────────────────
FROM ubuntu:24.04 AS cpp
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY server/ server/
COPY test/ test/
COPY bench/ bench/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ── 3. Runtime ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04
WORKDIR /app
COPY --from=cpp /src/build/matchcore_server ./
COPY --from=web /web/dist ./public
USER ubuntu
ENV HOST=0.0.0.0 PORT=8080
EXPOSE 8080
# --trust-proxy: behind the host's load balancer, the client IP (for rate
# limiting) arrives in X-Forwarded-For rather than as the TCP peer.
# --seed: visitors land on a two-sided book instead of an empty one.
CMD ["./matchcore_server", "--static", "public", "--trust-proxy", "--seed"]
