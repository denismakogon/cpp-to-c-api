#!/usr/bin/env bash

set -xe

/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

brew install gcc
brew install opencv
brew install dlib
brew install pkg-config

pkg-config --modversion opencv4
pkg-config --modversion dlib-1
