# The Decky CLI and Plugin Store build the helper in this image so it links against SteamOS's libraries.
FROM ghcr.io/steamdeckhomebrew/holo-base:latest

RUN pacman -Sy --noconfirm --needed gcc make pkgconf libpipewire libx11 libxext libxrender \
        libxcomposite libxdamage libxfixes \
    && pacman -Scc --noconfirm

ENTRYPOINT [ "/backend/entrypoint.sh" ]
