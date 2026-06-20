# Contract: CI Publish Pipelines

Two credential-gated pipelines build the same `Dockerfile` (the dev-container image) and publish it. Neither affects local (non-CI) use; both no-op when their credentials are absent (FR-037, SC-011).

## Cross-platform verification (`.github/workflows/ci.yml`)

The substitute for manual Windows testing (the maintainer is on macOS and cannot test Windows). Runs on every push/PR.

| Aspect | Contract |
|--------|----------|
| Matrix | `windows-latest`, `macos-latest`, `ubuntu-latest` |
| Steps | checkout (with submodules) → run the host-native bootstrap (`python scripts/setup.py`, identical on every OS) → `badge build <example>` |
| Pass condition | toolchain provisions and the example `.wasm` builds on all three OSes (SC-013) |
| Hardware steps | flash/serial are not run in CI; their non-hardware portions (arg parsing, build, port enumeration) run where possible, the rest is marked manual (FR-040) |
| Gating | always runs; no credentials required (independent of the publish pipelines) |

## GitHub Actions → Docker Hub (`.github/workflows/publish-image.yml`)

| Aspect | Contract |
|--------|----------|
| Trigger | push to default branch + tags; `workflow_dispatch` |
| Auth | `docker/login-action` with secrets `DOCKERHUB_USERNAME`, `DOCKERHUB_TOKEN` |
| Image repo | `krim404/cdc-badge-development` on Docker Hub |
| Build/push | `docker/build-push-action`, tags `:latest` and `:<git-sha>` (and `:<tag>` on releases) |
| Gating | step/job skipped (or short-circuits) when the Docker Hub secrets are not configured |
| Image | the repo `Dockerfile` (Rust + `wasm32` + pinned `wasm-opt` + python venv) |

## GitLab CI → registry.krim.dev (`.gitlab-ci.yml`)

Modelled on `~/GIT/selkies-gpu/.gitlab-ci.yml`.

| Aspect | Contract |
|--------|----------|
| Stages | `push`, `sign` |
| Image | `docker:27` |
| Vars | `IMAGE_NAME = $HARBOR_HOST/library/$CI_PROJECT_NAME` = `registry.krim.dev/library/cdc-badge-development`, `HARBOR_HOST = registry.krim.dev` |
| Auth | `docker login -u $HARBOR_USERNAME -p $HARBOR_PASSWORD $HARBOR_HOST` |
| Build/push | `docker buildx build -t $IMAGE_NAME:$CI_COMMIT_REF_SLUG -t $IMAGE_NAME:latest --metadata-file metadata.json --push .` |
| Sign (optional) | `cosign sign` on the resolved `$IMAGE_DIGEST` using `$COSIGN_PRIVATE_KEY` |
| Rule | `if: $CI_COMMIT_BRANCH == $CI_DEFAULT_BRANCH` |
| Gating | runs only when `HARBOR_*` CI variables are configured |

## Shared invariants

- Both consume the same `Dockerfile`; the published image is what `.devcontainer/devcontainer.json` pulls for the container alternative.
- Version pins inside the image (Rust toolchain, Binaryen `wasm-opt`, `pyserial`) match the host-native bootstrap so container and host builds are equivalent (FR-005).
- Confirmed at implementation time: Docker Hub repo namespace, the `registry.krim.dev` project path, and whether cosign signing is enabled for this image.
