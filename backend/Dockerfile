# The Decky CLI and Plugin Store build the helper in this image so it links against SteamOS's libraries.
FROM ghcr.io/steamdeckhomebrew/holo-base:latest

# holo-base is built from a SteamOS image without headers; reinstalling restores them.
RUN pacman -Syu --noconfirm --needed gcc make pkgconf \
    && pacman -S --noconfirm libpipewire xorgproto libx11 libxext libxrender libxcomposite libxdamage libxfixes \
    && pacman -Scc --noconfirm

ENTRYPOINT [ "/backend/entrypoint.sh" ]
