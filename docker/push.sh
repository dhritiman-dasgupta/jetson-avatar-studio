#!/bin/bash
# Tag + push the image to a FREE registry.
# Free options (public repos are free + unlimited on all of these):
#   - GitHub Container Registry:  ghcr.io/<your-gh-username>/jetson-avatar-studio
#       login:  echo <PAT-with-write:packages> | docker login ghcr.io -u <gh-username> --password-stdin
#   - Docker Hub:                 docker.io/<your-dockerhub-user>/jetson-avatar-studio
#       login:  docker login
#   - GitLab registry:           registry.gitlab.com/<user>/<project>
set -e
REGISTRY="${REGISTRY:-ghcr.io/CHANGE_ME/jetson-avatar-studio}"   # <-- edit or pass REGISTRY=...
TAG="${TAG:-latest}"
if [[ "$REGISTRY" == *CHANGE_ME* ]]; then echo "Edit REGISTRY in this script (or pass REGISTRY=...) first."; exit 1; fi
docker tag jetson-avatar-studio:latest "$REGISTRY:$TAG"
docker push "$REGISTRY:$TAG"
echo "pushed $REGISTRY:$TAG"
