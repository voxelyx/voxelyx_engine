# syntax=docker/dockerfile:1
# check=error=true

ARG DOCKER_IMAGE=alpine:3.24
FROM $DOCKER_IMAGE AS dev

ENV LUAJIT_VERSION=v2.1

RUN apk add --no-cache git build-base cmake curl-dev zlib-dev zstd-dev \
		sqlite-dev postgresql-dev hiredis-dev leveldb-dev \
		gmp-dev jsoncpp-dev ninja

WORKDIR /usr/src/

ADD https://github.com/jupp0r/prometheus-cpp.git#b9366fc3b28292e553ce4820c8929287cdf8e04d /usr/src/prometheus-cpp
ADD https://github.com/libspatialindex/libspatialindex.git?branch=main /usr/src/libspatialindex
ADD --keep-git-dir https://luajit.org/git/luajit.git?branch=${LUAJIT_VERSION} /usr/src/luajit

RUN cd prometheus-cpp && \
		cmake -B build \
			-DCMAKE_INSTALL_PREFIX=/usr/local \
			-DCMAKE_BUILD_TYPE=Release \
			-DENABLE_TESTING=0 \
			-GNinja && \
		cmake --build build && \
		cmake --install build && \
		cd /usr/src/ && \
	cd libspatialindex && \
		cmake -B build \
			-DCMAKE_INSTALL_PREFIX=/usr/local && \
		cmake --build build && \
		cmake --install build && \
		cd /usr/src/ && \
	cd luajit && \
		make amalg && make install && \
	cd /usr/src/

FROM dev AS builder

COPY .git /usr/src/voxelyx/.git
COPY CMakeLists.txt /usr/src/voxelyx/CMakeLists.txt
COPY README.md /usr/src/voxelyx/README.md
COPY main.conf.example /usr/src/voxelyx/main.conf.example
COPY builtin /usr/src/voxelyx/builtin
COPY cmake /usr/src/voxelyx/cmake
COPY doc /usr/src/voxelyx/doc
COPY fonts /usr/src/voxelyx/fonts
COPY lib /usr/src/voxelyx/lib
COPY misc /usr/src/voxelyx/misc
COPY po /usr/src/voxelyx/po
COPY src /usr/src/voxelyx/src
COPY irr /usr/src/voxelyx/irr
COPY textures /usr/src/voxelyx/textures

WORKDIR /usr/src/voxelyx
RUN cmake -B build \
		-DCMAKE_INSTALL_PREFIX=/usr/local \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SERVER=TRUE \
		-DENABLE_PROMETHEUS=TRUE \
		-DBUILD_UNITTESTS=FALSE -DBUILD_BENCHMARKS=FALSE \
		-DBUILD_CLIENT=FALSE \
		-GNinja && \
	cmake --build build && \
	cmake --install build

FROM $DOCKER_IMAGE AS runtime

RUN apk add --no-cache curl gmp libstdc++ libgcc libpq jsoncpp zstd-libs \
				sqlite-libs postgresql hiredis leveldb && \
	adduser -D voxelyx --uid 30000 -h /var/lib/voxelyx && \
	chown -R voxelyx:voxelyx /var/lib/voxelyx

WORKDIR /var/lib/voxelyx

COPY --from=builder /usr/local/share/voxelyx /usr/local/share/voxelyx
COPY --from=builder /usr/local/bin/voxelyxserver /usr/local/bin/voxelyxserver
COPY --from=builder /usr/local/share/doc/voxelyx/main.conf.example /etc/voxelyx/main.conf
COPY --from=builder /usr/local/lib/libspatialindex* /usr/local/lib/
COPY --from=builder /usr/local/lib/libluajit* /usr/local/lib/
USER voxelyx:voxelyx

EXPOSE 30000/udp 30000/tcp
VOLUME /var/lib/voxelyx/ /etc/voxelyx/

ENTRYPOINT ["/usr/local/bin/voxelyxserver"]
CMD ["--config", "/etc/voxelyx/main.conf"]
