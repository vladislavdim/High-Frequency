# Single-stage on purpose: build and run in the same image so there's no
# risk of a runtime Boost/OpenSSL version mismatching whatever the build
# stage compiled against. Bigger image, zero version-skew headaches - the
# right trade for a Render Background Worker where image size barely
# matters but "it just works" matters a lot.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libssl-dev \
    libboost-all-dev \
    nlohmann-json3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# Symbol (e.g. btcusdt) is passed as the container's command-line arg.
# Firebase config comes from environment variables set in the Render
# dashboard (FIREBASE_DB_HOST, FIREBASE_AUTH_SECRET) - never baked into
# the image or committed to the repo.
ENTRYPOINT ["/app/build/sentinel_core"]
CMD ["btcusdt"]
