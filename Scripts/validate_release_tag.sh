#!/bin/bash
set -euo pipefail

release_tag=${1:?Release tag required}
built_commit=$(git rev-parse --verify "${2:?Built commit required}^{commit}")
git check-ref-format "refs/tags/$release_tag"

status=0
remote_tag=$(git ls-remote --exit-code --tags origin "refs/tags/$release_tag" "refs/tags/$release_tag^{}") || status=$?
if (( status == 2 )); then
    exit 0 # The release will create this tag at the built commit.
elif (( status != 0 )); then
    exit "$status"
fi

tag_object=""
tag_commit=""
while read -r object ref; do
    if [[ "$ref" == "refs/tags/$release_tag^{}" ]]; then
        tag_commit=$object
    elif [[ "$ref" == "refs/tags/$release_tag" ]]; then
        tag_object=$object
    fi
done <<< "$remote_tag"
tag_commit=${tag_commit:-$tag_object}
if [[ "$tag_commit" != "$built_commit" ]]; then
    echo "Release tag '$release_tag' points to $tag_commit, but the binaries were built from $built_commit." >&2
    echo "Select the matching tag when running this workflow, or use a new release tag." >&2
    exit 1
fi
