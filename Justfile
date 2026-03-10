set shell := ["bash", "-cu"]

default:
    @just --list

web-install:
    cd web && npm install

web-dev:
    cd web && npm run dev

web-lint:
    cd web && npm run lint

web-test:
    cd web && npm test

web-typecheck:
    cd web && npm run typecheck

web-build:
    cd web && npm run build

generate-types:
    cd web && npm run generate:types

build-all:
    just web-build
    just build

setup:
    pio pkg install -d .

setup-dev:
    uv sync

build:
    pio run

upload:
    pio run -t upload

clean:
    pio run -t clean

test:
    uv run python3 -m pytest -q

test-cpp:
    pio test -e native
    pio test -e native_parser

coverage-cpp:
    pio test -e native
    pio test -e native_parser
    uv run python3 -m gcovr --root . --txt --print-summary \
        --fail-under-line 35 \
        --filter 'src/protocols/.*\.c' \
        --filter 'src/modules/.*_logic\.cpp' \
        --filter 'src/hal/.*_logic\.cpp' \
        --xml-pretty --xml coverage-native.xml

lint:
    pre-commit run --all-files

analyze-cpp:
    cppcheck --quiet --error-exitcode=1 \
        --enable=warning,performance \
        --inline-suppr --suppress=missingIncludeSystem \
        -I src -I src/protocols \
        src/app src/hal src/modules src/protocols src/storage src/web

quality:
    just lint
    just test
    just coverage-cpp
    just analyze-cpp
    just web-lint
    just web-test
    just web-typecheck

erase:
    uv run python scripts/flash.py --erase

flash:
    uv run python scripts/flash.py

flash-build:
    uv run python scripts/flash.py --build -y

flash-release tag="":
    uv run python scripts/flash.py --release {{tag}} -y

flash-batch:
    uv run python scripts/flash.py --batch

monitor *args="":
    uv run python scripts/monitor.py {{args}}

# ---------------------------------------------------------------------------
# Docker targets — run inside the devcontainer image, no local env needed
# ---------------------------------------------------------------------------

docker_image := "ghcr.io/dougborg/oui-spy-unified-blue/devcontainer:latest"

# Run a command inside the devcontainer image with the repo mounted
_docker-run +cmd:
    docker run --rm --init \
        -v "{{justfile_directory()}}:/workspaces/oui-spy-unified-blue" \
        -w /workspaces/oui-spy-unified-blue \
        -e PLATFORMIO_CORE_DIR=/home/vscode/.platformio \
        -u vscode \
        {{docker_image}} \
        bash -c 'git config --global --add safe.directory /workspaces/oui-spy-unified-blue && {{cmd}}'

docker-setup:
    just _docker-run 'pio pkg install -d . && cd web && npm ci && npm run generate:types'

docker-build:
    just _docker-run 'just build'

docker-build-all:
    just _docker-run 'just build-all'

docker-test:
    just _docker-run 'just test'

docker-test-cpp:
    just _docker-run 'just test-cpp'

docker-coverage-cpp:
    just _docker-run 'just coverage-cpp'

docker-lint:
    just _docker-run 'just lint'

docker-analyze-cpp:
    just _docker-run 'just analyze-cpp'

docker-web-lint:
    just _docker-run 'just web-lint'

docker-web-test:
    just _docker-run 'just web-test'

docker-web-typecheck:
    just _docker-run 'just web-typecheck'

docker-web-build:
    just _docker-run 'just web-build'

docker-quality:
    just _docker-run 'just quality'

devcontainer-auto:
    bash .devcontainer/select-profile.sh auto

devcontainer-portable:
    bash .devcontainer/select-profile.sh portable

devcontainer-linux-usb:
    bash .devcontainer/select-profile.sh linux-usb
