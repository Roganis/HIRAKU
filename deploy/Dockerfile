# syntax=docker/dockerfile:1
# SPDX-License-Identifier: GPL-3.0-or-later
# Not built here: first built by the workflow (.github/workflows/server.yml), which builds it and
# runs it until its health check passes; it is pushed to no registry.
#
# hiraku-server in a container (docs/SERVER.md, "As a container"). From the checkout's top folder:
#
#   docker build -f deploy/Dockerfile -t hiraku-server .
#   docker run -d --name hiraku-server -v hiraku-data:/data \
#     -p 8080:8080 -p 127.0.0.1:8081:8081 hiraku-server
#   docker logs hiraku-server        (the first admin's password, printed once)
#
# The games' port (8080) on every address, the API and web pages (8081) on the host's loopback only,
# for a reverse proxy's HTTPS (deploy/Caddyfile.example). The database, its key and cabinet files
# in the volume /data. The log on standard output (docker logs).

# --- the build: only hiraku-server, with only what it needs (no SDL3, Qt, FFmpeg...) ---
FROM debian:12 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build zlib1g-dev libzstd-dev libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# the commit, which the program says it was built from (cmake/build_id.cmake; no .git in here)
ARG GITHUB_SHA=
ENV GITHUB_SHA=${GITHUB_SHA}
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHK_TESTS=OFF \
      -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
 && ninja -C /build hiraku-server \
 && strip /build/hiraku-server

# --- the image: the program, its web pages, the system's SQLite, a user of its own ---
FROM debian:12-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsqlite3-0 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --user-group --home-dir /data --shell /usr/sbin/nologin hiraku-server \
 && mkdir -p /data && chown hiraku-server:hiraku-server /data && chmod 0750 /data
COPY --from=build /build/hiraku-server /opt/hiraku-server/bin/hiraku-server
COPY --from=build /src/web /opt/hiraku-server/share/hiraku-server/web
COPY --from=build /src/docs/SERVER.md /src/docs/SERVER-API.md /src/docs/openapi.yaml /src/docs/openapi.json /src/LICENSE /opt/hiraku-server/share/doc/hiraku-server/
USER hiraku-server
WORKDIR /data
VOLUME /data
EXPOSE 8080 8081
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD ["/opt/hiraku-server/bin/hiraku-server", "health", "--api", "127.0.0.1:8081"]
ENTRYPOINT ["/opt/hiraku-server/bin/hiraku-server"]
# The API on every address inside the container, so that the host's port reaches it: publish it on
# the host's 127.0.0.1 only (above). Behind a proxy of your own, add --api-trust-proxy.
CMD ["serve", "--db", "/data/server.db", "--port", "8080", "--bind", "0.0.0.0", "--api", "0.0.0.0:8081"]
