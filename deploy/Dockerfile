# HotPod production image (multi-stage)
# Build:  docker build -f deploy/Dockerfile -t ghcr.io/manish-9211/hotpod:latest .
# Push:   docker push ghcr.io/manish-9211/hotpod:latest
#
# Runtime contents: seeder, pageserver, demo_app binaries only.
# Orchestrator scripts are a dev-flow concern and stay out of prod images.

FROM gcc:14 AS build
WORKDIR /src
COPY phase2/ phase2/
COPY phase3/ phase3/
RUN make -C phase2 clean all && make -C phase3 clean all

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libstdc++6 ca-certificates tini \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 hotpod
COPY --from=build /src/phase2/seeder     /usr/local/bin/seeder
COPY --from=build /src/phase2/pageserver /usr/local/bin/pageserver
COPY --from=build /src/phase3/demo_app   /usr/local/bin/demo_app
USER hotpod
EXPOSE 46100
ENTRYPOINT ["/usr/bin/tini", "--"]
# Default: serve a checkpoint mounted at /data/app.isim
#   docker run -v $PWD/app.isim:/data/app.isim -e HOTPOD_TOKEN=... \
#     hotpod pageserver --port 46100 --image /data/app.isim
CMD ["pageserver", "--port", "46100", "--image", "/data/app.isim"]
