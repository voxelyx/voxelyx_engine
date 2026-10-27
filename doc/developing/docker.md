# Developing voxelyxserver with Docker

Docker provides an easy cross-platform solution to create a development environment. The advantage of this method is that it does not require you to install any development tools and libraries on your machine besides git and docker. This is a short guide describing how to set up a development environment for voxelyxserver; people unfamiliar with docker may also want to refer to the [Docker Documentation](https://docs.docker.com//) which is well written.

## Creating an image

The first step is to create a development image of voxelyxserver:
```bash
docker buildx build --target dev -t voxelyx-dev:0 .
```
The `--target dev` option instructs docker to build the intermediate development image, and the `-t voxelyx-dev:0` option tags the image so we can refer to it by that name.

## Opening a new container

Once an image has been created, a new container can be opened, with the source code mounted into it:
```bash
docker run -it \
  --mount type=bind,source=/home/bob/voxelyx_engine,target=/voxelyx_engine \
  voxelyx-dev:0
```
The `-it` runs the container interactively and puts you into a terminal in the container. The source and target of the bind mount should point to the directory on the host machine, and the directory in the container, respectively.
