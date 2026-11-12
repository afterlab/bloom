# The build image of the Linux Bloom package: Ubuntu 22.04 by digest, so the package is built
# against glibc 2.35, with the locked compiler (provision.sh).
#
#   docker build -t bloom-linux-package tools/ci/linux-package
#   docker run --rm -v "$PWD:/src" -w /src bloom-linux-package sh tools/ci/linux-package/build.sh
FROM ubuntu:22.04@sha256:74b12b11ffa550fba1929ad4d8d16533c34f6612728469671afba1b03b66881c
COPY provision.sh /tmp/provision.sh
RUN sh /tmp/provision.sh && rm /tmp/provision.sh
ENV CC=clang-22 CXX=clang++-22 LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=0
