#!/bin/bash
# Run the avatar container with GPU + audio + a writable socket dir.
set -e
docker run --rm -it \
  --runtime nvidia \
  --network host \
  --device /dev/snd \
  -v avatar-hf-cache:/root/.cache/huggingface \
  --tmpfs /run/user/0 \
  jetson-avatar-studio:latest
# push-to-talk page: http://<jetson-ip>:8091/
