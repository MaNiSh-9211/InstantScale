# HotPod development image: toolchain + CRIU + crit (image decoder)
FROM gcc:14
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        criu python3-minimal python3-protobuf ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
