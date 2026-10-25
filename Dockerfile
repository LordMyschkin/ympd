FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make pkgconf libmpdclient-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j4

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmpdclient2t64 libssl3t64 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/ympd /usr/bin/ympd
COPY --from=build /src/build/ashuffle /usr/bin/ashuffle
EXPOSE 6080
ENTRYPOINT ["/usr/bin/ympd"]
