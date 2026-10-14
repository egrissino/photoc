# PhotoC (C/C++)

A Linux port of PhotoCraft for Ubuntu 24.04 LTS on X11. If a feature is missing then code it yourself.

## Run with Pre-built image

```sh
git pull github.com/egrissino/photoc
docker-compose up -d
```

## Build (Docker)

```sh
./build.sh
```

This builds the `photoc-dev` image from `Dockerfile`, mounts the project at `/src` in the container, then configures and builds. The output lands in `build/` on the host.

The programs:

- `build/apps/photoc [image file]`: the desktop editor (X11; on a Wayland desktop it runs through XWayland).
- `build/apps/photoc-cli`: the command-line program (run it with `--help`).

The image also builds SDL3 (static, from the `release-3.4.18` tag) and puts the Dear ImGui
`v1.92.9b-docking` sources in `/opt/imgui`, because Ubuntu 24.04 has neither.

## Runtime libraries on the target machine (N/A if running in docker vnc container)

The binaries link these shared libraries, which the target machine must have:

```sh
sudo apt-get install -y liblcms2-2 libgomp1 libpng16-16t64 libjpeg-turbo8
```

glibc, libstdc++ and libgcc are part of every Ubuntu 24.04 install, and the desktop editor's OpenGL and
X11 libraries (`libgl1`, `libx11-6`) are part of every Ubuntu desktop. SDL3 and Dear ImGui are linked
in statically.

