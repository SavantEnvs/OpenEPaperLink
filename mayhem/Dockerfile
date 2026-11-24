# syntax=docker/dockerfile:1.7
#
# Commit image for OpenEPaperLink (firmware; the fuzz targets compile its host-buildable sources).
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile --build-arg COMMIT=$(git rev-parse HEAD) -t oepl .
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
ARG SOURCE_DATE_EPOCH
ARG SANITIZER_FLAGS
ARG COVERAGE_FLAGS=
ARG MAYHEM_JOBS=

# TinyTTF, the AP's TrueType renderer (ESP32_AP-Flasher/platformio.ini lib_deps
# https://github.com/nlimper/TinyTTF#6437932), pinned by commit + sha256. Source only: build.sh
# compiles it uninstrumented into the known-answer self test (it is an external library, not fuzzed).
USER root
ARG TINYTTF_COMMIT=6437932114e040fa2de4c544093666fd39a4081a
ARG TINYTTF_SHA256=8a43a2417fa31b17fc86237cd3d2a411b9db8381bb548301e1fe0f708df50ff4
RUN d=/opt/toolchains/deps/tinyttf-6437932 && mkdir -p "$d/src" \
 && curl -fsSL -o /tmp/tinyttf.tgz https://github.com/nlimper/TinyTTF/archive/${TINYTTF_COMMIT}.tar.gz \
 && echo "${TINYTTF_SHA256}  /tmp/tinyttf.tgz" | sha256sum -c - \
 && tar -xzf /tmp/tinyttf.tgz --strip-components=1 -C "$d/src" \
 && rm -f /tmp/tinyttf.tgz && chmod -R a+rX,go-w "$d"

USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
