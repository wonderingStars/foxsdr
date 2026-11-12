# FoxSDR headless engine (foxsdr-engine) with its web page, as a container image.
#
# NEVER BUILT. Written on a Windows PC with no Docker and no Linux; the first build is the manual-dispatch
# workflow .github/workflows/packaging.yml (job "docker"). Package names below are Ubuntu 24.04's from
# memory of its naming (the t64 suffixes) and are UNVERIFIED: that job's `ldd` check lists any library
# the engine needs that the runtime stage lacks.
#
# BUILD (from the repository root, so the whole tree is the context):
#   docker build -f packaging/docker/Dockerfile -t foxsdr-engine .
# RUN. The web page needs a password and the engine refuses to start without one, so there is no
# default: put the password in a file and mount it (never on the command line - docs/headless-service.md).
#   docker run --rm -p 127.0.0.1:8073:8073 -v foxsdr-data:/data \
#     -v ./web.pw:/run/secrets/foxsdr_web_password:ro foxsdr-engine
# A USB radio is passed through with --device /dev/bus/usb (plus the host's udev rules, see
# installer/linux/README.md); nothing about a radio is proven inside a container.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config ca-certificates \
        libsoapysdr-dev libssl-dev libunwind-dev libasound2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# CASCADE_BUILD_INTERFACE=OFF: the engine only - no window, so no OpenGL, GLFW or X11 packages.
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_BUILD_INTERFACE=OFF \
    && cmake --build /build --target foxsdr-engine -j"$(nproc)"

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsoapysdr0.8 libssl3t64 libunwind8 libasound2t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /data --shell /usr/sbin/nologin foxsdr
# The engine reads resources/bandplans and resources/firmware from BESIDE itself.
COPY --from=build /build/foxsdr-engine /opt/foxsdr/foxsdr-engine
COPY --from=build /src/resources/bandplans /opt/foxsdr/resources/bandplans
COPY --from=build /src/resources/firmware /opt/foxsdr/resources/firmware
USER foxsdr
VOLUME /data
EXPOSE 8073
ENTRYPOINT ["/opt/foxsdr/foxsdr-engine", "--config", "/data/config.json"]
CMD ["--web-enable", "--web-bind", "0.0.0.0", "--web-port", "8073", "--web-password-file", "/run/secrets/foxsdr_web_password"]
