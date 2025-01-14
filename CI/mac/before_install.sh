#!/bin/sh

brew update
brew install smpeg2 libpng freetype qt5 ffmpeg ninja boost tbb
brew install sdl2 sdl2_ttf sdl2_image sdl2_mixer

echo CMAKE_PREFIX_PATH="/usr/local/opt/qt5:$CMAKE_PREFIX_PATH" >> $GITHUB_ENV
