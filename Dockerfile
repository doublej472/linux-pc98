FROM debian:13-slim

ARG DEBIAN_FRONTEND=noninteractive
ARG HOST_UID=1000
ARG HOST_GID=1000

COPY scripts/setup-packages.txt /tmp/setup-packages.txt
RUN sed -e 's/[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' \
        /tmp/setup-packages.txt > /tmp/packages.txt \
    && apt-get update \
    && xargs -r apt-get install --yes --no-install-recommends \
        < /tmp/packages.txt \
    && rm -rf /var/lib/apt/lists/* /tmp/packages.txt /tmp/setup-packages.txt \
    && groupadd --gid "${HOST_GID}" builder \
    && useradd --uid "${HOST_UID}" --gid "${HOST_GID}" \
        --create-home --shell /bin/bash builder \
    && printf 'builder ALL=(ALL) NOPASSWD: ALL\n' > /etc/sudoers.d/builder \
    && chmod 0440 /etc/sudoers.d/builder

# zig is needed by ./build.sh tools to cross-build the static i486 musl
# pc98snd player (the host's 32-bit glibc is SSE2-built and SIGILLs on
# pre-SSE PC-98 CPUs).  zig is not packaged by Debian, so fetch the
# official tarball.
ARG ZIG_VERSION=0.16.0
RUN curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" \
        | tar -xJ -C /opt \
    && ln -s "/opt/zig-x86_64-linux-${ZIG_VERSION}/zig" /usr/local/bin/zig

WORKDIR /work/linux-pc98
VOLUME ["/work/linux-pc98"]
USER builder
CMD ["./build.sh", "--help"]
