# Build environment (build.sh picks the platform; ARCH names it for the packaging tools)
FROM ubuntu:noble-20250529 AS dev

COPY native-setup.sh /tmp/native-setup.sh
RUN bash /tmp/native-setup.sh && rm -rf /var/lib/apt/lists/* /tmp/native-setup.sh

WORKDIR /src

# Runtime image, used from a browser through noVNC: docker build --target local -t photoc:local .
# zenity draws the open and save dialogs (SDL falls back to it when there is no desktop portal)
FROM ubuntu:noble-20250529 AS local

RUN apt-get update && apt-get install -y --no-install-recommends liblcms2-2 libgomp1 libpng16-16t64 libjpeg-turbo8 libtiff6 libwebp7 libgif7 zlib1g \
    libharfbuzz0b libfontconfig1 libfribidi0 libunibreak5 libfreetype6 \
    libgl1 libegl1 libgl1-mesa-dri libx11-6 libxext6 libxrandr2 libxcursor1 libxi6 libxss1 libxfixes3 libxkbcommon0 libdbus-1-3 \
    zenity fonts-dejavu-core tigervnc-standalone-server openbox novnc websockify
RUN rm -rf /var/lib/apt/lists/*

# PhotoC fills the screen without a title bar; Xvnc resizes the screen to the browser window
RUN sed -i 's|</applications>|<application title="PhotoC" type="normal"><decor>no</decor><maximized>yes</maximized></application>\n</applications>|' /etc/xdg/openbox/rc.xml
# The X server runs as the compose user, so it can't create its socket directory itself
RUN mkdir -m 1777 /tmp/.X11-unix

COPY docker/index.html /usr/share/novnc/index.html
COPY docker/entrypoint.sh /usr/local/bin/photoc-vnc

COPY apps/photoc /usr/local/bin/
COPY apps/photoc-cli /usr/local/bin

EXPOSE 6080
WORKDIR /local
ENTRYPOINT ["photoc-vnc"]