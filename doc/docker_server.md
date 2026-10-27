# Docker Server

We provide Voxelyx server Docker images using the GitHub container registry.

Images are built on each commit and available using the following tag scheme:

* `ghcr.io/voxelyx/voxelyx_engine:master` (latest build)
* `ghcr.io/voxelyx/voxelyx_engine:<tag>` (specific Git tag)
* `ghcr.io/voxelyx/voxelyx_engine:latest` (latest Git tag, which is the stable release)

See [here](https://github.com/voxelyx/voxelyx_engine/pkgs/container/voxelyx_engine) for all available tags.

For a quick test you can easily run:

```shell
docker run ghcr.io/voxelyx/voxelyx_engine:master
```

To use it in a production environment, you should use volumes bound to the Docker host to persist data and modify the configuration:

```shell
docker create -v /home/voxelyx/data/:/var/lib/voxelyx/ -v /home/voxelyx/conf/:/etc/voxelyx/ ghcr.io/voxelyx/voxelyx_engine:master
```

You may also want to use [Docker Compose](https://docs.docker.com/compose):

```yaml
---
version: "2"
services:
  voxelyx_server:
    image: ghcr.io/voxelyx/voxelyx_engine:master
    restart: always
    networks:
      - default
    volumes:
      - /home/voxelyx/data/:/var/lib/voxelyx/
      - /home/voxelyx/conf/:/etc/voxelyx/
    ports:
      - "30000:30000/udp"
      - "127.0.0.1:30000:30000/tcp"
```

Data will be written to `/home/voxelyx/data` on the host, and configuration will be read from `/home/voxelyx/conf/main.conf`.

**Note:** If you don't understand the previous commands please read the [official Docker documentation](https://docs.docker.com) before use.
